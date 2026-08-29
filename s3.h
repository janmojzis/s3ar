#ifndef S3_H____
#define S3_H____

#include <libs3.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct s3 {
    char *host;
    const char *region;
    S3Protocol protocol;
    S3UriStyle uri_style;
    const char *access_key;
    const char *secret_key;
};

struct s3_bucket {
    const char *name;
    int64_t creation_date;
    const char *acl;
};

struct s3_metadata {
    const char *name;
    const char *value;
};

struct s3_object {
    const char *bucket;
    const char *key;
    uint64_t size;
    int64_t last_modified;
    const char *etag;
    const struct s3_metadata *metadata;
    size_t metadata_count;
};

/* Structures and strings passed to callbacks are valid only for the duration
 * of the callback invocation. */
typedef void (*s3_bucket_callback)(const struct s3_bucket *bucket,
                                   void *callback_data);
typedef void (*s3_object_callback)(const struct s3_object *object,
                                   void *callback_data);
typedef S3Status (*s3_object_properties_callback)(
    const struct s3_object *object, void *callback_data);
typedef S3Status (*s3_object_data_callback)(int size, const char *data,
                                            void *callback_data);

void s3_open(const struct s3 *s3);
void s3_close(void);

void s3_bucket_check(const struct s3 *s3, const char *bucket);
void s3_bucket_acl(const struct s3 *s3, const char *bucket,
                   s3_bucket_callback callback, void *callback_data);
void s3_bucket_list(const struct s3 *s3, s3_bucket_callback callback,
                    void *callback_data);

bool s3_object_head(const struct s3 *s3, const char *bucket, const char *key,
                    s3_object_callback callback, void *callback_data);
size_t s3_object_list_prefix(const struct s3 *s3, const char *bucket,
                             const char *prefix, s3_object_callback callback,
                             void *callback_data);
void s3_object_get(const struct s3 *s3, const char *bucket, const char *key,
                   s3_object_properties_callback properties_callback,
                   s3_object_data_callback data_callback,
                   void *callback_data);

#endif
