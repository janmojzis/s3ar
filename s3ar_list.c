/* SPDX-License-Identifier: MIT-0 */

#include "die.h"
#include "s3.h"
#include "s3ar.h"

#include <archive.h>
#include <archive_entry.h>

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct archive_member {
    char *name;
    char *details;
    uint64_t size;
    bool bucket;
};

struct archive_members {
    struct archive_member *items;
    size_t count;
    size_t capacity;
};

struct owned_metadata {
    char *name;
    char *value;
};

static _Noreturn void archive_fatal(struct archive *archive,
                                    const char *message) {
    errno = 0;
    die_fatal(message, archive_error_string(archive), NULL);
}

static char *copy_xattr_value(const char *name, const void *value,
                              size_t size) {
    if ((size > 0 && value == NULL) ||
        (size > 0 && memchr(value, '\0', size) != NULL) || size == SIZE_MAX) {
        errno = 0;
        die_fatal("s3ar: invalid archive metadata", name, NULL);
    }
    char *copy = malloc(size + 1);
    if (copy == NULL) { die_fatal("s3ar: out of memory", NULL, NULL); }
    if (size > 0) { memcpy(copy, value, size); }
    copy[size] = '\0';
    return copy;
}

static int compare_owned_metadata(const void *left, const void *right) {
    const struct owned_metadata *a = left;
    const struct owned_metadata *b = right;
    int result = strcmp(a->name, b->name);
    return result != 0 ? result : strcmp(a->value, b->value);
}

static char *entry_metadata(struct archive_entry *entry) {
    struct owned_metadata *metadata = NULL;
    size_t count = 0;
    archive_entry_xattr_reset(entry);
    const char *xattr_name;
    const void *value;
    size_t size;
    while (archive_entry_xattr_next(entry, &xattr_name, &value, &size) ==
           ARCHIVE_OK) {
        if (xattr_name == NULL || strncmp(xattr_name, "user.", 5) != 0 ||
            xattr_name[5] == '\0') {
            continue;
        }
        if (count == SIZE_MAX / sizeof(*metadata)) {
            errno = ENOMEM;
            die_fatal("s3ar: out of memory", NULL, NULL);
        }
        struct owned_metadata *items =
            realloc(metadata, (count + 1) * sizeof(*metadata));
        if (items == NULL) { die_fatal("s3ar: out of memory", NULL, NULL); }
        metadata = items;
        metadata[count].name = strdup(xattr_name + 5);
        metadata[count].value = copy_xattr_value(xattr_name, value, size);
        if (metadata[count].name == NULL) {
            die_fatal("s3ar: out of memory", NULL, NULL);
        }
        ++count;
    }
    qsort(metadata, count, sizeof(*metadata), compare_owned_metadata);

    size_t length = count == 0 ? 1 : 0;
    for (size_t i = 0; i < count; ++i) {
        size_t name_length = strlen(metadata[i].name);
        size_t value_length = strlen(metadata[i].value);
        size_t addition = name_length + value_length + 1 + (i > 0 ? 1 : 0);
        if (addition < name_length || length > SIZE_MAX - addition) {
            errno = ENOMEM;
            die_fatal("s3ar: out of memory", NULL, NULL);
        }
        length += addition;
    }
    char *result = malloc(length + 1);
    if (result == NULL) { die_fatal("s3ar: out of memory", NULL, NULL); }
    size_t offset = 0;
    if (count == 0) { result[offset++] = '-'; }
    for (size_t i = 0; i < count; ++i) {
        if (i > 0) { result[offset++] = ','; }
        size_t name_length = strlen(metadata[i].name);
        size_t value_length = strlen(metadata[i].value);
        memcpy(result + offset, metadata[i].name, name_length);
        offset += name_length;
        result[offset++] = '=';
        memcpy(result + offset, metadata[i].value, value_length);
        offset += value_length;
    }
    result[offset] = '\0';
    for (size_t i = 0; i < count; ++i) {
        free(metadata[i].name);
        free(metadata[i].value);
    }
    free(metadata);
    return result;
}

static char *entry_bucket_acl(struct archive_entry *entry) {
    char *acl = NULL;
    archive_entry_xattr_reset(entry);
    const char *name;
    const void *value;
    size_t size;
    while (archive_entry_xattr_next(entry, &name, &value, &size) ==
           ARCHIVE_OK) {
        if (name != NULL &&
            (strcmp(name, "s3ar.bucket-acl") == 0 ||
             strcmp(name, "SCHILY.xattr.s3ar.bucket-acl") == 0)) {
            free(acl);
            acl = copy_xattr_value(name, value, size);
        }
    }
    if (acl == NULL) {
        acl = strdup("unavailable");
        if (acl == NULL) { die_fatal("s3ar: out of memory", NULL, NULL); }
    }
    return acl;
}

