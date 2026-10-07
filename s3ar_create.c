/*
 * Create a POSIX PAX archive from selected buckets and objects in an
 * S3-compatible store. With --hash, small objects are buffered for SHA-512;
 * larger objects and failed buffer allocations use direct streaming.
 *
 * S3 metadata is stored in PAX SCHILY extended attributes:
 * SCHILY.xattr.user.s3ar.format identifies the namespaced layout,
 * SCHILY.xattr.user.s3ar.bucket and .key preserve URL-encoded S3 names,
 * SCHILY.xattr.user.s3ar.bucket-acl records a bucket ACL summary,
 * SCHILY.xattr.user.s3ar.etag records an informational object ETag,
 * SCHILY.xattr.user.s3ar.hash stores sha512:HEX or none for every object,
 * and SCHILY.xattr.user.s3ar.metadata.NAME preserves S3 user metadata for
 * later extraction. The UTF-8 archive pathname is informational; the PAX
 * identity attributes are authoritative.
 *
 * SPDX-License-Identifier: MIT-0
 */

#include "fsyncfile.h"
#include "log.h"
#include "s3ar_log.h"
#include "s3.h"
#include "s3ar.h"
#include "s3ar_format.h"
#include "s3ar_hash.h"
#include "s3ar_interrupt.h"
#include "sig.h"

#include <archive.h>
#include <archive_entry.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct create_context {
    const struct s3ar_config *config;
    struct archive *archive;
};

struct get_context {
    struct create_context *create;
    const char *bucket;
    const char *key;
    uint64_t expected;
    uint64_t received;
    unsigned char *buffer;
    struct archive_entry *entry;
    struct sha512_ctx hash;
    char hash_text[S3AR_HASH_TEXT_SIZE];
    bool buffered;
    bool properties_received;
    int64_t last_modified;
    char etag[256];
};

static struct get_context *active_get;
static volatile sig_atomic_t interrupted_signal;
static char *temporary_archive_path;
static bool temporary_archive_created;

static void handle_interrupt(int signal_number) {
    interrupted_signal = signal_number;
}

static void install_interrupt_handlers(void) {
    sig_catch(SIGINT, handle_interrupt);
    sig_catch(SIGTERM, handle_interrupt);
}

static void check_interrupted(void) {
    if (interrupted_signal == 0) return;
    log_f1("interrupted");
    s3ar_die(2);
}

static void cleanup_object(struct get_context *get) {
    free(get->buffer);
    archive_entry_free(get->entry);
    get->buffer = NULL;
    get->entry = NULL;
    active_get = NULL;
}

void s3ar_create_cleanup(void) {
    if (active_get != NULL) cleanup_object(active_get);
    if (temporary_archive_path != NULL) {
        if (temporary_archive_created) (void) unlink(temporary_archive_path);
        free(temporary_archive_path);
        temporary_archive_path = NULL;
        temporary_archive_created = false;
    }
}

static _Noreturn void archive_fatal(struct archive *archive,
                                    const char *message) {
    const char *detail = archive_error_string(archive);
    if (detail != NULL)
        log_f3(message, ": ", detail);
    else
        log_f1(message);
    s3ar_die(2);
}

static char *pathname_key(const char *key) {
    size_t key_length = strlen(key);
    bool trailing_slash = key_length > 0 && key[key_length - 1] == '/';
    if (trailing_slash && key_length > SIZE_MAX - 3) { return NULL; }
    size_t size = key_length + (trailing_slash ? 3 : 1);
    char *path = malloc(size);
    if (path == NULL) { return NULL; }

    /* TODO: Extend this filter with manifest-driven escaping for filesystem
     * exceptions such as empty, "." and ".." components, object/prefix
     * collisions, and filesystem-specific name restrictions. */
    if (trailing_slash) {
        memcpy(path, key, key_length - 1);
        memcpy(path + key_length - 1, "%2F", 4);
    }
    else { memcpy(path, key, key_length + 1); }
    return path;
}

