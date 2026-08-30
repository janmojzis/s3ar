/*
 * Restore bucket directories and object members from a POSIX PAX archive to
 * an S3-compatible store. Plain and zstd-compressed archives are supported;
 * selected object bodies are streamed directly into S3 PUT requests.
 *
 * s3ar archives store S3 metadata in PAX SCHILY extended attributes:
 * SCHILY.xattr.user.s3ar.format identifies the namespaced layout,
 * SCHILY.xattr.user.s3ar.bucket-acl and
 * SCHILY.xattr.user.s3ar.metadata.NAME. User metadata is restored to uploaded
 * objects; bucket ACL summaries are informational and are not restored.
 * Legacy SCHILY.xattr.user.NAME metadata remains readable.
 * Unsafe paths, links, and unsupported archive member types are rejected.
 *
 * SPDX-License-Identifier: MIT-0
 */

#include "die.h"
#include "log.h"
#include "s3.h"
#include "s3ar.h"

#include <archive.h>
#include <archive_entry.h>

#include <errno.h>
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

struct extract_context {
    const struct s3ar_config *config;
    struct archive *archive;
    struct s3ar_selection *selections;
    bool *matched;
    struct bucket_names buckets;
};

struct put_context {
    struct archive *archive;
    uint64_t remaining;
};

static _Noreturn void archive_fatal(struct archive *archive,
                                    const char *message) {
    errno = 0;
    die_fatal(message, archive_error_string(archive), NULL);
}

static bool bucket_ready(const struct bucket_names *buckets, const char *name) {
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

static void ensure_bucket(struct extract_context *context, const char *bucket) {
    if (bucket_ready(&context->buckets, bucket)) { return; }
    s3_bucket_ensure(&context->config->s3, bucket);
    remember_bucket(&context->buckets, bucket);
}

static bool selected(struct extract_context *context, const char *bucket,
                     const char *key) {
    if (context->config->operand_count == 0) { return true; }
    bool selected = false;
    for (int i = 0; i < context->config->operand_count; ++i) {
        const struct s3ar_selection *selection = &context->selections[i];
        if (!s3ar_selection_matches(selection, bucket, key)) { continue; }
        selected = true;
        if (key != NULL || selection->key == NULL) {
            context->matched[i] = true;
        }
    }
    return selected;
}

static bool bucket_path(char *path) {
    size_t length = strlen(path);
    if (length == 0 || path[0] == '/') { return false; }
    if (path[length - 1] == '/') { path[--length] = '\0'; }
    return length > 0 && strchr(path, '/') == NULL;
}

static void split_object_path(char *path, const char **bucket,
                              const char **key) {
    if (!s3ar_key_is_safe(path)) {
        errno = 0;
        die_fatal("s3ar: unsafe archive member", path, NULL);
    }
    char *slash = strchr(path, '/');
    if (slash == NULL || slash[1] == '\0') {
        errno = 0;
        die_fatal("s3ar: invalid object archive member", path, NULL);
    }
    *slash = '\0';
    *bucket = path;
    *key = slash + 1;
}

static void append_metadata(struct s3_metadata **metadata, size_t *count,
                            const char *name, const void *value,
                            size_t value_size) {
    if (name[0] == '\0' || strpbrk(name, "\r\n") != NULL ||
        (value_size > 0 && value == NULL) ||
        (value_size > 0 && memchr(value, '\r', value_size) != NULL) ||
        (value_size > 0 && memchr(value, '\n', value_size) != NULL) ||
        (value_size > 0 && memchr(value, '\0', value_size) != NULL) ||
        value_size == SIZE_MAX || *count == SIZE_MAX / sizeof(**metadata)) {
        errno = 0;
        die_fatal("s3ar: invalid object metadata", name, NULL);
    }
    struct s3_metadata *items =
        realloc(*metadata, (*count + 1) * sizeof(**metadata));
    if (items == NULL) { die_fatal("s3ar: out of memory", NULL, NULL); }
    *metadata = items;
    char *name_copy = strdup(name);
    char *value_copy = malloc(value_size + 1);
    if (name_copy == NULL || value_copy == NULL) {
        free(name_copy);
        free(value_copy);
        die_fatal("s3ar: out of memory", NULL, NULL);
    }
    if (value_size > 0) { memcpy(value_copy, value, value_size); }
    value_copy[value_size] = '\0';
    items[*count] = (struct s3_metadata) {
        .name = name_copy,
        .value = value_copy,
    };
    ++*count;
}

static bool namespaced_metadata(struct archive_entry *entry) {
    static const char format_name[] = "user.s3ar.format";
    static const char raw_format_name[] = "SCHILY.xattr.user.s3ar.format";
    archive_entry_xattr_reset(entry);
    const char *name;
    const void *value;
    size_t value_size;
    while (archive_entry_xattr_next(entry, &name, &value, &value_size) ==
           ARCHIVE_OK) {
        if (name == NULL ||
            (strcmp(name, format_name) != 0 &&
             strcmp(name, raw_format_name) != 0)) {
            continue;
        }
        if (value == NULL || value_size != 1 ||
            memcmp(value, "1", 1) != 0) {
            errno = 0;
            die_fatal("s3ar: unsupported archive metadata format", NULL,
                      NULL);
        }
        return true;
    }
    return false;
}

static struct s3_metadata *read_metadata(struct archive_entry *entry,
                                         size_t *count) {
    static const char metadata_prefix[] = "user.s3ar.metadata.";
    static const char raw_metadata_prefix[] =
        "SCHILY.xattr.user.s3ar.metadata.";
    static const char legacy_prefix[] = "user.";
    static const char raw_legacy_prefix[] = "SCHILY.xattr.user.";
    bool namespaced = namespaced_metadata(entry);
    struct s3_metadata *metadata = NULL;
    archive_entry_xattr_reset(entry);
    const char *xattr_name;
    const void *value;
    size_t value_size;
    while (archive_entry_xattr_next(entry, &xattr_name, &value, &value_size) ==
           ARCHIVE_OK) {
        const char *name = NULL;
        if (xattr_name == NULL) { continue; }
        if (namespaced &&
            strncmp(xattr_name, metadata_prefix,
                    sizeof(metadata_prefix) - 1) == 0) {
            name = xattr_name + sizeof(metadata_prefix) - 1;
        }
        else if (namespaced &&
                 strncmp(xattr_name, raw_metadata_prefix,
                         sizeof(raw_metadata_prefix) - 1) == 0) {
            name = xattr_name + sizeof(raw_metadata_prefix) - 1;
        }
        else if (!namespaced &&
                 strncmp(xattr_name, legacy_prefix,
                         sizeof(legacy_prefix) - 1) == 0) {
            name = xattr_name + sizeof(legacy_prefix) - 1;
        }
        else if (!namespaced &&
                 strncmp(xattr_name, raw_legacy_prefix,
                         sizeof(raw_legacy_prefix) - 1) == 0) {
            name = xattr_name + sizeof(raw_legacy_prefix) - 1;
        }
        if (name != NULL) {
            append_metadata(&metadata, count, name, value, value_size);
        }
    }
    return metadata;
}

static void free_metadata(struct s3_metadata *metadata, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        free((char *) metadata[i].name);
        free((char *) metadata[i].value);
    }
    free(metadata);
}

