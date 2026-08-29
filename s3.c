/* SPDX-License-Identifier: MIT-0 */

#include "die.h"
#include "s3.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { LIST_PAGE_SIZE = 512 };

struct s3_request {
    const char *error_text;
    const char *bucket;
    const char *key;
};

struct owned_bucket {
    char *name;
    int64_t creation_date;
};

struct bucket_list {
    struct s3_request request;
    struct owned_bucket *buckets;
    size_t count;
    size_t capacity;
};

struct object_page {
    struct s3_request request;
    const char *bucket;
    s3_object_callback callback;
    void *callback_data;
    bool truncated;
    char *next_marker;
    size_t count;
};

struct object_head {
    struct s3_request request;
    bool exists;
    uint64_t size;
    int64_t last_modified;
    char *etag;
    struct s3_metadata *metadata;
    size_t metadata_count;
};

struct acl_response {
    S3Status status;
};

static S3Status ignore_properties(const S3ResponseProperties *properties,
                                  void *callback_data) {
    (void) properties;
    (void) callback_data;
    return S3StatusOK;
}

static void request_complete(S3Status status, const S3ErrorDetails *details,
                             void *callback_data) {
    const struct s3_request *request = callback_data;
    if (status != S3StatusOK) {
        die_s3fatal(request->error_text, request->bucket, request->key, status,
                    details);
    }
}

static const S3ResponseHandler response_handler = {
    .propertiesCallback = ignore_properties,
    .completeCallback = request_complete,
};

static S3Status collect_head(const S3ResponseProperties *properties,
                             void *callback_data) {
    struct object_head *head = callback_data;
    head->size = properties->contentLength;
    head->last_modified = properties->lastModified;
    if (properties->eTag != NULL) {
        head->etag = strdup(properties->eTag);
        if (head->etag == NULL) {
            die_fatal("s3ar: out of memory", NULL, NULL);
        }
    }
    if (properties->metaDataCount > 0) {
        head->metadata = calloc((size_t) properties->metaDataCount,
                                sizeof(*head->metadata));
        if (head->metadata == NULL) {
            die_fatal("s3ar: out of memory", NULL, NULL);
        }
    }
    for (int i = 0; i < properties->metaDataCount; ++i) {
        const char *name = properties->metaData[i].name;
        const char *value = properties->metaData[i].value;
        if (name == NULL) { continue; }
        if (value == NULL) { value = ""; }
        char *name_copy = strdup(name);
        char *value_copy = strdup(value);
        if (name_copy == NULL || value_copy == NULL) {
            free(name_copy);
            free(value_copy);
            die_fatal("s3ar: out of memory", NULL, NULL);
        }
        head->metadata[head->metadata_count++] = (struct s3_metadata) {
            .name = name_copy,
            .value = value_copy,
        };
    }
    return S3StatusOK;
}

static int compare_metadata(const void *left, const void *right) {
    const struct s3_metadata *a = left;
    const struct s3_metadata *b = right;
    int result = strcmp(a->name, b->name);
    return result != 0 ? result : strcmp(a->value, b->value);
}

static void free_metadata(struct object_head *head) {
    for (size_t i = 0; i < head->metadata_count; ++i) {
        free((char *) head->metadata[i].name);
        free((char *) head->metadata[i].value);
    }
    free(head->metadata);
}

static void head_complete(S3Status status, const S3ErrorDetails *details,
                          void *callback_data) {
    struct object_head *head = callback_data;
    if (status == S3StatusOK) { head->exists = true; }
    else if (status != S3StatusErrorNoSuchKey &&
             status != S3StatusHttpErrorNotFound) {
        die_s3fatal(head->request.error_text, head->request.bucket,
                    head->request.key, status, details);
    }
}

static const S3ResponseHandler head_handler = {
    .propertiesCallback = collect_head,
    .completeCallback = head_complete,
};

static void acl_complete(S3Status status, const S3ErrorDetails *details,
                         void *callback_data) {
    (void) details;
    ((struct acl_response *) callback_data)->status = status;
}