static char *object_path(const char *bucket, const char *key) {
    size_t bucket_length = strlen(bucket);
    char *path_key = pathname_key(key);
    if (path_key == NULL) {
        log_f1("out of memory");
        s3ar_die(2);
    }
    size_t key_length = strlen(path_key);
    if (bucket_length > SIZE_MAX - key_length - 2) {
        free(path_key);
        log_f1("out of memory");
        s3ar_die(2);
    }
    char *path = malloc(bucket_length + key_length + 2);
    if (path == NULL) {
        free(path_key);
        log_f1("out of memory");
        s3ar_die(2);
    }
    memcpy(path, bucket, bucket_length);
    path[bucket_length] = '/';
    memcpy(path + bucket_length + 1, path_key, key_length + 1);
    free(path_key);
    return path;
}

static void add_encoded_name(struct archive_entry *entry, const char *xattr,
                             const char *name) {
    char *encoded = s3_uri_encode_alloc(name, true);
    if (encoded == NULL) {
        archive_entry_free(entry);
        log_f1("out of memory");
        s3ar_die(2);
    }
    archive_entry_xattr_add_entry(entry, xattr, encoded, strlen(encoded));
    free(encoded);
}

static bool write_bucket_acl(void *callback_data,
                             const struct s3_bucket *bucket) {
    check_interrupted();
    struct create_context *context = callback_data;
    size_t length = strlen(bucket->name);
    if (length == SIZE_MAX) {
        log_f1("out of memory");
        s3ar_die(2);
    }
    char *path = malloc(length + 2);
    struct archive_entry *entry = archive_entry_new();
    if (path == NULL || entry == NULL) {
        free(path);
        archive_entry_free(entry);
        log_f1("out of memory");
        s3ar_die(2);
    }
    memcpy(path, bucket->name, length);
    path[length] = '/';
    path[length + 1] = '\0';
    /* Preserve the known UTF-8 bytes without a locale-dependent conversion. */
    archive_entry_set_pathname(entry, path);
    archive_entry_set_filetype(entry, AE_IFDIR);
    archive_entry_set_perm(entry, 0700);
    archive_entry_set_uid(entry, 0);
    archive_entry_set_gid(entry, 0);
    archive_entry_set_size(entry, 0);
    archive_entry_set_mtime(entry, 0, 0);
    const char *acl = bucket->acl != NULL ? bucket->acl : "unavailable";
    archive_entry_xattr_add_entry(entry, S3AR_XATTR_FORMAT,
                                  S3AR_XATTR_FORMAT_VERSION,
                                  sizeof(S3AR_XATTR_FORMAT_VERSION) - 1);
    add_encoded_name(entry, S3AR_XATTR_BUCKET, bucket->name);
    archive_entry_xattr_add_entry(entry, S3AR_XATTR_BUCKET_ACL, acl,
                                  strlen(acl));
    if (archive_write_header(context->archive, entry) != ARCHIVE_OK) {
        archive_entry_free(entry);
        free(path);
        archive_fatal(context->archive, "cannot write bucket header");
    }
    if (archive_write_finish_entry(context->archive) != ARCHIVE_OK) {
        archive_entry_free(entry);
        free(path);
        archive_fatal(context->archive, "cannot finish bucket entry");
    }
    archive_entry_free(entry);
    free(path);
    log_i1(s3_log_uri(NULL, bucket->name, NULL));
    return true;
}

static void write_bucket(struct create_context *context, const char *bucket) {
    check_interrupted();
    struct s3_error error = {0};
    enum s3_result result = s3_bucket_acl(context->config->s3, &error, bucket,
                                          write_bucket_acl, context);
    if (result != S3_RESULT_OK) {
        log_f4(result == S3_RESULT_NOT_FOUND ? "bucket does not exist "
                                             : "unable to read bucket ACL ",
               s3_log_uri(NULL, bucket, NULL), ": ", s3ar_log_error(&error));
        s3ar_die(2);
    }
    check_interrupted();
}

static void add_object_hash(struct archive_entry *entry, const char *hash) {
    int count = archive_entry_xattr_count(entry);
    archive_entry_xattr_add_entry(entry, S3AR_XATTR_HASH, hash, strlen(hash));
    if (archive_entry_xattr_count(entry) != count + 1) {
        log_f1("cannot allocate object hash attribute");
        s3ar_die(2);
    }
}

