/* SPDX-License-Identifier: MIT-0 */
/* Private declarations for the s3ar S3 client. */
#ifndef S3_INTERNAL_H
#define S3_INTERNAL_H

#include "s3.h"

#include <curl/curl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

enum {
    S3_ERROR_BODY_LIMIT = 64 * 1024,
    S3_XML_BODY_LIMIT = 16 * 1024 * 1024,
    S3_DEFAULT_MAX_ATTEMPTS = 8,
};

struct s3_client {
    CURL *curl;
    CURLU *url;
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
    s3_cancel_callback cancel_callback;
    void *cancel_data;
    unsigned long long trace_id;
    bool trace_active;
    unsigned trace_sent;
    unsigned trace_attempt;
    unsigned trace_max_attempts;
    struct timespec trace_started;
    char trace_request_id[128];
};

struct s3_response {
    long status;
    bool headers_done;
    bool invalid_headers;
    bool have_retry_after;
    uint64_t retry_after_ms;
    char bucket_region[128];
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

struct s3_memory_response {
    struct s3_response response;
    char *body;
    size_t size;
    size_t capacity;
    size_t limit;
    enum s3_result body_error;
};

void s3_error_clear(struct s3_error *error);
enum s3_result s3_error_set(struct s3_error *error, enum s3_result result,
                            const char *message);
char *s3_memory_strdup(const char *value);
void s3_memory_secure_free(char *value);
/* Double capacity (or use initial_capacity). Failure preserves items/capacity.
 */
void *s3_memory_grow(void *items, size_t *capacity, size_t item_size,
                     size_t initial_capacity);

/* RFC 3986 Section 2.3 unreserved octets. With keep_slash=0, '/' is
 * escaped in individual components such as bucket names and query values.
 * With keep_slash!=0, '/' remains a Section 3.3 path separator, as in an
 * object key. '/' itself is not unreserved. */
int s3_uri_encode_isliteral(unsigned char c, int keep_slash);
/* Decode a non-NULL string; '+' stays literal. Reject malformed escapes and
 * encoded NUL, and leave *decoded NULL on failure. Caller owns the result. */
enum s3_result s3_uri_decode_alloc(const char *encoded, char **decoded);

struct s3_query_param {
    const char *name;
    const char *value;
};
/* Append parameters in order to an already encoded base. Names are fixed,
 * URI-safe literals; values are encoded. NULL values are omitted, empty values
 * retained. Terminate params with a NULL name. Caller owns the result. */
char *s3_query_build(const char *base, const struct s3_query_param *params);

enum s3_result s3_url_build(const struct s3_client *client, const char *bucket,
                            const char *key, char **url,
                            struct s3_error *error);
enum s3_result s3_url_build_service(const struct s3_client *client,
                                    const char *query, char **url,
                                    struct s3_error *error);
enum s3_result s3_url_build_bucket(const struct s3_client *client,
                                   const char *bucket, const char *query,
                                   char **url, struct s3_error *error);
enum s3_result s3_url_build_object(const struct s3_client *client,
                                   const char *bucket, const char *key,
                                   const char *query, char **url,
                                   struct s3_error *error);

/* Parse decimal digits in the non-NULL range [first, last).
 * Empty input, invalid digits and overflow leave *value unchanged. */
bool s3_parse_u64(const char *first, const char *last, uint64_t *value);

void s3_response_reset(struct s3_response *response);
void s3_response_cleanup(struct s3_response *response);
/* Move properties into an empty destination. The caller owns the transferred
 * allocations; the response retains its transport and header state. */
void s3_response_take_properties(struct s3_response *response,
                                 struct s3_object_properties *properties);
enum s3_result s3_response_check_headers(const struct s3_response *response,
                                         const char *message,
                                         struct s3_error *error);
size_t s3_headers_callback(char *buffer, size_t size, size_t count, void *data);
void s3_error_parse_xml(const char *body, size_t size, struct s3_error *error);
enum s3_result s3_result_from_response(CURLcode code,
                                       const struct s3_response *response,
                                       bool callback_failed,
                                       struct s3_error *error);
bool s3_retry_allowed(CURLcode code, long status, const char *s3_code);
void s3_retry_delay(struct s3_client *client, unsigned attempt,
                    unsigned max_attempts, CURLcode code,
                    const struct s3_response *response,
                    const struct s3_error *error);
void s3_trace_perform_start(struct s3_client *client, unsigned attempt,
                            unsigned max_attempts);
void s3_trace_perform_end(struct s3_client *client, unsigned attempt,
                          unsigned max_attempts, CURLcode code,
                          const struct s3_response *response,
                          const struct s3_error *error);

enum s3_result s3_request_prepare(struct s3_client *client, const char *url,
                                  struct curl_slist **headers,
                                  struct s3_error *error);
/* Perform a prepared request and collect transport diagnostics. The caller
 * finishes tracing after any operation-specific diagnostics and applies its
 * own validation and retry policy. curl_error must be initially empty. */
CURLcode s3_request_perform(struct s3_client *client, unsigned attempt,
                            unsigned max_attempts, struct s3_response *response,
                            curl_write_callback write_callback,
                            void *write_data, char curl_error[CURL_ERROR_SIZE],
                            struct s3_error *error);
enum s3_result s3_request_result(CURLcode code,
                                 const struct s3_response *response,
                                 const char *curl_error,
                                 struct s3_error *error);
enum s3_result s3_request_bucket(struct s3_client *client,
                                 struct s3_error *error, const char *bucket,
                                 const char *query, const char *method,
                                 const char *request_body, char **response_body,
                                 size_t *response_size);
enum s3_result s3_request_url(struct s3_client *client, struct s3_error *error,
                              const char *url, const char *method,
                              const char *request_body, char **response_body,
                              size_t *response_size);
bool s3_headers_add(struct curl_slist **headers, const char *name,
                    const char *value);
void s3_response_memory_reset(struct s3_memory_response *response,
                              size_t limit);
void s3_response_memory_cleanup(struct s3_memory_response *response);
size_t s3_response_memory_collect(char *buffer, size_t size, size_t count,
                                  void *data);

#endif
