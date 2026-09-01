/*
 * Main command-line entry point for S3 Archiver. This module parses command
 * options and S3 environment variables, initializes the S3 connection, and
 * dispatches archive creation, extraction, and bucket or object listing.
 *
 * s3ar is a tar-like utility. Archive operations intentionally use familiar
 * tar options such as -c, -x, -f, and -v, and selection operands follow tar
 * member-selection semantics. s3ar aims to provide a similar command-line
 * experience for S3 resources, but implements only the options documented by
 * this utility and is not a complete replacement for tar.
 *
 * SPDX-License-Identifier: MIT-0
 */

#include "die.h"
#include "s3.h"
#include "s3ar.h"

#include <errno.h>
#include <getopt.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *stream) {
    fprintf(stream, "Usage: s3ar (-c | --create) [-v | --verbose] "
                    "[--zstd] [-f TARFILE] S3...\n"
                    "       s3ar (-x | --extract) [-v | --verbose] "
                    "[--zstd] [-f TARFILE] [S3...]\n"
                    "       s3ar --list-objects [-v | --verbose] S3...\n"
                    "       s3ar --list-buckets [-v | --verbose] [S3...]\n"
                    "\n"
                    "Options:\n"
                    "  -c, --create  create a tar archive from S3\n"
                    "  -x, --extract, --get  extract a tar archive to S3\n"
                    "      --list-objects  list S3 objects\n"
                    "      --list-buckets  list bucket names\n"
                    "  -f, --file TARFILE  read or write TARFILE\n"
                    "      --zstd  use zstd archive compression\n"
                    "  -v, --verbose  enable verbose output\n"
                    "  -h, --help  display this help\n"
                    "\n"
                    "S3 source:\n"
                    "  s3://                all buckets and objects\n"
                    "  s3://BUCKET[/]        bucket and all its objects\n"
                    "  s3://BUCKET/NAME[/]   object and objects below NAME/\n");
    fflush(stream);
}

static const struct option long_options[] = {
    {"create", no_argument, NULL, 'c'},
    {"extract", no_argument, NULL, 'x'},
    {"get", no_argument, NULL, 'x'},
    {"list-buckets", no_argument, NULL, 257},
    {"list-objects", no_argument, NULL, 258},
    {"file", required_argument, NULL, 'f'},
    {"zstd", no_argument, NULL, 256},
    {"verbose", no_argument, NULL, 'v'},
    {"help", no_argument, NULL, 'h'},
    {NULL, 0, NULL, 0},
};

static struct s3ar_config config;

