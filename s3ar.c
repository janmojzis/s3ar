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
    fprintf(stream, "Usage: s3ar (-t | --list) [-v | --verbose] S3...\n"
                    "\n"
                    "Options:\n"
                    "  -t, --list  list an S3 source\n"
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
    {"list", no_argument, NULL, 't'},
    {"verbose", no_argument, NULL, 'v'},
    {"help", no_argument, NULL, 'h'},
    {NULL, 0, NULL, 0},
};

static struct s3ar_config config;

int main(int argc, char **argv) {

    /* parse options */
    opterr = 0;
    for (;;) {
        int option = getopt_long(argc, argv, "tvh", long_options, NULL);
        if (option == -1) { break; }

        /* -t --list */
        if (option == 't') {
            if (config.command != S3AR_COMMAND_NONE) {
                errno = 0;
                die_fatal("s3ar: list option specified twice", NULL, NULL);
            }
            config.command = S3AR_COMMAND_LIST;
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
        die_fatal("s3ar: specify -t or --list", NULL, NULL);
    }
    if (argc - optind < 1) {
        errno = 0;
        die_fatal("s3ar: list requires at least one S3 operand", NULL, NULL);
    }
    config.operand_count = argc - optind;
    config.operands = &argv[optind];

    /* parse environment */
    parse_s3_environment(&config.s3);


    /* run commands */
    s3_open(&config.s3);
    switch (config.command) {
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