static const S3ResponseHandler acl_handler = {
    .propertiesCallback = ignore_properties,
    .completeCallback = acl_complete,
};

static bool group_permission(S3Permission permission, bool *read,
                             bool *write) {
    switch (permission) {
        case S3PermissionRead:
            *read = true;
            return true;
        case S3PermissionWrite:
            *write = true;
            return true;
        case S3PermissionFullControl:
            *read = true;
            *write = true;
            return true;
        case S3PermissionReadACP:
        case S3PermissionWriteACP:
            return false;
    }
    return false;
}

static char *format_acl(bool public_read, bool public_write,
                        bool authenticated_read, bool authenticated_write,
                        bool custom) {
    if (!public_read && !public_write && !authenticated_read &&
        !authenticated_write && !custom) {
        return strdup("private");
    }
    const char *values[5];
    size_t count = 0;
    if (public_read) { values[count++] = "public-read"; }
    if (public_write) { values[count++] = "public-write"; }
    if (authenticated_read) { values[count++] = "authenticated-read"; }
    if (authenticated_write) { values[count++] = "authenticated-write"; }
    if (custom) { values[count++] = "custom"; }

    size_t length = 0;
    for (size_t i = 0; i < count; ++i) {
        size_t value_length = strlen(values[i]);
        if (length > SIZE_MAX - value_length - (i > 0 ? 1 : 0)) {
            errno = ENOMEM;
            die_fatal("s3ar: out of memory", NULL, NULL);
        }
        length += value_length + (i > 0 ? 1 : 0);
    }
    char *summary = malloc(length + 1);
    if (summary == NULL) {
        die_fatal("s3ar: out of memory", NULL, NULL);
    }
    size_t offset = 0;
    for (size_t i = 0; i < count; ++i) {
        if (i > 0) { summary[offset++] = ','; }
        size_t value_length = strlen(values[i]);
        memcpy(summary + offset, values[i], value_length);
        offset += value_length;
    }
    summary[offset] = '\0';
    return summary;
}

static void append_bucket(struct bucket_list *list, const char *name,
                          int64_t creation_date) {
    if (list->count == list->capacity) {
        size_t capacity = list->capacity == 0 ? 8 : list->capacity * 2;
        if (capacity < list->capacity ||
            capacity > SIZE_MAX / sizeof(*list->buckets)) {
            errno = ENOMEM;
            die_fatal("s3ar: out of memory", NULL, NULL);
        }
        struct owned_bucket *buckets =
            realloc(list->buckets, capacity * sizeof(*buckets));
        if (buckets == NULL) { die_fatal("s3ar: out of memory", NULL, NULL); }
        list->buckets = buckets;
        list->capacity = capacity;
    }
    char *copy = strdup(name);
    if (copy == NULL) { die_fatal("s3ar: out of memory", NULL, NULL); }
    list->buckets[list->count++] = (struct owned_bucket) {
        .name = copy,
        .creation_date = creation_date,
    };
}

static S3Status collect_bucket(const char *owner_id,
                               const char *owner_display_name,
                               const char *bucket_name, int64_t creation_date,
                               void *callback_data) {
    struct bucket_list *list = callback_data;
    (void) owner_id;
    (void) owner_display_name;
    append_bucket(list, bucket_name, creation_date);
    return S3StatusOK;
}

static const S3ListServiceHandler service_handler = {
    .responseHandler =
        {
            .propertiesCallback = ignore_properties,
            .completeCallback = request_complete,
        },
    .listServiceCallback = collect_bucket,
};

static int compare_buckets(const void *left, const void *right) {
    const struct owned_bucket *a = left;
    const struct owned_bucket *b = right;
    return strcmp(a->name, b->name);
}

static void free_buckets(struct bucket_list *list) {
    for (size_t i = 0; i < list->count; ++i) { free(list->buckets[i].name); }
    free(list->buckets);
}

static void replace_marker(char **destination, const char *marker) {
    char *copy = strdup(marker);
    if (copy == NULL) { die_fatal("s3ar: out of memory", NULL, NULL); }
    free(*destination);
    *destination = copy;
}