static int read_object_data(int size, char *data, void *callback_data) {
    struct put_context *put = callback_data;
    if (size <= 0 || put->remaining == 0) { return 0; }
    size_t wanted = (uint64_t) size < put->remaining ? (size_t) size
                                                     : (size_t) put->remaining;
    la_ssize_t amount = archive_read_data(put->archive, data, wanted);
    if (amount < 0) {
        archive_fatal(put->archive, "s3ar: cannot read object data");
    }
    if (amount == 0) {
        errno = 0;
        die_fatal("s3ar: truncated object in archive", NULL, NULL);
    }
    put->remaining -= (uint64_t) amount;
    return (int) amount;
}

static void extract_bucket(struct extract_context *context,
                           struct archive_entry *entry, char *path) {
    if (!bucket_path(path)) {
        errno = 0;
        die_fatal("s3ar: invalid bucket archive member", path, NULL);
    }
    (void) namespaced_metadata(entry);
    if (!selected(context, path, NULL)) { return; }
    ensure_bucket(context, path);
    if (context->config->verbose) { log_s3_name(stderr, path, NULL); }
}

static void extract_object(struct extract_context *context,
                           struct archive_entry *entry, char *path) {
    const char *bucket;
    const char *key;
    split_object_path(path, &bucket, &key);
    if (!selected(context, bucket, key)) {
        if (archive_read_data_skip(context->archive) != ARCHIVE_OK) {
            archive_fatal(context->archive, "s3ar: cannot skip archive member");
        }
        return;
    }

    la_int64_t archive_size = archive_entry_size(entry);
    if (archive_size < 0) {
        errno = 0;
        die_fatal("s3ar: object has an invalid size", bucket, key);
    }
    size_t metadata_count = 0;
    struct s3_metadata *metadata = read_metadata(entry, &metadata_count);
    ensure_bucket(context, bucket);
    struct put_context put = {
        .archive = context->archive,
        .remaining = (uint64_t) archive_size,
    };
    s3_object_put(&context->config->s3, bucket, key, (uint64_t) archive_size,
                  metadata, metadata_count, read_object_data, &put);
    free_metadata(metadata, metadata_count);
    if (put.remaining != 0) {
        errno = 0;
        die_fatal("s3ar: incomplete object in archive", bucket, key);
    }
    if (context->config->verbose) { log_s3_name(stderr, bucket, key); }
}

