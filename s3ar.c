/* SPDX-License-Identifier: MIT-0 */

#include "die.h"
#include "s3.h"
#include "s3ar.h"

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *stream) {
    fprintf(stream, "Usage: s3ar (-c | --create) [-v | --verbose] "
                    "[-f TARFILE] S3...\n"
                    "       s3ar (-t | --list) [-v | --verbose] S3...\n"
                    "\n"
                    "Options:\n"
                    "  -c, --create  create a tar archive from S3\n"
                    "  -t, --list  list an S3 source\n"
                    "  -f, --file TARFILE  write the archive to TARFILE\n"
                    "  -v, --verbose  enable verbose output\n"
                    "  -h, --help  display this help\n"
                    "\n"
                    "S3 source:\n"
                    "  s3://                all buckets and objects\n"
                    "  s3://BUCKET[/]        bucket and all its objects\n"
                    "  s3://BUCKET/NAME[/]   object and objects below NAME/\n");
    fflush(stream);
}

static void parse_s3_environment(struct s3 *s3) {
    const char *x;

    /* S3AR_REGION */
    s3->region = "us-east-1";
    x = getenv("S3AR_REGION");
    if (x) {
        s3->region = x;
    }

    /* S3AR_URI_STYLE */
    s3->uri_style = S3UriStylePath;
    x = getenv("S3AR_URI_STYLE");
    if (x) {
        if (strcmp(x, "virtual") == 0) {
            s3->uri_style = S3UriStyleVirtualHost;
        }
        else if (strcmp(x, "path") != 0) {
            errno = 0;
            die_fatal("s3ar: $S3AR_URI_STYLE must be 'path' or 'virtual'",
                      NULL, NULL);
        }
    }

    /* access key */
    x = getenv("S3AR_ACCESS_KEY");
    if (!x) {
        errno = 0;
        die_fatal("s3ar: $S3AR_ACCESS_KEY not set", NULL, NULL);
    }
    s3->access_key = x;

    /* secret key */
    x = getenv("S3AR_SECRET_KEY");
    if (!x) {
        errno = 0;
        die_fatal("s3ar: $S3AR_SECRET_KEY not set", NULL, NULL);
    }
    s3->secret_key = x;

    /* endpoint */
    x = getenv("S3AR_ENDPOINT");
    if (!x) {
        errno = 0;
        die_fatal("s3ar: $S3AR_ENDPOINT not set", NULL, NULL);
    }

    /* http/https protocol */
    s3->protocol = S3ProtocolHTTPS;
    if (strncmp(x, "http://", 7) == 0) {
        s3->protocol = S3ProtocolHTTP;
        x += 7;
    }
    else if (strncmp(x, "https://", 8) == 0) {
        x += 8;
    }

    /* host */
    s3->host = strdup(x);
    if (s3->host == NULL) { die_fatal("s3ar: out of memory", NULL, NULL); }
    size_t length = strlen(s3->host);
    if (length > 0 && s3->host[length - 1] == '/') {
        s3->host[--length] = '\0';
    }
    if (length == 0 || strchr(s3->host, '/') != NULL) {
        errno = 0;
        die_fatal("s3ar: S3AR_ENDPOINT must contain a host and no path", NULL,
                  NULL);
    }

}

static const struct option long_options[] = {
    {"create", no_argument, NULL, 'c'},
    {"list", no_argument, NULL, 't'},
    {"file", required_argument, NULL, 'f'},
    {"verbose", no_argument, NULL, 'v'},
    {"help", no_argument, NULL, 'h'},
    {NULL, 0, NULL, 0},
};

static struct s3ar_config config;

int main(int argc, char **argv) {

    /* parse options */
    opterr = 0;
    for (;;) {
        int option = getopt_long(argc, argv, "ctvf:h", long_options, NULL);
        if (option == -1) { break; }

        /* -c --create */
        if (option == 'c') {
            if (config.command != S3AR_COMMAND_NONE) {
                errno = 0;
                die_fatal("s3ar: command specified twice", NULL, NULL);
            }
            config.command = S3AR_COMMAND_CREATE;
        }

        /* -t --list */
        else if (option == 't') {
            if (config.command != S3AR_COMMAND_NONE) {
                errno = 0;
                die_fatal("s3ar: command specified twice", NULL, NULL);
            }
            config.command = S3AR_COMMAND_LIST;
        }

        /* -f --file */
        else if (option == 'f') {
            if (config.archive_path != NULL) {
                errno = 0;
                die_fatal("s3ar: archive file specified twice", NULL, NULL);
            }
            config.archive_path = optarg;
        }

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
        die_fatal("s3ar: specify -c or -t", NULL, NULL);
    }
    if (config.command == S3AR_COMMAND_LIST && config.archive_path != NULL) {
        errno = 0;
        die_fatal("s3ar: -f is valid only with -c", NULL, NULL);
    }
    if (argc - optind < 1) {
        errno = 0;
        die_fatal("s3ar: command requires at least one S3 operand", NULL,
                  NULL);
    }
    config.operand_count = argc - optind;
    config.operands = &argv[optind];

    /* parse environment */
    parse_s3_environment(&config.s3);


    /* run commands */
    s3_open(&config.s3);
    switch (config.command) {
        case S3AR_COMMAND_CREATE:
            s3ar_create(&config);
            break;
        case S3AR_COMMAND_LIST:
            s3ar_list(&config);
            break;
        case S3AR_COMMAND_NONE:
            break;
    }
    s3_close();
    free(config.s3.host);

    if (fflush(stdout) == EOF) {
        die_fatal("s3ar: unable to flush standard output", NULL, NULL);
    }
    return 0;
}
