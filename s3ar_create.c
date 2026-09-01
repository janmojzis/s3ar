/*
 * Create a POSIX PAX archive from selected buckets and objects in an
 * S3-compatible store. Object bodies are streamed directly from S3 GET
 * requests into an uncompressed or zstd-compressed archive.
 *
 * S3 metadata is stored in PAX SCHILY extended attributes:
 * SCHILY.xattr.user.s3ar.format identifies the namespaced layout,
 * SCHILY.xattr.user.s3ar.bucket and .key preserve URL-encoded S3 names,
 * SCHILY.xattr.user.s3ar.bucket-acl records a bucket ACL summary,
 * and SCHILY.xattr.user.s3ar.metadata.NAME preserves S3 user metadata for
 * later extraction. Unsafe object keys are rejected.
 *
 * SPDX-License-Identifier: MIT-0
 */

#include "die.h"
#include "fsyncfile.h"
#include "log.h"
#include "s3.h"
#include "s3ar.h"

#include <archive.h>
#include <archive_entry.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct bucket_names {
    char **names;
    size_t count;
    size_t capacity;
};

struct create_context {
    const struct s3ar_config *config;
    struct archive *archive;
    struct bucket_names buckets;
};

struct get_context {
    struct create_context *create;
    const char *bucket;
    const char *key;
    uint64_t expected;
    uint64_t written;
    bool header_written;
};

static _Noreturn void archive_fatal(struct archive *archive,
                                    const char *message) {
    errno = 0;
    die_fatal(message, archive_error_string(archive), NULL);
}

static bool bucket_written(const struct bucket_names *buckets,
                           const char *name) {
    for (size_t i = 0; i < buckets->count; ++i) {
        if (strcmp(buckets->names[i], name) == 0) { return true; }
    }
    return false;
}

static void remember_bucket(struct bucket_names *buckets, const char *name) {
    if (buckets->count == buckets->capacity) {
        size_t capacity = buckets->capacity == 0 ? 8 : buckets->capacity * 2;
        if (capacity < buckets->capacity ||
            capacity > SIZE_MAX / sizeof(*buckets->names)) {
            errno = ENOMEM;
            die_fatal("s3ar: out of memory", NULL, NULL);
        }
        char **names = realloc(buckets->names, capacity * sizeof(*names));
        if (names == NULL) { die_fatal("s3ar: out of memory", NULL, NULL); }
        buckets->names = names;
        buckets->capacity = capacity;
    }
    char *copy = strdup(name);
    if (copy == NULL) { die_fatal("s3ar: out of memory", NULL, NULL); }
    buckets->names[buckets->count++] = copy;
}

static void free_buckets(struct bucket_names *buckets) {
    for (size_t i = 0; i < buckets->count; ++i) { free(buckets->names[i]); }
    free(buckets->names);
}

static char *object_path(const char *bucket, const char *key) {
    size_t bucket_length = strlen(bucket);
    size_t key_length = strlen(key);
    if (bucket_length > SIZE_MAX - key_length - 2) {
        errno = ENOMEM;
        die_fatal("s3ar: out of memory", NULL, NULL);
    }
    char *path = malloc(bucket_length + key_length + 2);
    if (path == NULL) { die_fatal("s3ar: out of memory", NULL, NULL); }
    memcpy(path, bucket, bucket_length);
    path[bucket_length] = '/';
    memcpy(path + bucket_length + 1, key, key_length + 1);
    return path;
}

static void add_encoded_name(struct archive_entry *entry, const char *xattr,
                             const char *name) {
    char *encoded = url_encode_name(name);
    if (encoded == NULL) {
        archive_entry_free(entry);
        die_fatal("s3ar: out of memory", NULL, NULL);
    }
    archive_entry_xattr_add_entry(entry, xattr, encoded, strlen(encoded));
    free(encoded);
}

static bool write_bucket_acl(void *callback_data,
                             const struct s3_bucket *bucket) {
    struct create_context *context = callback_data;
    size_t length = strlen(bucket->name);
    if (length == SIZE_MAX) {
        errno = ENOMEM;
        die_fatal("s3ar: out of memory", NULL, NULL);
    }
    char *path = malloc(length + 2);
    struct archive_entry *entry = archive_entry_new();
    if (path == NULL || entry == NULL) {
        free(path);
        archive_entry_free(entry);
        die_fatal("s3ar: out of memory", NULL, NULL);
    }
    memcpy(path, bucket->name, length);
    path[length] = '/';
    path[length + 1] = '\0';
    /* Path names are UTF-8 bytes; the writer's BINARY header charset keeps
     * libarchive from converting them through the process locale. */
    archive_entry_set_pathname(entry, path);
    archive_entry_set_filetype(entry, AE_IFDIR);
    archive_entry_set_perm(entry, 0700);
    archive_entry_set_uid(entry, 0);
    archive_entry_set_gid(entry, 0);
    archive_entry_set_size(entry, 0);
    archive_entry_set_mtime(entry, 0, 0);
    const char *acl = bucket->acl != NULL ? bucket->acl : "unavailable";
    archive_entry_xattr_add_entry(entry, "user.s3ar.format", "1", 1);
    add_encoded_name(entry, "user.s3ar.bucket", bucket->name);
    archive_entry_xattr_add_entry(entry, "user.s3ar.bucket-acl", acl,
                                  strlen(acl));
    if (archive_write_header(context->archive, entry) != ARCHIVE_OK) {
        archive_entry_free(entry);
        free(path);
        archive_fatal(context->archive, "s3ar: cannot write bucket header");
    }
    if (archive_write_finish_entry(context->archive) != ARCHIVE_OK) {
        archive_entry_free(entry);
        free(path);
        archive_fatal(context->archive, "s3ar: cannot finish bucket entry");
    }
    archive_entry_free(entry);
    free(path);
    if (context->config->verbose) {
        log_s3_name(stderr, bucket->name, NULL);
    }
    return true;
}

