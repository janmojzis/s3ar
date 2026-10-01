/* SPDX-License-Identifier: MIT-0 */
#include "main.h"
#include "s3ar_config.h"
#include "s3ar_interrupt.h"
#include "s3ar_log.h"
#include "log.h"
#include "sig.h"

#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct s3_client *client;
static struct s3ar_config_env config;
static struct s3_uri_buffer source, destination;
static struct s3_error error;
static volatile sig_atomic_t interrupted_signal;

static void handle_interrupt(int signal_number) {
    interrupted_signal = signal_number;
}

static void usage(FILE *stream) {
    log_usage(stream, "Usage: s3ar-copy [-v|-vv|-vvv] [--create-bucket] "
                      "[--multipart-size SIZE] "
                      "s3://SOURCE/KEY s3://DESTINATION/KEY\n"
                      "Copy one S3 object on the server.\n");
}

static _Noreturn void die(int status) {
    if (status == 0 && interrupted_signal != 0) {
        log_f1("interrupted");
        status = 2;
    }
    s3_client_close(client);
    s3ar_config_free(&config);
    exit(status);
}

int main_s3ar_copy(int argc, char **argv) {
    enum { OPTION_CREATE_BUCKET = 256, OPTION_MULTIPART_SIZE };
    static const struct option long_options[] = {
        {"create-bucket", no_argument, NULL, OPTION_CREATE_BUCKET},
        {"multipart-size", required_argument, NULL, OPTION_MULTIPART_SIZE},
        {"verbose", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    size_t part_size = S3_MULTIPART_PART_SIZE;
    bool create_bucket = false, part_size_seen = false;
    enum s3_result result;
    int verbosity = 0;
    int option;

    log_set_name("s3ar-copy");
    sig_ignore(SIGPIPE);
    sig_catch(SIGINT, handle_interrupt);
    sig_catch(SIGTERM, handle_interrupt);
    sig_catch(SIGUSR1, log_inc_level);
    sig_catch(SIGUSR2, log_dec_level);

    opterr = 0;
    while ((option = getopt_long(argc, argv, "vh", long_options, NULL)) != -1) {
        if (option == OPTION_CREATE_BUCKET)
            create_bucket = true;
        else if (option == OPTION_MULTIPART_SIZE) {
            if (part_size_seen) {
                log_f1("multipart size specified twice");
                die(2);
            }
            part_size_seen = true;
            if (!s3ar_parse_multipart_size(optarg, &part_size)) {
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
    if (argc - optind != 2) {
        usage(stderr);
        die(2);
    }
    result = s3_uri_parse_into(argv[optind], &source, &error);
    if (result != S3_RESULT_OK) {
        log_f2("invalid source: ", s3ar_log_error(&error));
        die(2);
    }
    result = s3_uri_parse_into(argv[optind + 1], &destination, &error);
    if (result != S3_RESULT_OK) {
        log_f2("invalid destination: ", s3ar_log_error(&error));
        die(2);
    }
    if (strcmp(source.bucket, destination.bucket) == 0 &&
        strcmp(source.key, destination.key) == 0) {
        log_f1("source and destination are identical");
        die(2);
    }

    log_d3("(option -v) verbosity = '", log_num(verbosity), "'");
    log_d1("(option -h) help = 'false'");
    log_d3("(option --create-bucket) create-bucket = '",
           create_bucket ? "true" : "false", "'");
    log_d3("(argument) source = '", s3_log_uri("s3", source.bucket, source.key),
           "'");
    log_d3("(argument) destination = '",
           s3_log_uri("s3", destination.bucket, destination.key), "'");
    log_d5("(option --multipart-size) multipart-size = '",
           log_bytes((long long) part_size), "'; maximum object size = '",
           log_bytes((long long) part_size * 10000), "' (10000 parts)");

    if (s3ar_client_open(&client, &config) != 0) die(2);
    s3ar_interrupt_bind(client, &interrupted_signal);
    result =
        s3_url_validate_object_name(client, source.bucket, source.key, &error);
    if (result != S3_RESULT_OK) {
        log_f2("invalid source: ", s3ar_log_error(&error));
        die(2);
    }
    result = s3_url_validate_object_name(client, destination.bucket,
                                         destination.key, &error);
    if (result != S3_RESULT_OK) {
        log_f2("invalid destination: ", s3ar_log_error(&error));
        die(2);
    }
    if (create_bucket) {
        result = s3_bucket_ensure(client, &error, destination.bucket);
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
    result = s3_object_copy(client, &error, source.bucket, source.key,
                            destination.bucket, destination.key, part_size);
    if (interrupted_signal != 0) {
        log_f1("interrupted");
        die(2);
    }
    if (result != S3_RESULT_OK) {
        log_f4("copy failed for ", s3_log_uri("s3", source.bucket, source.key),
               ": ", s3ar_log_error(&error));
        die(2);
    }
    log_s3(s3_log_uri("s3", source.bucket, source.key), " to ",
           s3_log_uri("s3", destination.bucket, destination.key));
    die(0);
}
