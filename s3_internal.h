/* SPDX-License-Identifier: MIT-0 */
/* Private declarations for the s3ar S3 client. */
#ifndef S3_INTERNAL_H
#define S3_INTERNAL_H

#include "s3.h"

#include <curl/curl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { S3_ERROR_BODY_LIMIT = 64 * 1024 };

struct s3_client {
    CURL *curl;
    char *endpoint;
    char *region;
    char *access_key;
    char *secret_key;
    char *credentials;
    char *session_token;
    char *user_agent;
    enum s3_uri_style uri_style;
    unsigned max_attempts;
    unsigned connect_timeout_ms;
    unsigned low_speed_time_s;
};

struct s3_response {
    long status;
    bool headers_done;
    bool invalid_headers;
    bool have_length;
    uint64_t content_length;
    bool have_content_range;
    uint64_t range_first;
    uint64_t range_last;
    uint64_t range_total;
    struct s3_object_properties properties;
    struct s3_metadata *metadata;
    size_t metadata_count;
    size_t metadata_capacity;
    size_t header_bytes;
    char error_body[S3_ERROR_BODY_LIMIT + 1];
    size_t error_body_size;
};

void s3_error_clear(struct s3_error *error);
enum s3_result s3_error_set(struct s3_error *error, enum s3_result result,
                            const char *message);
char *s3_strdup(const char *value);
void s3_secure_free(char *value);

enum s3_result s3_build_url(const struct s3_client *client, const char *bucket,
                            const char *key, char **url,
                            struct s3_error *error);
enum s3_result s3_build_service_url(const struct s3_client *client, char **url,
                                    struct s3_error *error);
enum s3_result s3_build_bucket_url(const struct s3_client *client,
                                   const char *bucket, const char *query,
                                   char **url, struct s3_error *error);
enum s3_result s3_build_object_url(const struct s3_client *client,
                                   const char *bucket, const char *key,
                                   const char *query, char **url,
                                   struct s3_error *error);
char *s3_uri_encode(const unsigned char *input, bool keep_slash);

void s3_response_reset(struct s3_response *response);
void s3_response_cleanup(struct s3_response *response);
size_t s3_header_callback(char *buffer, size_t size, size_t count, void *data);
void s3_parse_error_xml(const char *body, size_t size, struct s3_error *error);
enum s3_result s3_parse_bucket_list(const char *body, size_t size,
                                    s3_bucket_callback callback, void *data,
                                    struct s3_error *error);
enum s3_result s3_result_from_response(CURLcode code,
                                       const struct s3_response *response,
                                       bool callback_failed,
                                       struct s3_error *error);
bool s3_is_retryable(CURLcode code, long status, const char *s3_code);
void s3_retry_delay(unsigned attempt);

enum s3_result s3_prepare_request(struct s3_client *client, const char *url,
                                  struct curl_slist **headers,
                                  struct s3_error *error);
bool s3_add_header(struct curl_slist **headers, const char *name,
                   const char *value);

#endif