static void ensure_bucket(struct create_context *context, const char *bucket) {
    if (bucket_written(&context->buckets, bucket)) { return; }
    struct s3_error error;
    enum s3_result result =
        s3_bucket_acl(context->config->s3, &error, bucket, write_bucket_acl,
                      context);
    if (result != S3_RESULT_OK) {
        die_s3fatal("s3ar: unable to read bucket ACL", bucket, NULL, result,
                    &error);
    }
    remember_bucket(&context->buckets, bucket);
}

static bool write_object_header(
    void *callback_data, const struct s3_object_properties *object) {
    struct get_context *get = callback_data;
    if (object->size > INT64_MAX) {
        errno = 0;
        die_fatal("s3ar: oversized S3 object", get->bucket, get->key);
    }
    char *path = object_path(get->bucket, get->key);
    struct archive_entry *entry = archive_entry_new();
    if (entry == NULL) {
        free(path);
        die_fatal("s3ar: out of memory", NULL, NULL);
    }
    archive_entry_set_pathname(entry, path);
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0600);
    archive_entry_set_uid(entry, 0);
    archive_entry_set_gid(entry, 0);
    archive_entry_set_size(entry, (la_int64_t) object->size);
    archive_entry_set_mtime(entry,
                            object->last_modified >= 0
                                ? (time_t) object->last_modified
                                : (time_t) 0,
                            0);
    archive_entry_xattr_add_entry(entry, "user.s3ar.format", "1", 1);
    add_encoded_name(entry, "user.s3ar.bucket", get->bucket);
    add_encoded_name(entry, "user.s3ar.key", get->key);
    for (size_t i = 0; i < object->metadata_count; ++i) {
        const char *name = object->metadata[i].name;
        const char *value = object->metadata[i].value;
        if (name == NULL) { continue; }
        if (value == NULL) { value = ""; }
        size_t name_length = strlen(name);
        if (name_length > SIZE_MAX - sizeof("user.s3ar.metadata.")) {
            archive_entry_free(entry);
            free(path);
            errno = ENOMEM;
            die_fatal("s3ar: out of memory", NULL, NULL);
        }
        char *xattr = malloc(sizeof("user.s3ar.metadata.") + name_length);
        if (xattr == NULL) {
            archive_entry_free(entry);
            free(path);
            die_fatal("s3ar: out of memory", NULL, NULL);
        }
        memcpy(xattr, "user.s3ar.metadata.",
               sizeof("user.s3ar.metadata.") - 1);
        memcpy(xattr + sizeof("user.s3ar.metadata.") - 1, name,
               name_length + 1);
        archive_entry_xattr_add_entry(entry, xattr, value, strlen(value));
        free(xattr);
    }
    if (archive_write_header(get->create->archive, entry) != ARCHIVE_OK) {
        archive_entry_free(entry);
        free(path);
        archive_fatal(get->create->archive,
                      "s3ar: cannot write object header");
    }
    archive_entry_free(entry);
    free(path);
    get->expected = object->size;
    get->header_written = true;
    return true;
}

static bool write_object_data(void *callback_data,
                              const unsigned char *data, size_t size) {
    struct get_context *get = callback_data;
    if (!get->header_written || (uint64_t) size > get->expected - get->written) {
        errno = EIO;
        return false;
    }
    la_ssize_t written = archive_write_data(get->create->archive, data, size);
    if (written < 0 || (size_t) written != size) {
        archive_fatal(get->create->archive,
                      "s3ar: cannot write object data");
    }
    get->written += (uint64_t) size;
    return true;
}

static bool write_object(struct create_context *context, const char *bucket,
                         const char *key, bool optional) {
    if (!s3ar_key_is_safe(key)) {
        errno = 0;
        die_fatal("s3ar: unsafe S3 key", bucket, key);
    }
    struct get_context get = {
        .create = context,
        .bucket = bucket,
        .key = key,
    };
    struct s3_error error;
    enum s3_result result = s3_object_get(
        context->config->s3, &error, write_object_header, write_object_data,
        &get, bucket, key);
    if (optional && result == S3_RESULT_NOT_FOUND && !get.header_written) {
        return false;
    }
    if (result != S3_RESULT_OK) {
        die_s3fatal("s3ar: unable to read object", bucket, key, result,
                    &error);
    }
    if (!get.header_written || get.written != get.expected) {
        errno = 0;
        die_fatal("s3ar: incomplete S3 object", bucket, key);
    }
    if (archive_write_finish_entry(context->archive) != ARCHIVE_OK) {
        archive_fatal(context->archive, "s3ar: cannot finish object entry");
    }
    if (context->config->verbose) { log_s3_name(stderr, bucket, key); }
    return true;
}

