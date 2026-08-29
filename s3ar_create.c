/* SPDX-License-Identifier: MIT-0 */

#include "die.h"
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
#include <sys/stat.h>
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

static void verbose_name(const struct create_context *context,
                         const char *bucket, const char *key) {
    if (!context->config->verbose) { return; }
    int result = key == NULL ? fprintf(stderr, "%s\n", bucket)
                             : fprintf(stderr, "%s/%s\n", bucket, key);
    if (result < 0) {
        die_fatal("s3ar: unable to write verbose output", bucket, key);
    }
}

static void write_bucket_acl(const struct s3_bucket *bucket,
                             void *callback_data) {
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
    archive_entry_set_pathname_utf8(entry, path);
    archive_entry_set_filetype(entry, AE_IFDIR);
    archive_entry_set_perm(entry, 0700);
    archive_entry_set_uid(entry, 0);
    archive_entry_set_gid(entry, 0);
    archive_entry_set_size(entry, 0);
    archive_entry_set_mtime(entry, 0, 0);
    const char *acl = bucket->acl != NULL ? bucket->acl : "unavailable";
    archive_entry_xattr_add_entry(entry, "s3ar.bucket-acl", acl,
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
    verbose_name(context, bucket->name, NULL);
}

static void ensure_bucket(struct create_context *context, const char *bucket) {
    if (bucket_written(&context->buckets, bucket)) { return; }
    s3_bucket_check(&context->config->s3, bucket);
    s3_bucket_acl(&context->config->s3, bucket, write_bucket_acl, context);
    remember_bucket(&context->buckets, bucket);
}

static S3Status write_object_header(const struct s3_object *object,
                                    void *callback_data) {
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
    archive_entry_set_pathname_utf8(entry, path);
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
    if (object->etag != NULL) {
        archive_entry_xattr_add_entry(entry, "etag", object->etag,
                                      strlen(object->etag));
    }
    for (size_t i = 0; i < object->metadata_count; ++i) {
        const char *name = object->metadata[i].name;
        const char *value = object->metadata[i].value;
        if (name == NULL) { continue; }
        if (value == NULL) { value = ""; }
        size_t name_length = strlen(name);
        if (name_length > SIZE_MAX - sizeof("user.")) {
            archive_entry_free(entry);
            free(path);
            errno = ENOMEM;
            die_fatal("s3ar: out of memory", NULL, NULL);
        }
        char *xattr = malloc(sizeof("user.") + name_length);
        if (xattr == NULL) {
            archive_entry_free(entry);
            free(path);
            die_fatal("s3ar: out of memory", NULL, NULL);
        }
        memcpy(xattr, "user.", sizeof("user.") - 1);
        memcpy(xattr + sizeof("user.") - 1, name, name_length + 1);
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
    return S3StatusOK;
}

static S3Status write_object_data(int size, const char *data,
                                  void *callback_data) {
    struct get_context *get = callback_data;
    if (!get->header_written || size < 0 ||
        (uint64_t) size > get->expected - get->written) {
        return S3StatusAbortedByCallback;
    }
    la_ssize_t written = archive_write_data(get->create->archive, data,
                                            (size_t) size);
    if (written < 0 || written != size) {
        archive_fatal(get->create->archive,
                      "s3ar: cannot write object data");
    }
    get->written += (uint64_t) size;
    return S3StatusOK;
}

static void write_object(struct create_context *context, const char *bucket,
                         const char *key) {
    if (!s3ar_key_is_safe(key)) {
        errno = 0;
        die_fatal("s3ar: unsafe S3 key", bucket, key);
    }
    struct get_context get = {
        .create = context,
        .bucket = bucket,
        .key = key,
    };
    s3_object_get(&context->config->s3, bucket, key, write_object_header,
                  write_object_data, &get);
    if (!get.header_written || get.written != get.expected) {
        errno = 0;
        die_fatal("s3ar: incomplete S3 object", bucket, key);
    }
    if (archive_write_finish_entry(context->archive) != ARCHIVE_OK) {
        archive_fatal(context->archive, "s3ar: cannot finish object entry");
    }
    verbose_name(context, bucket, key);
}

static void write_listed_object(const struct s3_object *object,
                                void *callback_data) {
    struct create_context *context = callback_data;
    write_object(context, object->bucket, object->key);
}

static void write_exact_object(const struct s3_object *object,
                               void *callback_data) {
    write_listed_object(object, callback_data);
}

static void write_all_bucket(const struct s3_bucket *bucket,
                             void *callback_data) {
    struct create_context *context = callback_data;
    ensure_bucket(context, bucket->name);
    s3_object_list_prefix(&context->config->s3, bucket->name, NULL,
                          write_listed_object, context);
}

static void write_selection(struct create_context *context,
                            const struct s3ar_selection *selection) {
    const struct s3 *s3 = &context->config->s3;
    if (selection->bucket == NULL) {
        s3_bucket_list(s3, write_all_bucket, context);
        return;
    }
    if (selection->key != NULL && !s3ar_key_is_safe(selection->key)) {
        errno = 0;
        die_fatal("s3ar: unsafe S3 key", selection->bucket,
                  selection->key);
    }
    ensure_bucket(context, selection->bucket);
    if (selection->key == NULL) {
        s3_object_list_prefix(s3, selection->bucket, NULL,
                              write_listed_object, context);
        return;
    }

    bool found = s3_object_head(s3, selection->bucket, selection->key,
                                write_exact_object, context);
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
    size_t descendants = s3_object_list_prefix(
        s3, selection->bucket, prefix, write_listed_object, context);
    free(prefix);
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
        fd = open(config->archive_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) {
            die_fatal("s3ar: cannot create archive", config->archive_path,
                      NULL);
        }
    }

    struct archive *archive = archive_write_new();
    if (archive == NULL) { die_fatal("s3ar: out of memory", NULL, NULL); }
    if (archive_write_set_format_pax_restricted(archive) != ARCHIVE_OK ||
        archive_write_set_options(archive, "xattrheader=SCHILY") !=
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
    if (close_fd && fsync(fd) != 0) {
        die_fatal("s3ar: cannot sync archive", config->archive_path, NULL);
    }
    if (close_fd && close(fd) != 0) {
        die_fatal("s3ar: cannot close archive", config->archive_path, NULL);
    }
}
