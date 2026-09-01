/* SPDX-License-Identifier: MIT-0 */

#include "die.h"
#include "log.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>

static bool url_safe(unsigned char value) {
    return (value >= 'A' && value <= 'Z') ||
           (value >= 'a' && value <= 'z') ||
           (value >= '0' && value <= '9') || value == '-' || value == '.' ||
           value == '_' || value == '~' || value == '/';
}

int log_url_encoded_name(FILE *stream, const char *name) {
    static const char hex[] = "0123456789ABCDEF";
    int saved_errno = errno;
    const unsigned char *input = (const unsigned char *) name;
    for (; *input != '\0'; ++input) {
        if (url_safe(*input)) {
            if (fputc(*input, stream) == EOF) { return -1; }
            continue;
        }
        if (fputc('%', stream) == EOF ||
            fputc(hex[*input >> 4], stream) == EOF ||
            fputc(hex[*input & 15], stream) == EOF) {
            return -1;
        }
    }
    errno = saved_errno;
    return 0;
}

void log_s3_name(FILE *stream, const char *bucket, const char *key) {
    int result = log_url_encoded_name(stream, bucket);
    if (result >= 0 && key != NULL) {
        result = fputc('/', stream) == EOF
                     ? -1
                     : log_url_encoded_name(stream, key);
    }
    if (result >= 0) { result = fputc('\n', stream) == EOF ? -1 : 0; }
    if (result < 0) {
        die_fatal(stream == stdout ? "s3ar: unable to write standard output"
                                   : "s3ar: unable to write verbose output",
                  bucket, key);
    }
}

void log_s3_bucket(const struct s3_bucket *bucket, bool verbose) {
    int result = log_url_encoded_name(stdout, bucket->name);
    if (result >= 0 && verbose) {
        result = fprintf(stdout, " acl=%s",
                         bucket->acl != NULL ? bucket->acl : "unavailable");
    }
    if (result >= 0) { result = fputc('\n', stdout) == EOF ? -1 : 0; }
    if (result < 0) {
        die_fatal("s3ar: unable to write standard output", bucket->name,
                  NULL);
    }
}

void log_s3_object(const struct s3_object *object, bool verbose) {
    if (!verbose) {
        log_s3_name(stdout, object->bucket, object->key);
        return;
    }
    int result = log_url_encoded_name(stdout, object->bucket);
    if (result >= 0) { result = fputc('/', stdout) == EOF ? -1 : 0; }
    if (result >= 0) {
        result = log_url_encoded_name(stdout, object->key);
    }
    if (result >= 0) {
        result = fprintf(stdout, " %" PRIu64 " %" PRId64 " %s",
                         object->size, object->last_modified,
                         object->etag != NULL ? object->etag : "-");
    }
    if (result >= 0) { result = fputc('\n', stdout); }
    if (result < 0) {
        die_fatal("s3ar: unable to write standard output", object->bucket,
                  object->key);
    }
}
