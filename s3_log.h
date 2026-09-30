/* SPDX-License-Identifier: MIT-0 */
#ifndef S3_LOG_H
#define S3_LOG_H

#include "log.h"
#include "s3.h"

/* The error must remain valid until the synchronous log call returns. */
struct log_part s3_log_error(const struct s3_error *error);

struct s3_log_uri_data {
    const char *scheme;
    const char *bucket;
    const char *key;
};

void s3_log_format_uri(FILE *stream, const void *data);

/* URI data remains valid throughout a synchronous log call. */
#define s3_log_uri(scheme, bucket, key)                                        \
    log_custom(s3_log_format_uri,                                              \
               &(const struct s3_log_uri_data) {(scheme), (bucket), (key)})

#endif
