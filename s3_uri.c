/* SPDX-License-Identifier: MIT-0 */
#include "s3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum s3_result s3_uri_parse(const char *text, struct s3_uri *uri,
                            struct s3_error *error) {
    const char *slash;
    size_t bucket_size;
    if (uri != NULL) memset(uri, 0, sizeof(*uri));
    if (error != NULL) memset(error, 0, sizeof(*error));
    if (text == NULL || uri == NULL || strncmp(text, "s3://", 5) != 0) goto bad;
    slash = strchr(text + 5, '/');
    if (slash == NULL || slash == text + 5 || slash[1] == '\0') goto bad;
    bucket_size = (size_t)(slash - (text + 5));
    uri->bucket = malloc(bucket_size + 1);
    uri->key = strdup(slash + 1);
    if (uri->bucket == NULL || uri->key == NULL) {
        s3_uri_free(uri);
        if (error != NULL) {
            error->result = S3_RESULT_ERROR;
            (void)snprintf(error->message, sizeof(error->message),
                           "out of memory");
        }
        return S3_RESULT_ERROR;
    }
    memcpy(uri->bucket, text + 5, bucket_size);
    uri->bucket[bucket_size] = '\0';
    return S3_RESULT_OK;
bad:
    if (error != NULL) {
        error->result = S3_RESULT_CONFIGURATION_ERROR;
        (void)snprintf(error->message, sizeof(error->message),
                       "operand must be s3://BUCKET/KEY");
    }
    return S3_RESULT_CONFIGURATION_ERROR;
}

void s3_uri_free(struct s3_uri *uri) {
    if (uri == NULL) return;
    free(uri->bucket);
    free(uri->key);
    uri->bucket = NULL;
    uri->key = NULL;
}
