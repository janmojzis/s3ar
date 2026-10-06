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
    S3_RESULT_ERROR,
    S3_RESULT_NOT_MODIFIED
};

enum s3_uri_style { S3_URI_STYLE_PATH = 0, S3_URI_STYLE_VIRTUAL };

struct s3_client_config {
    /* All strings are borrowed during s3_client_open() and copied on success.
     */
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

void s3_config_init(struct s3_client_config *config);

struct s3_error {
    /* The return value is authoritative. Public operations clear this struct
     * on entry; on failure result matches the return value. HTTP fields are
     * populated only when a response was received. */
    enum s3_result result;
    long http_status;
    unsigned attempts;
    int callback_errno;
    char s3_code[64];
    char request_id[128];
    char message[256];
    char bucket_region[128];
    /* Multipart cleanup result; OK also means no abort was needed or tried.
     * A cleanup failure does not replace the primary result above. */
    enum s3_result abort_result;
};

/* Maximum user metadata fields accepted by the client. */
enum { S3_METADATA_LIMIT = 128 };

struct s3_metadata {
    const char *name;
    const char *value;
};

struct s3_object_properties {
    /* PUT borrows all strings and metadata for the call. HEAD returns owned
     * strings and metadata, released with s3_object_properties_free().
     * GET properties callbacks borrow them only during the callback. */
    uint64_t size;
    int64_t last_modified;
    char etag[256];
    /* NULL means the response did not include this property. */
    const char *content_type;
    const char *content_encoding;
    const char *cache_control;
    const struct s3_metadata *metadata;
    size_t metadata_count;
    const char *content_disposition;
    const char *content_language;
    const char *expires;
};

enum { S3_MULTIPART_PART_SIZE = 16 * 1024 * 1024 };
#define S3_MULTIPART_MAX_PART_SIZE (UINT64_C(5) * 1024 * 1024 * 1024)

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

struct s3_uri {
    char *bucket;
    char *key;
};

/* Fixed-size storage for a path-style bucket and an S3 object key. */
struct s3_uri_buffer {
    char bucket[256]; /*  255B + '\0' */
    char key[1025];   /* 1024B + '\0' */
};

/* Allocates bucket and key on success; release both with s3_uri_free(). */
enum s3_result s3_uri_parse_alloc(const char *text, struct s3_uri *uri,
                                  struct s3_error *error);
void s3_uri_free(struct s3_uri *uri);
/* Writes into caller-owned storage; returns an empty uri on failure. */
enum s3_result s3_uri_parse_into(const char *text, struct s3_uri_buffer *uri,
                                 struct s3_error *error);

struct s3_client;

bool s3_url_key_valid(const char *key);
/* Validates names without creating a client. HTTPS virtual-host addressing
 * additionally rejects dots in bucket names. */
enum s3_result
s3_url_validate_object_name_for_style(enum s3_uri_style uri_style, bool https,
                                      const char *bucket, const char *key,
                                      struct s3_error *error);
enum s3_result s3_url_validate_object_name(const struct s3_client *client,
                                           const char *bucket, const char *key,
                                           struct s3_error *error);

/* Returns a newly allocated URL-encoded string, or NULL for invalid input or
 * allocation failure. The caller frees it. */
char *s3_uri_encode_alloc(const char *input, bool keep_slash);

/* Capacity for an encoded 1024-byte S3 key, including the trailing NUL. */
enum { S3_URI_ENCODED_MAX_BYTES = 3 * 1024 + 1 };

/* output and input must not overlap. Returns false without changing output
 * for NULL inputs or insufficient capacity (including the terminating NUL).
 */
bool s3_uri_encode_into(char *output, size_t capacity, const char *input,
                        bool keep_slash);

/* s3_client.c */

/* Each successful open owns one independent curl handle and one balanced
 * libcurl global initialization. Close each client after its last operation.
 * Other curl users may maintain their own balanced global initialization.
 * A client may be used by only one operation at a time; separate clients may
 * be used concurrently. Callbacks must not start another operation on the
 * same client, except bucket and object callbacks passed to s3_bucket_list()
 * and s3_object_list(): these run after the listing response has been fully
 * received and parsed and may synchronously perform nested operations on the
 * same client. Use a
 * separate s3_error for each nested operation. Do not close or reconfigure a
 * client during an operation. On libcurl builds without
 * CURL_VERSION_THREADSAFE, open and close clients only while the process has a
 * single thread. */
enum s3_result s3_client_open(struct s3_client **client, struct s3_error *error,
                              const struct s3_client_config *config);
void s3_client_close(struct s3_client *client);

typedef bool (*s3_cancel_callback)(void *data);
/* The callback and data are borrowed until replaced or the client is closed.
 * A true result cancels transfers, paging and retry waits. Multipart PUT
 * attempts to abort an unfinished upload after cancellation. */
void s3_client_set_cancel_callback(struct s3_client *client,
                                   s3_cancel_callback callback, void *data);

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

/* Callback data is borrowed for the duration of the call. Output pointers
 * passed to callbacks are valid only until the callback returns. Returning
 * false (or S3_READ_ERROR) reports S3_RESULT_CALLBACK_ERROR; callback_errno
 * records errno when available. Retry may invoke listing callbacks again;
 * GET network retries resume at the first byte not previously accepted.
 * PUT reads are consumed once; a failed read is not replayed. */

enum s3_result s3_bucket_list(struct s3_client *client, struct s3_error *error,
                              s3_bucket_callback callback, void *data);
enum s3_result s3_bucket_head(struct s3_client *client, struct s3_error *error,
                              const char *bucket);
enum s3_result s3_bucket_create(struct s3_client *client,
                                struct s3_error *error, const char *bucket);
enum s3_result s3_bucket_ensure(struct s3_client *client,
                                struct s3_error *error, const char *bucket);
enum s3_result s3_bucket_delete(struct s3_client *client,
                                struct s3_error *error, const char *bucket);
enum s3_result s3_bucket_acl(struct s3_client *client, struct s3_error *error,
                             const char *bucket, s3_bucket_callback callback,
                             void *data);

enum s3_result s3_object_list(struct s3_client *client, struct s3_error *error,
                              const char *bucket, const char *prefix,
                              s3_object_callback callback, void *data,
                              size_t *count);

/* One owned page of versions or multipart uploads. An item's id is a version
 * ID for s3_listing_versions_page() and an upload ID for
 * s3_listing_uploads_page(). Pass a zero-initialized page and free it with
 * s3_listing_page_free() before fetching another page into the same object. */
struct s3_listing_item {
    char *key;
    char *id;
    bool delete_marker;
};

struct s3_listing_page {
    struct s3_listing_item *items;
    size_t count;
    bool truncated;
    char *next_key;
    char *next_id;
};

enum s3_result s3_listing_versions_page(struct s3_client *client,
                                        struct s3_error *error,
                                        const char *bucket, const char *prefix,
                                        const char *key_marker,
                                        const char *version_marker,
                                        struct s3_listing_page *page);
enum s3_result s3_listing_uploads_page(struct s3_client *client,
                                       struct s3_error *error,
                                       const char *bucket, const char *prefix,
                                       const char *key_marker,
                                       const char *upload_marker,
                                       struct s3_listing_page *page);
void s3_listing_page_free(struct s3_listing_page *page);

enum { S3_DELETE_BATCH_LIMIT = 1000 };

struct s3_object_version_ref {
    const char *key;
    const char *version_id;
};

struct s3_object_delete_result {
    bool deleted;
    char *code;
    char *message;
};

enum s3_result s3_object_delete_version(struct s3_client *client,
                                        struct s3_error *error,
                                        const char *bucket, const char *key,
                                        const char *version_id);
/* A valid batch response returns OK even if individual results contain errors.
 * Each result corresponds to the input at the same index and must be freed.
 * Targets whose key or version ID cannot be represented in XML 1.0 are
 * deleted with individual URL-encoded DELETE requests. A request failure
 * returns an error with empty results; some targets may already be deleted.
 * With a count from 1 through S3_DELETE_BATCH_LIMIT and output storage for
 * that many results, results are initialized even on argument errors and
 * remain empty on failure, so they can be passed to the free call. A NULL
 * results pointer or a count outside that range is rejected without writing
 * to results. Free any previous results before reusing the output storage. */
enum s3_result s3_object_delete_batch(struct s3_client *client,
                                      struct s3_error *error,
                                      const char *bucket,
                                      const struct s3_object_version_ref *items,
                                      size_t count,
                                      struct s3_object_delete_result *results);
void s3_object_delete_results_free(struct s3_object_delete_result *results,
                                   size_t count);

enum s3_result s3_multipart_abort(struct s3_client *client,
                                  struct s3_error *error, const char *bucket,
                                  const char *key, const char *upload_id);

enum s3_result s3_object_head(struct s3_client *client, struct s3_error *error,
                              struct s3_object_properties *properties,
                              const char *bucket, const char *key);

/* GET retries resume at the first byte not accepted by write_callback. */
enum s3_result s3_object_get(struct s3_client *client, struct s3_error *error,
                             s3_properties_callback properties_callback,
                             s3_write_callback write_callback, void *data,
                             const char *bucket, const char *key);
/* A matching initial If-None-Match response returns NOT_MODIFIED without
 * invoking either callback. Resumed transfers use Range and If-Match. */
enum s3_result
s3_object_get_conditional(struct s3_client *client, struct s3_error *error,
                          s3_properties_callback properties_callback,
                          s3_write_callback write_callback, void *data,
                          const char *bucket, const char *key,
                          const char *if_none_match);

/* PUT for a known length. Objects exceeding 10,000 parts of 5 GiB are
 * rejected before allocating the part buffer or invoking read_callback. */
enum s3_result s3_object_put(struct s3_client *client, struct s3_error *error,
                             const char *bucket, const char *key, uint64_t size,
                             const struct s3_object_properties *properties,
                             s3_read_callback read_callback, void *data);

/* Multipart PUT for inputs whose length is not known in advance. */
enum s3_result
s3_object_put_stream(struct s3_client *client, struct s3_error *error,
                     const char *bucket, const char *key, size_t part_size,
                     const struct s3_object_properties *properties,
                     s3_read_callback read_callback, void *data);

/* Server-side copy. Nonempty objects use multipart UploadPartCopy. */
enum s3_result s3_object_copy(struct s3_client *client, struct s3_error *error,
                              const char *source_bucket, const char *source_key,
                              const char *destination_bucket,
                              const char *destination_key, size_t part_size);

/* Releases metadata and HTTP properties allocated by s3_object_head(). Call
 * only for a HEAD output, never for caller-provided PUT properties. Safe for
 * a zero-initialized object or an unsuccessful HEAD output. */
void s3_object_properties_free(struct s3_object_properties *properties);

const char *s3_result_name(enum s3_result result);

#endif
