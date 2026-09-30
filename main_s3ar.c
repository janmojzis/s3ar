/*
 * Main command-line entry point for S3 Archiver. This module parses command
 * options and S3 environment variables, initializes the S3 connection, and
 * dispatches archive creation, extraction, and archive listing.
 *
 * s3ar is a tar-like utility. Archive operations intentionally use familiar
 * tar options such as -c, -x, -f, and -v, and selection operands follow tar
 * member-selection semantics. s3ar aims to provide a similar command-line
 * experience for S3 resources, but implements only the options documented by
 * this utility and is not a complete replacement for tar.
 *
 * SPDX-License-Identifier: MIT-0
 */

#include "log.h"
#include "s3ar_log.h"
#include "s3.h"
#include "s3ar.h"
#include "main.h"
#include "s3ar_config.h"
#include "s3ar_transform.h"
#include "sig.h"

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void) {
    log_usage(stderr,
              "Usage: s3ar (-c | --create) [-v | --verbose] "
              "[--zstd] [-f TARFILE] S3...\n"
              "       s3ar (-x | --extract) [-v | --verbose] "
              "[--zstd] [--transform EXPR] [-f TARFILE] S3...\n"
              "       s3ar (-t | --list) [-v | --verbose] "
              "[--zstd] [--transform EXPR] [-f TARFILE] [S3...]\n"
              "\n"
              "Options:\n"
              "  -c, --create  create a tar archive from S3\n"
              "  -x, --extract, --get  extract a tar archive to S3\n"
              "  -t, --list  list objects in a tar archive\n"
              "  -f, --file TARFILE  read or write TARFILE\n"
              "      --zstd  use zstd archive compression\n"
              "      --transform EXPR  rename BUCKET/KEY during -x or -t\n"
              "                        "
              "s<delimiter>REGEX<delimiter>REPLACEMENT<delimiter>[gi]\n"
              "                        repeat to apply substitutions in order\n"
              "  -v, --verbose  increase verbosity (repeat up to -vvv)\n"
              "  -h, --help  display this help\n"
              "\n"
              "S3 source:\n"
              "  s3://                all buckets and objects\n"
              "  s3://BUCKET[/]        bucket and all its objects\n"
              "  s3://BUCKET/NAME[/]   object and objects below NAME/\n");
}

enum long_option { OPTION_ZSTD = 256, OPTION_TRANSFORM };

static const struct option long_options[] = {
    {"create", no_argument, NULL, 'c'},
    {"extract", no_argument, NULL, 'x'},
    {"get", no_argument, NULL, 'x'},
    {"list", no_argument, NULL, 't'},
    {"file", required_argument, NULL, 'f'},
    {"zstd", no_argument, NULL, OPTION_ZSTD},
    {"transform", required_argument, NULL, OPTION_TRANSFORM},
    {"verbose", no_argument, NULL, 'v'},
    {"help", no_argument, NULL, 'h'},
    {NULL, 0, NULL, 0},
};

static struct s3ar_config config;
static struct s3ar_config_env s3_config;

static void debug_options(int verbosity, bool help) {
    log_d3("(option -v) verbosity = '", log_num(verbosity), "'");
    log_d3("(option -h) help = '", help ? "true" : "false", "'");
    log_d3("(option -c) create = '",
           config.command == S3AR_COMMAND_CREATE ? "true" : "false", "'");
    log_d3("(option -x) extract = '",
           config.command == S3AR_COMMAND_EXTRACT ? "true" : "false", "'");
    log_d3("(option -t) list = '",
           config.command == S3AR_COMMAND_LIST_ARCHIVE ? "true" : "false", "'");
    log_d3("(option -f) archive-file = '",
           config.archive_path != NULL ? config.archive_path : "-", "'");
    log_d3("(option --zstd) zstd = '", config.zstd ? "true" : "false", "'");
    s3ar_transform_log(config.transforms);
}

_Noreturn void s3ar_die(int status) {
    s3ar_create_cleanup();
    s3_client_close(config.s3);
    s3ar_config_free(&s3_config);
    s3ar_transform_free(config.transforms);
    exit(status);
}

