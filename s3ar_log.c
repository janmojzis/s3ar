/* SPDX-License-Identifier: MIT-0 */
#include "s3ar_log.h"

static void format_error(FILE *stream, const void *data) {
    const struct s3_error *error = data;
    struct log_part part = s3_log_error(error);
    part.value.custom.format(stream, part.value.custom.data);
    if (error != NULL && error->bucket_region[0] != '\0')
        (void) fputs("; set S3AR_REGION", stream);
}

struct log_part s3ar_log_error(const struct s3_error *error) {
    return log_custom(format_error, error);
}
