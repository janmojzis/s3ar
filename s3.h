/* SPDX-License-Identifier: MIT-0 */
#ifndef S3_H
#define S3_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum s3_result {
    S3_RESULT_OK = 0,
    S3_RESULT_NOT_FOUND,
    S3_RESULT_PRECONDITION_FAILED,
    S3_RESULT_ACCESS_DENIED,
    S3_RESULT_CALLBACK_ERROR,
    S3_RESULT_RETRY_EXHAUSTED,
    S3_RESULT_PROTOCOL_ERROR,
    S3_RESULT_CONFIGURATION_ERROR,
    S3_RESULT_ERROR
};

enum s3_uri_style { S3_URI_STYLE_PATH = 0, S3_URI_STYLE_VIRTUAL };

struct s3_client_config {
    const char *endpoint;
    const char *region;
    enum s3_uri_style uri_style;
    const char *access_key;
    const char *secret_key;
    const char *session_token;
    const char *user_agent;
    unsigned max_attempts;
    unsigned connect_timeout_ms;
    unsigned low_speed_time_s;
};

struct s3_error {
    enum s3_result result;
    long http_status;
    unsigned attempts;
    int callback_errno;
    char s3_code[64];
    char request_id[128];
    char message[256];
};

struct s3_metadata {
    const char *name;
    const char *value;
};

struct s3_object_properties {
    uint64_t size;
    int64_t last_modified;
    char etag[256];
    char content_type[256];
    char content_encoding[128];
    char cache_control[256];
    const struct s3_metadata *metadata;
    size_t metadata_count;
};

/* Strings passed to callbacks are valid only during the callback. */
struct s3_bucket {
    const char *name;
    const char *acl;
};

struct s3_object {
    const char *bucket;
    const char *key;
    uint64_t size;
    int64_t last_modified;
    const char *etag;
};

/* Pointers in client remain valid until s3_config_free(). */
struct s3_config {
    struct s3_client_config client;
    char *endpoint;
    char *region;
    char *access_key;
    char *secret_key;
    char *session_token;
};

struct s3_uri {
    char *bucket;
    char *key;
};

/* s3_config.c */
enum s3_result s3_config_from_env(struct s3_config *config,
                                  struct s3_error *error);
void s3_config_free(struct s3_config *config);

enum s3_result s3_uri_parse(const char *text, struct s3_uri *uri,
                            struct s3_error *error);
void s3_uri_free(struct s3_uri *uri);

/* s3_client.c */
struct s3_client;

enum s3_result s3_client_open(struct s3_client **client, struct s3_error *error,
                              const struct s3_client_config *config);
void s3_client_close(struct s3_client *client);

typedef bool (*s3_write_callback)(void *data, const unsigned char *buffer,
                                  size_t size);
typedef bool (*s3_properties_callback)(
    void *data, const struct s3_object_properties *properties);
typedef bool (*s3_bucket_callback)(void *data, const struct s3_bucket *bucket);
typedef bool (*s3_object_callback)(void *data, const struct s3_object *object);

enum s3_read_result { S3_READ_DATA = 0, S3_READ_EOF, S3_READ_ERROR };
typedef enum s3_read_result (*s3_read_callback)(void *data,
                                                unsigned char *buffer,
                                                size_t capacity, size_t *size);

enum s3_result s3_bucket_list(struct s3_client *client, struct s3_error *error,
                              s3_bucket_callback callback, void *data);
enum s3_result s3_bucket_head(struct s3_client *client, struct s3_error *error,
                              const char *bucket);
enum s3_result s3_bucket_create(struct s3_client *client,
                                struct s3_error *error, const char *bucket);
enum s3_result s3_bucket_ensure(struct s3_client *client,
                                struct s3_error *error, const char *bucket);
enum s3_result s3_bucket_acl(struct s3_client *client, struct s3_error *error,
                             const char *bucket, s3_bucket_callback callback,
                             void *data);

enum s3_result s3_object_list(struct s3_client *client, struct s3_error *error,
                              const char *bucket, const char *prefix,
                              s3_object_callback callback, void *data,
                              size_t *count);

enum s3_result s3_object_head(struct s3_client *client, struct s3_error *error,
                              struct s3_object_properties *properties,
                              const char *bucket, const char *key);

/* GET retries resume at the first byte not accepted by write_callback. */
enum s3_result s3_object_get(struct s3_client *client, struct s3_error *error,
                             s3_properties_callback properties_callback,
                             s3_write_callback write_callback, void *data,
                             const char *bucket, const char *key);

enum s3_result s3_object_put(struct s3_client *client, struct s3_error *error,
                             const char *bucket, const char *key, uint64_t size,
                             const struct s3_object_properties *properties,
                             s3_read_callback read_callback, void *data);

/* Releases metadata allocated by s3_object_head(). */
void s3_object_properties_free(struct s3_object_properties *properties);

const char *s3_result_name(enum s3_result result);

#endif