int main_s3ar(int argc, char **argv) {
    int verbosity = 0;
    log_set_name("s3ar");
    struct s3_error s3_error;
    enum s3_result s3_result;
    sig_ignore(SIGPIPE);

    /* adjust verbosity using signals */
    sig_catch(SIGUSR1, log_inc_level);
    sig_catch(SIGUSR2, log_dec_level);

    /* parse options */
    opterr = 0;
    for (;;) {
        int option = getopt_long(argc, argv, ":cxtvf:h", long_options, NULL);
        if (option == -1) { break; }

        /* -c --create */
        if (option == 'c') {
            if (config.command != S3AR_COMMAND_NONE) {
                log_f1("command specified twice");
                s3ar_die(2);
            }
            config.command = S3AR_COMMAND_CREATE;
        }

        /* -x --extract */
        else if (option == 'x') {
            if (config.command != S3AR_COMMAND_NONE) {
                log_f1("command specified twice");
                s3ar_die(2);
            }
            config.command = S3AR_COMMAND_EXTRACT;
        }

        /* -t --list */
        else if (option == 't') {
            if (config.command != S3AR_COMMAND_NONE) {
                log_f1("command specified twice");
                s3ar_die(2);
            }
            config.command = S3AR_COMMAND_LIST_ARCHIVE;
        }

        /* -f --file */
        else if (option == 'f') {
            if (config.archive_path != NULL) {
                log_f1("archive file specified twice");
                s3ar_die(2);
            }
            config.archive_path = optarg;
        }

        /* --zstd */
        else if (option == OPTION_ZSTD) { config.zstd = true; }

        else if (option == OPTION_TRANSFORM) {
            char error[256];
            if (!s3ar_transform_add(&config.transforms, optarg, error,
                                    sizeof(error))) {
                log_f2("invalid --transform: ", error);
                s3ar_die(2);
            }
        }

        /* -v --verbose */
        else if (option == 'v') {
            if (verbosity < 3) ++verbosity;
            config.verbose = true;
            log_inc_level(0);
        }

        /* -h --help */
        else if (option == 'h') {
            debug_options(verbosity, true);
            usage();
            s3ar_die(0);
        }

        /* missing option argument */
        else if (option == ':') {
            if (optopt == OPTION_TRANSFORM) {
                log_f1("option requires an argument --transform");
                s3ar_die(2);
            }
            char name[] = {'-', (char) optopt, '\0'};
            log_f2("option requires an argument ", name);
            s3ar_die(2);
        }

        /* unknown */
        else {
            char name[] = {'-', (char) optopt, '\0'};
            const char *argument = optopt != 0 ? name : argv[optind - 1];
            log_f2("unknown option ", argument);
            s3ar_die(2);
        }
    }

    if (config.command == S3AR_COMMAND_NONE) {
        log_f1("specify -c, -x or -t");
        s3ar_die(2);
    }
    if (config.command == S3AR_COMMAND_CREATE && config.transforms != NULL) {
        log_f1("--transform requires -x or -t");
        s3ar_die(2);
    }
    if (config.archive_path != NULL &&
        strncmp(config.archive_path, "s3://", 5) == 0) {
        log_f1("TARFILE must be a local filesystem path or '-'");
        s3ar_die(2);
    }
    if ((config.command == S3AR_COMMAND_CREATE ||
         config.command == S3AR_COMMAND_EXTRACT) &&
        argc - optind < 1) {
        log_f1("command requires at least one S3 operand");
        s3ar_die(2);
    }
    config.operand_count = argc - optind;
    config.operands = &argv[optind];

    debug_options(verbosity, false);
    if (config.operand_count == 0) log_d1("(argument) selection = '(all)'");
    for (int i = optind; i < argc; ++i) {
        bool s3_uri = strncmp(argv[i], "s3://", 5) == 0;
        log_d3("(argument) selection = '",
               s3_log_uri(NULL, s3_uri ? argv[i] + 5 : argv[i], NULL), "'");
    }

    if (config.command == S3AR_COMMAND_LIST_ARCHIVE) {
        s3ar_list_archive(&config);
        if (fflush(stdout) == EOF) {
            log_f3("unable to flush standard output", ": ", log_errno());
            s3ar_die(2);
        }
        s3ar_die(0);
    }

    /* Parse environment and connect to S3. */
    s3_result = s3ar_config_from_env(&s3_config, &s3_error);
    if (s3_result != S3_RESULT_OK) {
        log_f2("invalid configuration: ", s3ar_log_error(&s3_error));
        s3ar_die(2);
    }
    s3_result = s3_client_open(&config.s3, &s3_error, &s3_config.client);
    if (s3_result != S3_RESULT_OK) {
        log_f2("unable to initialize S3 client: ", s3ar_log_error(&s3_error));
        s3ar_die(2);
    }

    /* run commands */
    switch (config.command) {
        case S3AR_COMMAND_CREATE:
            s3ar_create(&config);
            break;
        case S3AR_COMMAND_EXTRACT:
            s3ar_extract(&config);
            break;
        case S3AR_COMMAND_LIST_ARCHIVE:
            break;
        case S3AR_COMMAND_NONE:
            break;
    }
    if (fflush(stderr) == EOF) {
        log_f3("unable to flush standard error", ": ", log_errno());
        s3ar_die(2);
    }
    if (fflush(stdout) == EOF) {
        log_f3("unable to flush standard output", ": ", log_errno());
        s3ar_die(2);
    }
    s3ar_die(0);
}
