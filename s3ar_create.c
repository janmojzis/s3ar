/*
 * Create a POSIX PAX archive from selected buckets and objects in an
 * S3-compatible store. Object bodies are streamed directly from S3 GET
 * requests into an uncompressed or zstd-compressed archive.
 *
 * S3 metadata is stored in PAX SCHILY extended attributes:
 * SCHILY.xattr.user.s3ar.format identifies the namespaced layout,
 * SCHILY.xattr.user.s3ar.bucket and .key preserve URL-encoded S3 names,
 * SCHILY.xattr.user.s3ar.bucket-acl records a bucket ACL summary,
 * SCHILY.xattr.user.s3ar.etag records an informational object ETag,
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
    uint64_t written;
    int64_t last_modified;
    char etag[256];
    bool header_written;
};

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

void s3ar_create_cleanup(void) {
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
    archive_entry_xattr_add_entry(entry, "user.s3ar.format",
                                  S3AR_XATTR_FORMAT_VERSION,
                                  sizeof(S3AR_XATTR_FORMAT_VERSION) - 1);
    add_encoded_name(entry, "user.s3ar.bucket", bucket->name);
    archive_entry_xattr_add_entry(entry, "user.s3ar.bucket-acl", acl,
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
    archive_entry_xattr_add_entry(entry, "user.s3ar.format",
                                  S3AR_XATTR_FORMAT_VERSION,
                                  sizeof(S3AR_XATTR_FORMAT_VERSION) - 1);
    add_encoded_name(entry, "user.s3ar.bucket", get->bucket);
    add_encoded_name(entry, "user.s3ar.key", get->key);
    if (object->etag[0] != '\0') {
        archive_entry_xattr_add_entry(entry, "user.s3ar.etag", object->etag,
                                      strlen(object->etag));
    }
    for (size_t i = 0; i < object->metadata_count; ++i) {
        const char *name = object->metadata[i].name;
        const char *value = object->metadata[i].value;
        if (name == NULL) { continue; }
        if (value == NULL) { value = ""; }
        size_t name_length = strlen(name);
        if (name_length > SIZE_MAX - sizeof("user.s3ar.metadata.")) {
            archive_entry_free(entry);
            free(path);
            log_f1("out of memory");
            s3ar_die(2);
        }
        char *xattr = malloc(sizeof("user.s3ar.metadata.") + name_length);
        if (xattr == NULL) {
            archive_entry_free(entry);
            free(path);
            log_f1("out of memory");
            s3ar_die(2);
        }
        memcpy(xattr, "user.s3ar.metadata.", sizeof("user.s3ar.metadata.") - 1);
        memcpy(xattr + sizeof("user.s3ar.metadata.") - 1, name,
               name_length + 1);
        archive_entry_xattr_add_entry(entry, xattr, value, strlen(value));
        free(xattr);
    }
    if (archive_write_header(get->create->archive, entry) != ARCHIVE_OK) {
        archive_entry_free(entry);
        free(path);
        archive_fatal(get->create->archive, "cannot write object header");
    }
    archive_entry_free(entry);
    free(path);
    get->expected = object->size;
    get->last_modified = (int64_t) mtime;
    memcpy(get->etag, object->etag, sizeof(get->etag));
    get->header_written = true;
    return true;
}

static bool write_object_data(void *callback_data, const unsigned char *data,
                              size_t size) {
    struct get_context *get = callback_data;
    if (interrupted_signal != 0) {
        errno = EINTR;
        return false;
    }
    if (!get->header_written ||
        (uint64_t) size > get->expected - get->written) {
        errno = EIO;
        return false;
    }
    la_ssize_t written = archive_write_data(get->create->archive, data, size);
    if (written < 0 || (size_t) written != size) {
        archive_fatal(get->create->archive, "cannot write object data");
    }
    if (interrupted_signal != 0) {
        errno = EINTR;
        return false;
    }
    get->written += (uint64_t) size;
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
    struct s3_error error = {0};
    enum s3_result result =
        s3_object_get(context->config->s3, &error, write_object_header,
                      write_object_data, &get, bucket, key);
    check_interrupted();
    if (optional && result == S3_RESULT_NOT_FOUND && !get.header_written) {
        return false;
    }
    if (result != S3_RESULT_OK) {
        log_f4("unable to read object ", s3_log_uri(NULL, bucket, key), ": ",
               s3ar_log_error(&error));
        s3ar_die(2);
    }
    if (!get.header_written || get.written != get.expected) {
        log_f3("incomplete S3 object", " ", s3_log_uri(NULL, bucket, key));
        s3ar_die(2);
    }
    if (archive_write_finish_entry(context->archive) != ARCHIVE_OK) {
        archive_fatal(context->archive, "cannot finish object entry");
    }
    /* TODO: Replace the trailing hash placeholder with the object's SHA-512. */
    log_i8(s3_log_uri(NULL, bucket, key), " ",
           log_num((long long) get.expected), " ",
           log_num((long long) get.last_modified), " ",
           get.etag[0] != '\0' ? get.etag : "-", " -");
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

