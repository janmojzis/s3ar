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
        log_url_encoded_name(stderr, x);
        if (y) {
            fputc('/', stderr);
            log_url_encoded_name(stderr, y);
        }
    }

    if (errno != 0) { fprintf(stderr, ": %s", strerror(errno)); }
    fprintf(stderr, "\n");
    fflush(stderr);
    exit(2);
}

_Noreturn void die_s3fatal(const char *text, const char *bucket,
                           const char *key, enum s3_result result,
                           const struct s3_error *error) {
    fputs(text, stderr);
    if (bucket != NULL) {
        fputc(' ', stderr);
        log_url_encoded_name(stderr, bucket);
        if (key != NULL) {
            fputc('/', stderr);
            log_url_encoded_name(stderr, key);
        }
    }
    fprintf(stderr, ": %s", s3_result_name(result));
    if (error != NULL && error->message[0] != '\0') {
        fprintf(stderr, ": %s", error->message);
    }
    if (error != NULL && error->s3_code[0] != '\0') {
        fprintf(stderr, " (S3 code %s)", error->s3_code);
    }
    if (error != NULL && error->request_id[0] != '\0') {
        fprintf(stderr, " (request %s)", error->request_id);
    }
    fputc('\n', stderr);
    fflush(stderr);
    exit(2);
}
