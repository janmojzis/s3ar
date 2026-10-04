/* SPDX-License-Identifier: MIT-0 */
#ifndef S3_UPLOAD_H
#define S3_UPLOAD_H

#include "s3_internal.h"

/* Shared upload transport and multipart lifecycle for PUT and COPY.
 * Upload IDs passed between these functions are already URI-encoded.
 * Returned IDs and ETags are owned by the caller. */
enum {
    S3_UPLOAD_MIN_PART_SIZE = 5 * 1024 * 1024,
    S3_UPLOAD_RESPONSE_LIMIT = 1024 * 1024
};
enum s3_upload_retry_mode {
    S3_UPLOAD_ONCE,
    S3_UPLOAD_RETRY,
    S3_UPLOAD_COMPLETE
};

struct s3_upload_copy_request {
    const char *source;
    const char *etag;
    const char *range;
    const char *result_root;
};

enum s3_result s3_upload_request_ex(
    struct s3_client *client, struct s3_error *error, const char *url,
    const char *method, const unsigned char *body, size_t body_size,
    const struct s3_object_properties *properties,
    enum s3_upload_retry_mode retry_mode, bool *completion_uncertain,
    struct s3_memory_response *output,
    const struct s3_upload_copy_request *copy, const char *tagging);

enum s3_result s3_upload_request(struct s3_client *client,
                                 struct s3_error *error, const char *url,
                                 const char *method, const unsigned char *body,
                                 size_t body_size,
                                 const struct s3_object_properties *properties,
                                 enum s3_upload_retry_mode retry_mode,
                                 bool *completion_uncertain,
                                 struct s3_memory_response *output);

enum s3_result s3_upload_initiate(struct s3_client *client,
                                  struct s3_error *error, const char *bucket,
                                  const char *key,
                                  const struct s3_object_properties *properties,
                                  const char *tagging,
                                  char **encoded_upload_id);

enum s3_result s3_upload_part(struct s3_client *client, struct s3_error *error,
                              const char *bucket, const char *key,
                              const char *encoded_upload_id, size_t part_number,
                              const unsigned char *body, size_t body_size,
                              const struct s3_upload_copy_request *copy,
                              char **etag);

enum s3_result s3_upload_complete(struct s3_client *client,
                                  struct s3_error *error, const char *bucket,
                                  const char *key,
                                  const char *encoded_upload_id,
                                  char *const *etags, size_t count,
                                  bool *uncertain);

void s3_upload_cleanup_failed(struct s3_client *client, struct s3_error *error,
                              const char *bucket, const char *key,
                              const char *encoded_upload_id,
                              enum s3_result result, bool completion_uncertain);

#endif
