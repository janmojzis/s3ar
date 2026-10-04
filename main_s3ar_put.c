/* SPDX-License-Identifier: MIT-0 */
#include "s3ar_io.h"
#include "main.h"
#include "s3ar_client.h"
#include "s3ar_config.h"
#include "s3ar_interrupt.h"
#include "log.h"
#include "s3ar_log.h"
#include "sig.h"

#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(FILE *stream) {
    log_usage(stream, "Usage: s3ar-put [-v|-vv] [-f FILE] "
                      "[--create-bucket] [--multipart-size SIZE] "
                      "s3://BUCKET/KEY\n"
                      "Read FILE, or standard input, and upload it using S3 "
                      "multipart upload.\n");
}

static const char *fn = NULL;
static int fd = -1;
static struct s3_uri_buffer uri;
static struct s3ar_config_env config;
static struct s3_client *client = NULL;
static struct s3_error error = {0};
static enum s3_result result;
static volatile sig_atomic_t interrupted_signal;

static void handle_interrupt(int signal_number) {
    interrupted_signal = signal_number;
}
static size_t multipart_size = S3_MULTIPART_PART_SIZE;
static bool multipart_size_seen = false;
static bool create_bucket = false;
static int option;

enum { OPTION_MULTIPART_SIZE = 256, OPTION_CREATE_BUCKET };
static const struct option long_options[] = {
    {"file", required_argument, NULL, 'f'},
    {"create-bucket", no_argument, NULL, OPTION_CREATE_BUCKET},
    {"multipart-size", required_argument, NULL, OPTION_MULTIPART_SIZE},
    {"verbose", no_argument, NULL, 'v'},
    {"help", no_argument, NULL, 'h'},
    {NULL, 0, NULL, 0},
};

static _Noreturn void die(int status) {
    if (status == 0 && interrupted_signal != 0) {
        log_f1("interrupted");
        status = 2;
    }
    s3_client_close(client);
    s3ar_config_free(&config);
    if (fn != NULL && fd >= 0 && close(fd) != 0) {
        if (status == 0) log_f4("cannot close ", fn, ": ", log_errno());
        status = 2;
    }
    exit(status);
}

int main_s3ar_put(int argc, char **argv) {
    int verbosity = 0;
    log_set_name("s3ar-put");

    /* ignore SIGPIPE */
    sig_ignore(SIGPIPE);
    sig_catch(SIGINT, handle_interrupt);
    sig_catch(SIGTERM, handle_interrupt);

    /* adjust verbosity using signals */
    sig_catch(SIGUSR1, log_inc_level);
    sig_catch(SIGUSR2, log_dec_level);

    /* parse options */
    opterr = 0;
    while ((option = getopt_long(argc, argv, "f:vh", long_options, NULL)) !=
           -1) {
        if (option == 'f') {
            if (fn != NULL) {
                log_f1("input file specified twice");
                die(2);
            }
            fn = optarg;
        }
        else if (option == OPTION_CREATE_BUCKET) { create_bucket = true; }
        else if (option == OPTION_MULTIPART_SIZE) {
            if (multipart_size_seen) {
                log_f1("multipart size specified twice");
                die(2);
            }
            multipart_size_seen = true;
            if (!s3ar_parse_multipart_size(optarg, &multipart_size)) {
                log_f1("--multipart-size must be between 5M and 5G");
                die(2);
            }
        }
        else if (option == 'v') {
            if (verbosity < 3) ++verbosity;
            log_inc_level(0);
        }
        else if (option == 'h') {
            log_d3("(option -v) verbosity = '", log_num(verbosity), "'");
            log_d1("(option -h) help = 'true'");
            usage(stdout);
            die(0);
        }
        else {
            usage(stderr);
            die(2);
        }
    }
    if (argc - optind != 1) {
        usage(stderr);
        die(2);
    }

    /* parse destination URI */
    result = s3_uri_parse_into(argv[optind], &uri, &error);
    if (result != S3_RESULT_OK) {
        log_f2("invalid destination: ", s3ar_log_error(&error));
        die(2);
    }
    if (fn == NULL || strcmp(fn, "-") == 0) fn = NULL;

    log_d3("(option -v) verbosity = '", log_num(verbosity), "'");
    log_d1("(option -h) help = 'false'");
    log_d3("(option -f) input-file = '", fn != NULL ? fn : "-", "'");
    log_d3("(option --create-bucket) create-bucket = '",
           create_bucket ? "true" : "false", "'");
    log_d3("(argument) destination = '", s3_log_uri("s3", uri.bucket, uri.key),
           "'");
    log_d5("(option --multipart-size) multipart-size = '",
           log_bytes((long long) multipart_size), "'; maximum object size = '",
           log_bytes((long long) multipart_size * 10000), "' (10000 parts)");

    /* configure S3 client */
    if (s3ar_client_open(&client, &config) != 0) die(2);
    s3ar_interrupt_bind(client, &interrupted_signal);
    result = s3_url_validate_object_name(client, uri.bucket, uri.key, &error);
    if (result != S3_RESULT_OK) {
        log_f2("invalid destination: ", s3ar_log_error(&error));
        die(2);
    }

    /* open input */
    if (fn != NULL) {
        fd = open(fn, O_RDONLY);
        if (fd < 0) {
            log_f4("cannot open ", fn, ": ", log_errno());
            die(2);
        }
    }
    else
        fd = STDIN_FILENO;

    /* ensure destination bucket before reading input */
    if (create_bucket) {
        result = s3_bucket_ensure(client, &error, uri.bucket);
        if (interrupted_signal != 0) {
            log_f1("interrupted");
            die(2);
        }
        if (result != S3_RESULT_OK) {
            log_f2("cannot ensure destination bucket: ",
                   s3ar_log_error(&error));
            die(2);
        }
    }

    /* upload input */
    struct s3ar_io_read_context input = {.fd = &fd,
                                         .interrupted = &interrupted_signal};
    result = s3_object_put_stream(client, &error, uri.bucket, uri.key,
                                  multipart_size, NULL, s3ar_io_read, &input);
    if (interrupted_signal != 0) {
        log_f1("interrupted");
        die(2);
    }
    if (result != S3_RESULT_OK) {
        log_f4("upload failed for ", s3_log_uri("s3", uri.bucket, uri.key),
               ": ", s3ar_log_error(&error));
        die(2);
    }
    if (fn != NULL) {
        int input_fd = fd;
        fd = -1;
        if (close(input_fd) != 0) {
            log_f4("cannot close ", fn, ": ", log_errno());
            die(2);
        }
    }
    log_i2(s3_log_uri("s3", uri.bucket, uri.key), " uploaded");
    log_s3(fn != NULL ? fn : "stdin", " to ",
           s3_log_uri("s3", uri.bucket, uri.key));
    die(0);
}