int main(int argc, char **argv) {
    struct s3_config s3_config;
    struct s3_error s3_error;
    enum s3_result s3_result;
    if (setlocale(LC_CTYPE, "") == NULL) {
        fputs("s3ar: warning: unable to initialize character locale; "
              "using C locale\n",
              stderr);
        if (setlocale(LC_CTYPE, "C") == NULL) {
            errno = 0;
            die_fatal("s3ar: unable to initialize C locale", NULL, NULL);
        }
    }
    /* parse options */
    opterr = 0;
    for (;;) {
        int option = getopt_long(argc, argv, "cxvf:h", long_options, NULL);
        if (option == -1) { break; }

        /* -c --create */
        if (option == 'c') {
            if (config.command != S3AR_COMMAND_NONE) {
                errno = 0;
                die_fatal("s3ar: command specified twice", NULL, NULL);
            }
            config.command = S3AR_COMMAND_CREATE;
        }

        /* -x --extract */
        else if (option == 'x') {
            if (config.command != S3AR_COMMAND_NONE) {
                errno = 0;
                die_fatal("s3ar: command specified twice", NULL, NULL);
            }
            config.command = S3AR_COMMAND_EXTRACT;
        }

        /* --list-objects */
        else if (option == 258) {
            if (config.command != S3AR_COMMAND_NONE) {
                errno = 0;
                die_fatal("s3ar: command specified twice", NULL, NULL);
            }
            config.command = S3AR_COMMAND_LIST_OBJECTS;
        }

        /* --list-buckets */
        else if (option == 257) {
            if (config.command != S3AR_COMMAND_NONE) {
                errno = 0;
                die_fatal("s3ar: command specified twice", NULL, NULL);
            }
            config.command = S3AR_COMMAND_LIST_BUCKETS;
        }

        /* -f --file */
        else if (option == 'f') {
            if (config.archive_path != NULL) {
                errno = 0;
                die_fatal("s3ar: archive file specified twice", NULL, NULL);
            }
            config.archive_path = optarg;
        }

        /* --zstd */
        else if (option == 256) { config.zstd = true; }

        /* -v --verbose */
        else if (option == 'v') { config.verbose = true; }

        /* -h --help */
        else if (option == 'h') {
            usage(stdout);
            return 0;
        }

        /* unknown */
        else {
            errno = 0;
            die_fatal("s3ar: unknown option", argv[optind - 1], NULL);
        }
    }

    if (config.command == S3AR_COMMAND_NONE) {
        errno = 0;
        die_fatal("s3ar: specify -c, -x, --list-objects or --list-buckets",
                  NULL, NULL);
    }
    if ((config.command == S3AR_COMMAND_LIST_OBJECTS ||
         config.command == S3AR_COMMAND_LIST_BUCKETS) &&
        config.archive_path != NULL) {
        errno = 0;
        die_fatal("s3ar: -f is valid only with -c or -x", NULL, NULL);
    }
    if (config.archive_path != NULL &&
        strncmp(config.archive_path, "s3://", 5) == 0) {
        errno = 0;
        die_fatal("s3ar: TARFILE must be a local filesystem path or '-'",
                  NULL, NULL);
    }
    if ((config.command == S3AR_COMMAND_CREATE ||
         config.command == S3AR_COMMAND_LIST_OBJECTS) &&
        argc - optind < 1) {
        errno = 0;
        die_fatal("s3ar: command requires at least one S3 operand", NULL,
                  NULL);
    }
    if (config.command == S3AR_COMMAND_LIST_OBJECTS ||
        config.command == S3AR_COMMAND_LIST_BUCKETS) {
        if (config.zstd) {
            errno = 0;
            die_fatal(config.command == S3AR_COMMAND_LIST_OBJECTS
                          ? "s3ar: option --zstd is not valid with "
                            "--list-objects"
                          : "s3ar: option --zstd is not valid with "
                            "--list-buckets",
                      NULL, NULL);
        }
    }
    if (config.command == S3AR_COMMAND_LIST_OBJECTS) {
        for (int i = optind; i < argc; ++i) {
            if (strncmp(argv[i], "s3://", 5) != 0) {
                errno = 0;
                die_fatal("s3ar: --list-objects operand must be s3://",
                          argv[i], NULL);
            }
        }
    }
    config.operand_count = argc - optind;
    config.operands = &argv[optind];

    /* Parse environment and connect to S3. */
    s3_result = s3_config_from_env(&s3_config, &s3_error);
    if (s3_result != S3_RESULT_OK) {
        die_s3fatal("s3ar: invalid S3 configuration", NULL, NULL, s3_result,
                    &s3_error);
    }
    s3_result = s3_client_open(&config.s3, &s3_error, &s3_config.client);
    if (s3_result != S3_RESULT_OK) {
        s3_config_free(&s3_config);
        die_s3fatal("s3ar: unable to initialize S3 client", NULL, NULL,
                    s3_result, &s3_error);
    }

    /* run commands */
    switch (config.command) {
        case S3AR_COMMAND_CREATE:
            s3ar_create(&config);
            break;
        case S3AR_COMMAND_EXTRACT:
            s3ar_extract(&config);
            break;
        case S3AR_COMMAND_LIST_OBJECTS:
            s3ar_list_objects(&config);
            break;
        case S3AR_COMMAND_LIST_BUCKETS:
            s3ar_list_buckets(&config);
            break;
        case S3AR_COMMAND_NONE:
            break;
    }
    s3_client_close(config.s3);
    s3_config_free(&s3_config);

    if (fflush(stdout) == EOF) {
        die_fatal("s3ar: unable to flush standard output", NULL, NULL);
    }
    return 0;
}