static void extract_entry(struct extract_context *context,
                          struct archive_entry *entry) {
    const char *pathname = archive_entry_pathname_utf8(entry);
    if (pathname == NULL) { pathname = archive_entry_pathname(entry); }
    if (pathname == NULL || pathname[0] == '\0') {
        errno = 0;
        die_fatal("s3ar: archive member has no name", NULL, NULL);
    }
    char *path = strdup(pathname);
    if (path == NULL) { die_fatal("s3ar: out of memory", NULL, NULL); }

    if (archive_entry_hardlink(entry) != NULL ||
        archive_entry_symlink(entry) != NULL) {
        errno = 0;
        die_fatal("s3ar: archive links are not supported", path, NULL);
    }
    mode_t type = archive_entry_filetype(entry);
    if (type == AE_IFDIR) { extract_bucket(context, entry, path); }
    else if (type == AE_IFREG) { extract_object(context, entry, path); }
    else {
        errno = 0;
        die_fatal("s3ar: unsupported archive member type", path, NULL);
    }
    free(path);
}

void s3ar_extract(const struct s3ar_config *config) {
    struct archive *archive = archive_read_new();
    if (archive == NULL) { die_fatal("s3ar: out of memory", NULL, NULL); }
    if (archive_read_support_filter_none(archive) != ARCHIVE_OK ||
        archive_read_support_filter_zstd(archive) != ARCHIVE_OK ||
        archive_read_support_format_tar(archive) != ARCHIVE_OK) {
        archive_fatal(archive, "s3ar: cannot initialize archive reader");
    }
    int result;
    if (config->archive_path == NULL ||
        strcmp(config->archive_path, "-") == 0) {
        result = archive_read_open_fd(archive, STDIN_FILENO, 10240);
    }
    else {
        result =
            archive_read_open_filename(archive, config->archive_path, 10240);
    }
    if (result != ARCHIVE_OK) {
        archive_fatal(archive, "s3ar: cannot open archive");
    }
    if (config->zstd &&
        archive_filter_code(archive, 0) != ARCHIVE_FILTER_ZSTD) {
        archive_fatal(archive, "s3ar: cannot open tar archive");
    }

    struct extract_context context = {
        .config = config,
        .archive = archive,
    };
    if (config->operand_count > 0) {
        context.selections =
            calloc((size_t) config->operand_count, sizeof(*context.selections));
        context.matched =
            calloc((size_t) config->operand_count, sizeof(*context.matched));
        if (context.selections == NULL || context.matched == NULL) {
            die_fatal("s3ar: out of memory", NULL, NULL);
        }
        for (int i = 0; i < config->operand_count; ++i) {
            s3ar_selection_parse(&context.selections[i], config->operands[i]);
        }
    }

    struct archive_entry *entry;
    while ((result = archive_read_next_header(archive, &entry)) == ARCHIVE_OK) {
        extract_entry(&context, entry);
    }
    if (result != ARCHIVE_EOF) {
        archive_fatal(archive, "s3ar: cannot read archive header");
    }
    for (int i = 0; i < config->operand_count; ++i) {
        if (!context.matched[i]) {
            errno = 0;
            die_fatal("s3ar: not found in archive", config->operands[i], NULL);
        }
        s3ar_selection_free(&context.selections[i]);
    }
    free(context.selections);
    free(context.matched);
    free_buckets(&context.buckets);

    if (archive_read_close(archive) != ARCHIVE_OK) {
        archive_fatal(archive, "s3ar: cannot close archive");
    }
    if (archive_read_free(archive) != ARCHIVE_OK) {
        errno = 0;
        die_fatal("s3ar: cannot free archive reader", NULL, NULL);
    }
}