static bool write_object_header(void *callback_data,
                                const struct s3_object_properties *object) {
    check_interrupted();
    struct get_context *get = callback_data;
    if (object->size > INT64_MAX) {
        log_f3("oversized S3 object", " ",
               s3_log_uri(NULL, get->bucket, get->key));
        s3ar_die(2);
    }
    char *path = object_path(get->bucket, get->key);
    struct archive_entry *entry = archive_entry_new();
    if (entry == NULL) {
        free(path);
        log_f1("out of memory");
        s3ar_die(2);
    }
    archive_entry_set_pathname(entry, path);
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0600);
    archive_entry_set_uid(entry, 0);
    archive_entry_set_gid(entry, 0);
    archive_entry_set_size(entry, (la_int64_t) object->size);
    time_t mtime = object->last_modified >= 0 ? (time_t) object->last_modified
                                              : (time_t) 0;
    archive_entry_set_mtime(entry, mtime, 0);
    archive_entry_xattr_add_entry(entry, S3AR_XATTR_FORMAT,
                                  S3AR_XATTR_FORMAT_VERSION,
                                  sizeof(S3AR_XATTR_FORMAT_VERSION) - 1);
    add_encoded_name(entry, S3AR_XATTR_BUCKET, get->bucket);
    add_encoded_name(entry, S3AR_XATTR_KEY, get->key);
    if (object->etag[0] != '\0') {
        archive_entry_xattr_add_entry(entry, S3AR_XATTR_ETAG, object->etag,
                                      strlen(object->etag));
    }
    for (size_t i = 0; i < object->metadata_count; ++i) {
        const char *name = object->metadata[i].name;
        const char *value = object->metadata[i].value;
        if (name == NULL) { continue; }
        if (value == NULL) { value = ""; }
        size_t name_length = strlen(name);
        if (name_length > SIZE_MAX - sizeof(S3AR_XATTR_METADATA_PREFIX)) {
            archive_entry_free(entry);
            free(path);
            log_f1("out of memory");
            s3ar_die(2);
        }
        char *xattr = malloc(sizeof(S3AR_XATTR_METADATA_PREFIX) + name_length);
        if (xattr == NULL) {
            archive_entry_free(entry);
            free(path);
            log_f1("out of memory");
            s3ar_die(2);
        }
        memcpy(xattr, S3AR_XATTR_METADATA_PREFIX,
               sizeof(S3AR_XATTR_METADATA_PREFIX) - 1);
        memcpy(xattr + sizeof(S3AR_XATTR_METADATA_PREFIX) - 1, name,
               name_length + 1);
        archive_entry_xattr_add_entry(entry, xattr, value, strlen(value));
        free(xattr);
    }
    free(path);
    get->entry = entry;
    get->expected = object->size;
    get->last_modified = (int64_t) mtime;
    memcpy(get->etag, object->etag, sizeof(get->etag));
    get->properties_received = true;
    if (get->create->config->hash &&
        object->size <= get->create->config->multipart_size &&
        object->size <= SIZE_MAX) {
        if (object->size != 0) get->buffer = malloc((size_t) object->size);
        get->buffered = object->size == 0 || get->buffer != NULL;
    }
    if (get->buffered) { sha512_init(&get->hash); }
    else {
        if (get->create->config->hash) {
            const char *reason =
                object->size > get->create->config->multipart_size
                    ? "object exceeds the --multipart-size buffer limit"
                : object->size > SIZE_MAX
                    ? "object exceeds addressable buffer size"
                    : "buffer allocation failed";
            log_w4("cannot compute SHA-512 for ",
                   s3_log_uri(NULL, get->bucket, get->key), ": ", reason);
        }
        add_object_hash(entry, "none");
        if (archive_write_header(get->create->archive, entry) != ARCHIVE_OK)
            archive_fatal(get->create->archive, "cannot write object header");
    }
    return true;
}

