/* SPDX-License-Identifier: MIT-0 */

#include "die.h"
#include "log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Noreturn void die_fatal(const char *text, const char *x, const char *y) {
    fputs(text, stderr);
    if (x) {
        fputc(' ', stderr);
        log_quote_name(stderr, x);
        if (y) {
            fputc('/', stderr);
            log_quote_name(stderr, y);
        }
    }

    if (errno != 0) { fprintf(stderr, ": %s", strerror(errno)); }
    fprintf(stderr, "\n");
    fflush(stderr);
    exit(2);
}

_Noreturn void die_s3fatal(const char *text, const char *bucket,
                           const char *key, S3Status status,
                           const S3ErrorDetails *details) {
    fputs(text, stderr);
    if (bucket != NULL) {
        fputc(' ', stderr);
        log_quote_name(stderr, bucket);
        if (key != NULL) {
            fputc('/', stderr);
            log_quote_name(stderr, key);
        }
    }
    fprintf(stderr, ": %s", S3_get_status_name(status));
    if (details != NULL && details->message != NULL) {
        fprintf(stderr, ": %s", details->message);
    }
    fputc('\n', stderr);
    fflush(stderr);
    exit(2);
}
