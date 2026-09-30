/* SPDX-License-Identifier: MIT-0 */
#include "s3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static enum s3_result invalid_uri(struct s3_error *error) {
    if (error != NULL) {
        error->result = S3_RESULT_CONFIGURATION_ERROR;
        (void) snprintf(error->message, sizeof(error->message),
                        "operand must be s3://BUCKET/KEY");
    }
    return S3_RESULT_CONFIGURATION_ERROR;
}

static enum s3_result split_uri(const char *text, const char **bucket,
                                size_t *bucket_size, const char **key,
                                size_t *key_size, struct s3_error *error) {
    const char *slash;
    if (text == NULL || strncmp(text, "s3://", 5) != 0 ||
        (slash = strchr(text + 5, '/')) == NULL || slash == text + 5 ||
        slash[1] == '\0')
        return invalid_uri(error);
    *bucket = text + 5;
    *bucket_size = (size_t) (slash - *bucket);
    *key = slash + 1;
    *key_size = strlen(*key);
    return S3_RESULT_OK;
}

enum s3_result s3_uri_parse_alloc(const char *text, struct s3_uri *uri,
                                  struct s3_error *error) {
    const char *bucket, *key;
    size_t bucket_size, key_size;
    enum s3_result result;
    if (uri != NULL) memset(uri, 0, sizeof(*uri));
    if (error != NULL) memset(error, 0, sizeof(*error));
    if (uri == NULL) return invalid_uri(error);
    result = split_uri(text, &bucket, &bucket_size, &key, &key_size, error);
    if (result != S3_RESULT_OK) return result;
    uri->bucket = malloc(bucket_size + 1);
    uri->key = malloc(key_size + 1);
    if (uri->bucket == NULL || uri->key == NULL) {
        s3_uri_free(uri);
        if (error != NULL) {
            error->result = S3_RESULT_ERROR;
            (void) snprintf(error->message, sizeof(error->message),
                            "out of memory");
        }
        return S3_RESULT_ERROR;
    }
    memcpy(uri->bucket, bucket, bucket_size);
    uri->bucket[bucket_size] = '\0';
    memcpy(uri->key, key, key_size + 1);
    return S3_RESULT_OK;
}

void s3_uri_free(struct s3_uri *uri) {
    if (uri == NULL) return;
    free(uri->bucket);
    free(uri->key);
    uri->bucket = NULL;
    uri->key = NULL;
}

enum s3_result s3_uri_parse_into(const char *text, struct s3_uri_buffer *uri,
                                 struct s3_error *error) {
    const char *bucket, *key;
    size_t bucket_size, key_size;
    enum s3_result result;
    if (uri != NULL) memset(uri, 0, sizeof(*uri));
    if (error != NULL) memset(error, 0, sizeof(*error));
    if (uri == NULL) return invalid_uri(error);
    result = split_uri(text, &bucket, &bucket_size, &key, &key_size, error);
    if (result != S3_RESULT_OK) return result;
    if (bucket_size >= sizeof(uri->bucket) || key_size >= sizeof(uri->key)) {
        if (error != NULL) {
            error->result = S3_RESULT_CONFIGURATION_ERROR;
            (void) snprintf(error->message, sizeof(error->message),
                            "S3 URI bucket or key is too long");
        }
        return S3_RESULT_CONFIGURATION_ERROR;
    }
    memcpy(uri->bucket, bucket, bucket_size);
    uri->bucket[bucket_size] = '\0';
    memcpy(uri->key, key, key_size + 1);
    return S3_RESULT_OK;
}