static bool write_object_data(void *callback_data, const unsigned char *data,
                              size_t size) {
    struct get_context *get = callback_data;
    if (interrupted_signal != 0) {
        errno = EINTR;
        return false;
    }
    if (!get->properties_received || get->received > get->expected ||
        (uint64_t) size > get->expected - get->received) {
        errno = EIO;
        return false;
    }
    if (get->buffered) {
        if (size != 0) {
            memcpy(get->buffer + (size_t) get->received, data, size);
            sha512_update(&get->hash, size, data);
        }
    }
    else {
        la_ssize_t written =
            archive_write_data(get->create->archive, data, size);
        if (written < 0 || (size_t) written != size)
            archive_fatal(get->create->archive, "cannot write object data");
    }
    if (interrupted_signal != 0) {
        errno = EINTR;
        return false;
    }
    get->received += (uint64_t) size;
    return true;
}

static bool write_object(struct create_context *context, const char *bucket,
                         const char *key, bool optional) {
    check_interrupted();
    struct get_context get = {
        .create = context,
        .bucket = bucket,
        .key = key,
    };
    active_get = &get;
    struct s3_error error = {0};
    enum s3_result result =
        s3_object_get(context->config->s3, &error, write_object_header,
                      write_object_data, &get, bucket, key);
    check_interrupted();
    if (optional && result == S3_RESULT_NOT_FOUND && !get.properties_received) {
        cleanup_object(&get);
        return false;
    }
    if (result != S3_RESULT_OK) {
        log_f4("unable to read object ", s3_log_uri(NULL, bucket, key), ": ",
               s3ar_log_error(&error));
        s3ar_die(2);
    }
    if (!get.properties_received || get.received != get.expected) {
        log_f3("incomplete S3 object", " ", s3_log_uri(NULL, bucket, key));
        s3ar_die(2);
    }
    if (get.buffered) {
        s3ar_hash_text(&get.hash, get.hash_text);
        add_object_hash(get.entry, get.hash_text);
        if (archive_write_header(context->archive, get.entry) != ARCHIVE_OK)
            archive_fatal(context->archive, "cannot write object header");
        for (size_t offset = 0; offset < (size_t) get.expected;) {
            check_interrupted();
            size_t size = (size_t) get.expected - offset;
            if (size > 1024 * 1024) size = 1024 * 1024;
            la_ssize_t written =
                archive_write_data(context->archive, get.buffer + offset, size);
            if (written < 0 || (size_t) written != size)
                archive_fatal(context->archive, "cannot write object data");
            offset += size;
        }
        check_interrupted();
    }
    if (archive_write_finish_entry(context->archive) != ARCHIVE_OK) {
        archive_fatal(context->archive, "cannot finish object entry");
    }
    cleanup_object(&get);
    char hash_field[137];
    (void) snprintf(hash_field, sizeof(hash_field), " %s",
                    get.buffered ? get.hash_text : "none");
    log_i8(s3_log_uri(NULL, bucket, key), " ",
           log_num((long long) get.expected), " ",
           log_num((long long) get.last_modified), " ",
           get.etag[0] != '\0' ? get.etag : "-", hash_field);
    return true;
}

static bool write_listed_object(void *callback_data,
                                const struct s3_object *object) {
    check_interrupted();
    struct create_context *context = callback_data;
    if (!write_object(context, object->bucket, object->key, true)) {
        log_w2("object disappeared during backup ",
               s3_log_uri(NULL, object->bucket, object->key));
        if (log_enabled(log_WARNING) && fflush(stderr) != 0) {
            log_f5("unable to write warning", " ",
                   s3_log_uri(NULL, object->bucket, object->key), ": ",
                   log_errno());
            s3ar_die(2);
        }
    }
    return true;
}

static void write_selected_bucket(void *data, const struct s3_bucket *bucket) {
    check_interrupted();
    write_bucket(data, bucket->name);
}

