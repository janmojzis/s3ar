/* SPDX-License-Identifier: MIT-0 */

#include "die.h"
#include "s3ar.h"

#include <inttypes.h>
#include <stdio.h>

static void log_s3_name(const char *bucket, const char *key) {
    int result = key == NULL ? fprintf(stdout, "s3://%s\n", bucket)
                             : fprintf(stdout, "s3://%s/%s\n", bucket, key);
    if (result < 0) {
        die_fatal("s3ar: unable to write standard output", bucket, key);
    }
}

void log_s3_object(const struct s3_object *object, bool verbose) {
    if (!verbose) {
        log_s3_name(object->bucket, object->key);
        return;
    }
    int result = fprintf(stdout, "s3://%s/%s\t%" PRIu64 "\t",
                         object->bucket, object->key, object->size);
    for (size_t i = 0; result >= 0 && i < object->metadata_count; ++i) {
        result = fprintf(stdout, "%s%s=%s", i > 0 ? "," : "",
                         object->metadata[i].name,
                         object->metadata[i].value);
    }
    if (result >= 0 && object->metadata_count == 0) {
        result = fputc('-', stdout);
    }
    if (result >= 0) { result = fputc('\n', stdout); }
    if (result < 0) {
        die_fatal("s3ar: unable to write standard output", object->bucket,
                  object->key);
    }
}