static bool write_listed_object(void *callback_data,
                                const struct s3_object *object) {
    struct create_context *context = callback_data;
    (void) write_object(context, object->bucket, object->key, false);
    return true;
}

static bool write_all_bucket(void *callback_data,
                             const struct s3_bucket *bucket) {
    struct create_context *context = callback_data;
    ensure_bucket(context, bucket->name);
    struct s3_error error;
    enum s3_result result = s3_object_list(
        context->config->s3, &error, bucket->name, NULL, write_listed_object,
        context, NULL);
    if (result != S3_RESULT_OK) {
        die_s3fatal("s3ar: unable to list objects", bucket->name, NULL,
                    result, &error);
    }
    return true;
}

static void write_selection(struct create_context *context,
                            const struct s3ar_selection *selection) {
    struct s3_client *s3 = context->config->s3;
    struct s3_error error;
    enum s3_result result;
    if (selection->bucket == NULL) {
        result = s3_bucket_list(s3, &error, write_all_bucket, context);
        if (result != S3_RESULT_OK) {
            die_s3fatal("s3ar: unable to list buckets", NULL, NULL, result,
                        &error);
        }
        return;
    }
    if (selection->key != NULL && !s3ar_key_is_safe(selection->key)) {
        errno = 0;
        die_fatal("s3ar: unsafe S3 key", selection->bucket,
                  selection->key);
    }
    ensure_bucket(context, selection->bucket);
    if (selection->key == NULL) {
        result = s3_object_list(s3, &error, selection->bucket, NULL,
                                write_listed_object, context, NULL);
        if (result != S3_RESULT_OK) {
            die_s3fatal("s3ar: unable to list objects", selection->bucket,
                        NULL, result, &error);
        }
        return;
    }

    bool found = write_object(context, selection->bucket, selection->key,
                              true);
    size_t length = strlen(selection->key);
    if (length > SIZE_MAX - 2) {
        errno = ENOMEM;
        die_fatal("s3ar: out of memory", NULL, NULL);
    }
    char *prefix = malloc(length + 2);
    if (prefix == NULL) { die_fatal("s3ar: out of memory", NULL, NULL); }
    memcpy(prefix, selection->key, length);
    prefix[length] = '/';
    prefix[length + 1] = '\0';
    size_t descendants = 0;
    result = s3_object_list(s3, &error, selection->bucket, prefix,
                            write_listed_object, context, &descendants);
    free(prefix);
    if (result != S3_RESULT_OK) {
        die_s3fatal("s3ar: unable to list objects", selection->bucket, NULL,
                    result, &error);
    }
    if (!found && descendants == 0) {
        errno = 0;
        die_fatal("s3ar: not found", selection->uri, NULL);
    }
}

void s3ar_create(const struct s3ar_config *config) {
    int fd = STDOUT_FILENO;
    bool close_fd = config->archive_path != NULL &&
                    strcmp(config->archive_path, "-") != 0;
    if (close_fd) {
        fd = open(config->archive_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd < 0) {
            die_fatal("s3ar: cannot create archive", config->archive_path,
                      NULL);
        }
    }

    struct archive *archive = archive_write_new();
    if (archive == NULL) { die_fatal("s3ar: out of memory", NULL, NULL); }
    if (archive_write_set_format_pax_restricted(archive) != ARCHIVE_OK ||
        /* S3 names are already UTF-8.  Preserve their bytes without a
         * locale-dependent character-set conversion. */
        archive_write_set_options(
            archive, "xattrheader=SCHILY,hdrcharset=BINARY") !=
            ARCHIVE_OK ||
        (config->zstd &&
         archive_write_add_filter_zstd(archive) != ARCHIVE_OK) ||
        archive_write_open_fd(archive, fd) != ARCHIVE_OK) {
        archive_fatal(archive, "s3ar: cannot open archive");
    }

    struct create_context context = {
        .config = config,
        .archive = archive,
    };
    for (int i = 0; i < config->operand_count; ++i) {
        struct s3ar_selection selection;
        s3ar_selection_parse(&selection, config->operands[i]);
        write_selection(&context, &selection);
        s3ar_selection_free(&selection);
    }
    free_buckets(&context.buckets);

    if (archive_write_close(archive) != ARCHIVE_OK) {
        archive_fatal(archive, "s3ar: cannot finish archive");
    }
    if (archive_write_free(archive) != ARCHIVE_OK) {
        errno = 0;
        die_fatal("s3ar: cannot free archive writer", NULL, NULL);
    }
    if (close_fd && fsyncfile(fd) != 0) {
        die_fatal("s3ar: cannot sync archive", config->archive_path, NULL);
    }
    if (close_fd && close(fd) != 0) {
        die_fatal("s3ar: cannot close archive", config->archive_path, NULL);
    }
}