static bool write_all_bucket(void *callback_data,
                             const struct s3_bucket *bucket) {
    check_interrupted();
    struct create_context *context = callback_data;
    write_bucket(context, bucket->name);
    struct s3_error error = {0};
    enum s3_result result =
        s3_object_list(context->config->s3, &error, bucket->name, NULL,
                       write_listed_object, context, NULL);
    if (result != S3_RESULT_OK) {
        log_f4("unable to list objects ", s3_log_uri(NULL, bucket->name, NULL),
               ": ", s3ar_log_error(&error));
        s3ar_die(2);
    }
    check_interrupted();
    return true;
}

static void write_selection(struct create_context *context,
                            const struct s3ar_selection *selection) {
    check_interrupted();
    struct s3_client *s3 = context->config->s3;
    struct s3_error error = {0};
    enum s3_result result;
    if (selection->bucket == NULL) {
        result = s3_bucket_list(s3, &error, write_all_bucket, context);
        if (result != S3_RESULT_OK) {
            log_f2("unable to list buckets: ", s3ar_log_error(&error));
            s3ar_die(2);
        }
        return;
    }
    write_bucket(context, selection->bucket);
    if (selection->key == NULL) {
        result = s3_object_list(s3, &error, selection->bucket, NULL,
                                write_listed_object, context, NULL);
        if (result != S3_RESULT_OK) {
            log_f4("unable to list objects ",
                   s3_log_uri(NULL, selection->bucket, NULL), ": ",
                   s3ar_log_error(&error));
            s3ar_die(2);
        }
        return;
    }

    bool found = write_object(context, selection->bucket, selection->key, true);
    size_t length = strlen(selection->key);
    if (length > SIZE_MAX - 2) {
        log_f1("out of memory");
        s3ar_die(2);
    }
    char *prefix = malloc(length + 2);
    if (prefix == NULL) {
        log_f1("out of memory");
        s3ar_die(2);
    }
    memcpy(prefix, selection->key, length);
    prefix[length] = '/';
    prefix[length + 1] = '\0';
    size_t descendants = 0;
    result = s3_object_list(s3, &error, selection->bucket, prefix,
                            write_listed_object, context, &descendants);
    free(prefix);
    if (result != S3_RESULT_OK) {
        log_f4("unable to list objects ",
               s3_log_uri(NULL, selection->bucket, NULL), ": ",
               s3ar_log_error(&error));
        s3ar_die(2);
    }
    if (!found && descendants == 0) {
        log_f2("not found ", selection->uri);
        s3ar_die(2);
    }
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
    for (int i = 0; i < config->operand_count; ++i) {
        check_interrupted();
        struct s3ar_selection selection;
        if (s3ar_selection_parse(&selection, config->operands[i]) != 0) {
            if (errno == EINVAL) {
                log_f2("invalid S3 operand ", config->operands[i]);
                s3ar_die(2);
            }
            log_f1("out of memory");
            s3ar_die(2);
        }
        write_selection(&context, &selection);
        s3ar_selection_free(&selection);
    }
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