static S3Status
emit_objects(int is_truncated, const char *next_marker, int contents_count,
             const S3ListBucketContent *contents, int common_prefixes_count,
             const char **common_prefixes, void *callback_data) {
    struct object_page *page = callback_data;
    (void) common_prefixes_count;
    (void) common_prefixes;

    for (int i = 0; i < contents_count; ++i) {
        const struct s3_object object = {
            .bucket = page->bucket,
            .key = contents[i].key,
            .size = contents[i].size,
            .last_modified = contents[i].lastModified,
            .etag = contents[i].eTag,
        };
        page->callback(&object, page->callback_data);
        if (page->count == SIZE_MAX) {
            errno = EOVERFLOW;
            die_fatal("s3ar: too many objects", page->bucket, NULL);
        }
        ++page->count;
    }

    page->truncated = is_truncated != 0;
    if (page->truncated) {
        const char *marker = next_marker;
        if ((marker == NULL || marker[0] == '\0') && contents_count > 0) {
            marker = contents[contents_count - 1].key;
        }
        if (marker == NULL || marker[0] == '\0') {
            errno = 0;
            die_fatal("s3ar: invalid pagination marker from S3", page->bucket,
                      NULL);
        }
        replace_marker(&page->next_marker, marker);
    }
    return S3StatusOK;
}

static const S3ListBucketHandler bucket_handler = {
    .responseHandler =
        {
            .propertiesCallback = ignore_properties,
            .completeCallback = request_complete,
        },
    .listBucketCallback = emit_objects,
};

static S3BucketContext bucket_context(const struct s3 *s3, const char *bucket) {
    return (S3BucketContext) {
        .hostName = s3->host,
        .bucketName = bucket,
        .protocol = s3->protocol,
        .uriStyle = s3->uri_style,
        .accessKeyId = s3->access_key,
        .secretAccessKey = s3->secret_key,
    };
}

static void validate_bucket(const struct s3 *s3, const char *bucket) {
    S3Status status = S3_validate_bucket_name(bucket, s3->uri_style);
    if (status != S3StatusOK) {
        die_s3fatal("s3ar: invalid bucket name", bucket, NULL, status, NULL);
    }
}

void s3_open(const struct s3 *s3) {
    S3Status status = S3_initialize("s3ar", S3_INIT_ALL, s3->host);
    if (status != S3StatusOK) {
        die_s3fatal("s3ar: unable to initialize libs3", NULL, NULL, status,
                    NULL);
    }
}

void s3_close(void) { S3_deinitialize(); }

void s3_bucket_check(const struct s3 *s3, const char *bucket) {
    validate_bucket(s3, bucket);
    struct s3_request request = {
        .error_text = "s3ar: unable to access bucket",
        .bucket = bucket,
    };
    char location[256] = "";
    S3_test_bucket(s3->protocol, s3->uri_style, s3->access_key, s3->secret_key,
                   s3->host, bucket, (int) sizeof(location), location, NULL,
                   &response_handler, &request);
}

void s3_bucket_acl(const struct s3 *s3, const char *bucket,
                   s3_bucket_callback callback, void *callback_data) {
    validate_bucket(s3, bucket);
    S3BucketContext context = bucket_context(s3, bucket);
    char owner_id[S3_MAX_GRANTEE_USER_ID_SIZE] = "";
    char owner_name[S3_MAX_GRANTEE_DISPLAY_NAME_SIZE] = "";
    S3AclGrant grants[S3_MAX_ACL_GRANT_COUNT];
    int grant_count = 0;
    struct acl_response response = {.status = S3StatusOK};
    S3_get_acl(&context, NULL, owner_id, owner_name, &grant_count, grants,
               NULL, &acl_handler, &response);

    char *summary = NULL;
    if (response.status != S3StatusOK) {
        summary = strdup("unavailable");
        if (summary == NULL) {
            die_fatal("s3ar: out of memory", NULL, NULL);
        }
    }
    else {
        bool public_read = false;
        bool public_write = false;
        bool authenticated_read = false;
        bool authenticated_write = false;
        bool custom = false;
        for (int i = 0; i < grant_count; ++i) {
            const S3AclGrant *grant = &grants[i];
            if (grant->granteeType == S3GranteeTypeAllUsers) {
                if (!group_permission(grant->permission, &public_read,
                                      &public_write)) {
                    custom = true;
                }
            }
            else if (grant->granteeType == S3GranteeTypeAllAwsUsers) {
                if (!group_permission(grant->permission, &authenticated_read,
                                      &authenticated_write)) {
                    custom = true;
                }
            }
            else if (grant->granteeType == S3GranteeTypeCanonicalUser &&
                     strcmp(grant->grantee.canonicalUser.id, owner_id) == 0) {
                if (grant->permission != S3PermissionFullControl) {
                    custom = true;
                }
            }
            else {
                custom = true;
            }
        }
        summary = format_acl(public_read, public_write, authenticated_read,
                             authenticated_write, custom);
    }

    const struct s3_bucket value = {
        .name = bucket,
        .creation_date = -1,
        .acl = summary,
    };
    callback(&value, callback_data);
    free(summary);
}