static bool archive_selected(const struct s3ar_selection *selections,
                             int selection_count, bool *matched,
                             const char *bucket, const char *key) {
    if (selection_count == 0) { return true; }
    bool selected = false;
    for (int i = 0; i < selection_count; ++i) {
        if (s3ar_selection_matches(&selections[i], bucket, key)) {
            selected = true;
            matched[i] = true;
        }
    }
    return selected;
}

static void append_archive_member(struct archive_members *members, char *name,
                                  bool bucket, uint64_t size, char *details) {
    if (members->count == members->capacity) {
        size_t capacity = members->capacity == 0 ? 16 : members->capacity * 2;
        if (capacity < members->capacity ||
            capacity > SIZE_MAX / sizeof(*members->items)) {
            errno = ENOMEM;
            die_fatal("s3ar: out of memory", NULL, NULL);
        }
        struct archive_member *items =
            realloc(members->items, capacity * sizeof(*items));
        if (items == NULL) { die_fatal("s3ar: out of memory", NULL, NULL); }
        members->items = items;
        members->capacity = capacity;
    }
    members->items[members->count++] = (struct archive_member) {
        .name = name,
        .details = details,
        .size = size,
        .bucket = bucket,
    };
}

static void collect_archive_entry(struct archive_members *members,
                                  struct archive_entry *entry,
                                  const struct s3ar_selection *selections,
                                  int selection_count, bool *matched) {
    const char *pathname = archive_entry_pathname_utf8(entry);
    if (pathname == NULL) { pathname = archive_entry_pathname(entry); }
    if (pathname == NULL || pathname[0] == '\0') {
        errno = 0;
        die_fatal("s3ar: archive member has no name", NULL, NULL);
    }
    if (archive_entry_hardlink(entry) != NULL ||
        archive_entry_symlink(entry) != NULL) {
        errno = 0;
        die_fatal("s3ar: archive links are not supported", pathname, NULL);
    }

    char *name = strdup(pathname);
    if (name == NULL) { die_fatal("s3ar: out of memory", NULL, NULL); }
    mode_t type = archive_entry_filetype(entry);
    if (type == AE_IFDIR) {
        size_t length = strlen(name);
        if (length > 0 && name[length - 1] == '/') { name[--length] = '\0'; }
        if (length == 0 || strchr(name, '/') != NULL) {
            errno = 0;
            die_fatal("s3ar: invalid bucket archive member", pathname, NULL);
        }
        if (!archive_selected(selections, selection_count, matched, name,
                              NULL)) {
            free(name);
            return;
        }
        append_archive_member(members, name, true, 0, entry_bucket_acl(entry));
        return;
    }
    if (type != AE_IFREG || !s3ar_key_is_safe(name)) {
        errno = 0;
        die_fatal("s3ar: invalid archive member", pathname, NULL);
    }
    char *slash = strchr(name, '/');
    if (slash == NULL || slash[1] == '\0') {
        errno = 0;
        die_fatal("s3ar: archive member lacks BUCKET/KEY", pathname, NULL);
    }
    *slash = '\0';
    bool selected =
        archive_selected(selections, selection_count, matched, name, slash + 1);
    *slash = '/';
    if (!selected) {
        free(name);
        return;
    }
    la_int64_t size = archive_entry_size(entry);
    if (size < 0) {
        errno = 0;
        die_fatal("s3ar: archive member has invalid size", pathname, NULL);
    }
    append_archive_member(members, name, false, (uint64_t) size,
                          entry_metadata(entry));
}

static void free_archive_members(struct archive_members *members) {
    for (size_t i = 0; i < members->count; ++i) {
        free(members->items[i].name);
        free(members->items[i].details);
    }
    free(members->items);
}