static int open_archive_output(const char *path) {
    struct stat existing;
    mode_t mode;
    if (stat(path, &existing) == 0) {
        if (!S_ISREG(existing.st_mode))
            return open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        mode = existing.st_mode & 07777;
    }
    else if (errno == ENOENT) {
        mode_t mask = umask(0);
        (void) umask(mask);
        mode = 0666 & ~mask;
    }
    else {
        log_f4("cannot inspect archive ", path, ": ", log_errno());
        s3ar_die(2);
    }

    if (strlen(path) > SIZE_MAX - 12) {
        errno = ENAMETOOLONG;
        log_f4("archive path is too long ", path, ": ", log_errno());
        s3ar_die(2);
    }
    size_t temporary_size = strlen(path) + 12;
    temporary_archive_path = malloc(temporary_size);
    if (temporary_archive_path == NULL) {
        log_f1("out of memory");
        s3ar_die(2);
    }
    (void) snprintf(temporary_archive_path, temporary_size, "%s.tmp.XXXXXX",
                    path);
    int fd = mkstemp(temporary_archive_path);
    if (fd < 0) {
        log_f4("cannot create temporary archive ", path, ": ", log_errno());
        s3ar_die(2);
    }
    temporary_archive_created = true;
    if (fchmod(fd, mode) != 0) {
        log_f4("cannot set archive permissions ", path, ": ", log_errno());
        s3ar_die(2);
    }
    return fd;
}

void s3ar_create(const struct s3ar_config *config) {
    struct s3ar_selection_set selections;
    if (s3ar_selection_set_parse(&selections, (size_t) config->operand_count,
                                 config->operands) != 0)
        s3ar_die(2);
    interrupted_signal = 0;
    s3ar_interrupt_bind(config->s3, &interrupted_signal);
    install_interrupt_handlers();
    int fd = STDOUT_FILENO;
    bool close_fd =
        config->archive_path != NULL && strcmp(config->archive_path, "-") != 0;
    if (close_fd) {
        fd = open_archive_output(config->archive_path);
        if (fd < 0) {
            log_f4("cannot create archive ", config->archive_path, ": ",
                   log_errno());
            s3ar_die(2);
        }
    }

    struct archive *archive = archive_write_new();
    if (archive == NULL) {
        log_f1("out of memory");
        s3ar_die(2);
    }
    /* libarchive otherwise pads the final output block.  For compressed
     * streams written to a pipe, that padding follows the compression frame
     * and is rejected as an invalid additional frame by decompressors. */
    if (archive_write_set_bytes_in_last_block(archive, 1) != ARCHIVE_OK ||
        archive_write_set_format_pax_restricted(archive) != ARCHIVE_OK ||
        archive_write_set_options(
            archive, "xattrheader=SCHILY,hdrcharset=BINARY") != ARCHIVE_OK ||
        (config->zstd &&
         archive_write_add_filter_zstd(archive) != ARCHIVE_OK) ||
        archive_write_open_fd(archive, fd) != ARCHIVE_OK) {
        archive_fatal(archive, "cannot open archive");
    }

    struct create_context context = {
        .config = config,
        .archive = archive,
    };
    const struct s3ar_selection_callbacks callbacks = {
        .bucket = write_selected_bucket,
        .object = write_listed_object,
    };
    for (size_t i = 0; i < selections.count; ++i) {
        check_interrupted();
        if (s3ar_selection_walk(config->s3, &selections.items[i], &callbacks,
                                &context) != S3_RESULT_OK)
            s3ar_die(2);
    }
    s3ar_selection_set_free(&selections);
    check_interrupted();

    if (archive_write_close(archive) != ARCHIVE_OK) {
        archive_fatal(archive, "cannot finish archive");
    }
    if (archive_write_free(archive) != ARCHIVE_OK) {
        log_f1("cannot free archive writer");
        s3ar_die(2);
    }
    check_interrupted();
    if (close_fd && fsyncfile(fd) != 0) {
        log_f4("cannot sync archive ", config->archive_path, ": ", log_errno());
        s3ar_die(2);
    }
    if (close_fd && close(fd) != 0) {
        log_f4("cannot close archive ", config->archive_path, ": ",
               log_errno());
        s3ar_die(2);
    }
    check_interrupted();
    if (temporary_archive_path != NULL) {
        if (rename(temporary_archive_path, config->archive_path) != 0) {
            log_f4("cannot install archive ", config->archive_path, ": ",
                   log_errno());
            s3ar_die(2);
        }
        char *installed_path = temporary_archive_path;
        temporary_archive_path = NULL;
        temporary_archive_created = false;
        free(installed_path);
    }
}