void s3_bucket_list(const struct s3 *s3, s3_bucket_callback callback,
                    void *callback_data) {
    struct bucket_list list = {
        .request = {.error_text = "s3ar: unable to list buckets"},
    };
    S3_list_service(s3->protocol, s3->access_key, s3->secret_key, s3->host,
                    NULL, &service_handler, &list);
    qsort(list.buckets, list.count, sizeof(*list.buckets), compare_buckets);
    for (size_t i = 0; i < list.count; ++i) {
        const struct s3_bucket bucket = {
            .name = list.buckets[i].name,
            .creation_date = list.buckets[i].creation_date,
        };
        callback(&bucket, callback_data);
    }
    free_buckets(&list);
}

bool s3_object_head(const struct s3 *s3, const char *bucket, const char *key,
                    s3_object_callback callback, void *callback_data) {
    validate_bucket(s3, bucket);
    S3BucketContext context = bucket_context(s3, bucket);
    struct object_head head = {
        .request =
            {
                .error_text = "s3ar: unable to access object",
                .bucket = bucket,
                .key = key,
            },
    };
    S3_head_object(&context, key, NULL, &head_handler, &head);
    if (!head.exists) {
        free(head.etag);
        free_metadata(&head);
        return false;
    }
    qsort(head.metadata, head.metadata_count, sizeof(*head.metadata),
          compare_metadata);
    const struct s3_object object = {
        .bucket = bucket,
        .key = key,
        .size = head.size,
        .last_modified = head.last_modified,
        .etag = head.etag,
        .metadata = head.metadata,
        .metadata_count = head.metadata_count,
    };
    callback(&object, callback_data);
    free(head.etag);
    free_metadata(&head);
    return true;
}

size_t s3_object_list_prefix(const struct s3 *s3, const char *bucket,
                             const char *prefix, s3_object_callback callback,
                             void *callback_data) {
    validate_bucket(s3, bucket);
    S3BucketContext context = bucket_context(s3, bucket);
    char *marker = NULL;
    size_t count = 0;
    for (;;) {
        struct object_page page = {
            .request =
                {
                    .error_text = "s3ar: unable to list bucket",
                    .bucket = bucket,
                },
            .bucket = bucket,
            .callback = callback,
            .callback_data = callback_data,
        };
        S3_list_bucket(&context, prefix, marker, NULL, LIST_PAGE_SIZE, NULL,
                       &bucket_handler, &page);
        if (SIZE_MAX - count < page.count) {
            errno = EOVERFLOW;
            die_fatal("s3ar: too many objects", bucket, NULL);
        }
        count += page.count;
        if (!page.truncated) {
            free(page.next_marker);
            break;
        }
        if (page.next_marker == NULL ||
            (marker != NULL && strcmp(marker, page.next_marker) == 0)) {
            errno = 0;
            die_fatal("s3ar: invalid pagination marker from S3", bucket, NULL);
        }
        free(marker);
        marker = page.next_marker;
    }
    free(marker);
    return count;
}