static void list_archive(const struct s3ar_config *config) {
    struct s3ar_selection *selections = NULL;
    bool *matched = NULL;
    if (config->operand_count > 0) {
        selections =
            calloc((size_t) config->operand_count, sizeof(*selections));
        matched = calloc((size_t) config->operand_count, sizeof(*matched));
        if (selections == NULL || matched == NULL) {
            die_fatal("s3ar: out of memory", NULL, NULL);
        }
        for (int i = 0; i < config->operand_count; ++i) {
            s3ar_selection_parse(&selections[i], config->operands[i]);
        }
    }

    struct archive *archive = archive_read_new();
    if (archive == NULL) { die_fatal("s3ar: out of memory", NULL, NULL); }
    if (archive_read_support_filter_none(archive) != ARCHIVE_OK ||
        archive_read_support_filter_zstd(archive) != ARCHIVE_OK ||
        archive_read_support_format_tar(archive) != ARCHIVE_OK) {
        archive_fatal(archive, "s3ar: cannot initialize tar reader");
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
        archive_fatal(archive, "s3ar: cannot open tar archive");
    }

    struct archive_members members = {0};
    struct archive_entry *entry;
    result = archive_read_next_header(archive, &entry);
    if (config->zstd &&
        archive_filter_code(archive, 0) != ARCHIVE_FILTER_ZSTD) {
        archive_fatal(archive, "s3ar: cannot open tar archive");
    }
    while (result == ARCHIVE_OK) {
        collect_archive_entry(&members, entry, selections,
                              config->operand_count, matched);
        if (archive_read_data_skip(archive) != ARCHIVE_OK) {
            archive_fatal(archive, "s3ar: cannot skip tar member");
        }
        result = archive_read_next_header(archive, &entry);
    }
    if (result != ARCHIVE_EOF) {
        archive_fatal(archive, "s3ar: cannot read tar archive");
    }
    for (int i = 0; i < config->operand_count; ++i) {
        if (!matched[i]) {
            errno = 0;
            die_fatal("s3ar: not found in archive", config->operands[i], NULL);
        }
        s3ar_selection_free(&selections[i]);
    }
    free(selections);
    free(matched);
    if (archive_read_close(archive) != ARCHIVE_OK) {
        archive_fatal(archive, "s3ar: cannot close tar archive");
    }
    if (archive_read_free(archive) != ARCHIVE_OK) {
        errno = 0;
        die_fatal("s3ar: cannot free tar reader", NULL, NULL);
    }

    for (size_t i = 0; i < members.count; ++i) {
        const struct archive_member *member = &members.items[i];
        int written;
        if (!config->verbose) {
            written = fprintf(stdout, "%s\n", member->name);
        }
        else if (member->bucket) {
            written =
                fprintf(stdout, "%s\tacl=%s\n", member->name, member->details);
        }
        else {
            written = fprintf(stdout, "%s\t%" PRIu64 "\t%s\n", member->name,
                              member->size, member->details);
        }
        if (written < 0) {
            die_fatal("s3ar: unable to write standard output", member->name,
                      NULL);
        }
    }
    free_archive_members(&members);
}

struct list_context {
    const struct s3 *s3;
    bool verbose;
};

static void log_head_object(const struct s3_object *object,
                            void *callback_data) {
    const struct list_context *context = callback_data;
    log_s3_object(object, context->verbose);
}

static void log_object(const struct s3_object *object, void *callback_data) {
    struct list_context *context = callback_data;
    if (context->verbose) {
        s3_object_head(context->s3, object->bucket, object->key,
                       log_head_object, context);
    }
    else { log_s3_object(object, false); }
}

static void log_acl_bucket(const struct s3_bucket *bucket,
                           void *callback_data) {
    const struct list_context *context = callback_data;
    log_s3_bucket(bucket, context->verbose);
}

static void log_bucket(const struct s3_bucket *bucket, void *callback_data) {
    struct list_context *context = callback_data;
    if (context->verbose) {
        s3_bucket_acl(context->s3, bucket->name, log_acl_bucket, context);
    }
    else { log_s3_bucket(bucket, false); }
}

static void list_all_bucket(const struct s3_bucket *bucket,
                            void *callback_data) {
    struct list_context *context = callback_data;
    log_bucket(bucket, context);
    s3_object_list_prefix(context->s3, bucket->name, NULL, log_object, context);
}

static void list_selection(struct list_context *context,
                           const struct s3ar_selection *selection) {
    const struct s3 *s3 = context->s3;
    if (selection->bucket == NULL) {
        s3_bucket_list(s3, list_all_bucket, context);
        return;
    }
    if (selection->key == NULL) {
        s3_bucket_check(s3, selection->bucket);
        const struct s3_bucket bucket = {
            .name = selection->bucket,
            .creation_date = -1,
        };
        log_bucket(&bucket, context);
        s3_object_list_prefix(s3, selection->bucket, NULL, log_object, context);
        return;
    }

    bool found = s3_object_head(s3, selection->bucket, selection->key,
                                log_head_object, context);
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
    size_t descendants = s3_object_list_prefix(s3, selection->bucket, prefix,
                                               log_object, context);
    free(prefix);
    if (!found && descendants == 0) {
        errno = 0;
        die_fatal("s3ar: not found", selection->uri, NULL);
    }
}

void s3ar_list(const struct s3ar_config *config) {
    if (config->archive_path != NULL || config->operand_count == 0) {
        list_archive(config);
        return;
    }
    struct list_context context = {
        .s3 = &config->s3,
        .verbose = config->verbose,
    };
    for (int i = 0; i < config->operand_count; ++i) {
        struct s3ar_selection selection;
        s3ar_selection_parse(&selection, config->operands[i]);
        list_selection(&context, &selection);
        s3ar_selection_free(&selection);
    }
}
