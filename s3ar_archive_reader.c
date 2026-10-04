/*
 * Read bucket directories and object members from a POSIX PAX archive for
 * listing or restoration to an S3-compatible store. Plain and zstd-compressed
 * archives are supported; selected object bodies are streamed directly into S3
 * PUT requests.
 *
 * s3ar archives store S3 metadata in PAX SCHILY extended attributes:
 * SCHILY.xattr.user.s3ar.format identifies the namespaced layout,
 * SCHILY.xattr.user.s3ar.bucket-acl and SCHILY.xattr.user.s3ar.etag record
 * informational listing properties, while
 * SCHILY.xattr.user.s3ar.metadata.NAME stores user metadata. User metadata is
 * restored to uploaded objects; bucket ACL summaries are informational and
 * are not restored.
 * Legacy SCHILY.xattr.user.NAME metadata remains readable.
 * Unsafe paths, links, and unsupported archive member types are rejected.
 *
 * SPDX-License-Identifier: MIT-0
 */

#include "log.h"
#include "s3ar_log.h"
#include "s3.h"
#include "s3ar_archive_reader.h"
#include "s3ar_interrupt.h"
#include "s3ar_transform.h"
#include "sig.h"

#include <archive.h>
#include <archive_entry.h>
#include <nettle/sha2.h>

#include <errno.h>
#include <signal.h>
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
    struct s3ar_selection_set selections;
    struct bucket_names archive_buckets;
    struct bucket_names ready_buckets;
    bool list_only;
};

struct put_context {
    struct archive *archive;
    uint64_t remaining;
    bool verify_hash;
    bool hash_verified;
    struct sha512_ctx hash;
    const char *expected_hash;
    enum {
        PUT_READ_OK = 0,
        PUT_READ_ARCHIVE_ERROR,
        PUT_READ_TRUNCATED,
        PUT_READ_INTERRUPTED,
        PUT_READ_HASH_MISMATCH,
    } read_status;
    char archive_error[256];
};

static volatile sig_atomic_t interrupted_signal;

static void handle_interrupt(int signal_number) {
    interrupted_signal = signal_number;
}

static void install_interrupt_handlers(void) {
    sig_catch(SIGINT, handle_interrupt);
    sig_catch(SIGTERM, handle_interrupt);
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

static size_t bucket_slot(char *const *names, size_t capacity,
                          const char *name) {
    /* cdb64 hash: start at 5381, then multiply by 33 and XOR each byte. */
    uint64_t hash = UINT64_C(5381);
    for (const unsigned char *p = (const unsigned char *) name; *p != 0; ++p) {
        hash = (hash * UINT64_C(33)) ^ *p;
    }
    size_t slot = (size_t) hash & (capacity - 1);
    while (names[slot] != NULL && strcmp(names[slot], name) != 0)
        slot = (slot + 1) & (capacity - 1);
    return slot;
}

static bool bucket_known(const struct bucket_names *buckets, const char *name) {
    return buckets->capacity != 0 &&
           buckets->names[bucket_slot(buckets->names, buckets->capacity,
                                      name)] != NULL;
}

static void remember_bucket(struct bucket_names *buckets, const char *name) {
    if (bucket_known(buckets, name)) return;
    /* Keep at least half the slots empty so probing always terminates. */
    if (buckets->count >= buckets->capacity / 2) {
        size_t capacity = buckets->capacity == 0 ? 16 : buckets->capacity * 2;
        if (capacity < buckets->capacity ||
            capacity > SIZE_MAX / sizeof(*buckets->names)) {
            log_f1("out of memory");
            s3ar_die(2);
        }
        char **names = calloc(capacity, sizeof(*names));
        if (names == NULL) {
            log_f1("out of memory");
            s3ar_die(2);
        }
        for (size_t i = 0; i < buckets->capacity; ++i) {
            char *old = buckets->names[i];
            if (old != NULL) names[bucket_slot(names, capacity, old)] = old;
        }
        free(buckets->names);
        buckets->names = names;
        buckets->capacity = capacity;
    }
    char *copy = strdup(name);
    if (copy == NULL) {
        log_f1("out of memory");
        s3ar_die(2);
    }
    buckets->names[bucket_slot(buckets->names, buckets->capacity, name)] = copy;
    ++buckets->count;
}

static void free_buckets(struct bucket_names *buckets) {
    for (size_t i = 0; i < buckets->capacity; ++i) free(buckets->names[i]);
    free(buckets->names);
}

static void ensure_bucket(struct extract_context *context, const char *bucket) {
    if (bucket_known(&context->ready_buckets, bucket)) { return; }
    struct s3_error error = {0};
    enum s3_result result =
        s3_bucket_ensure(context->config->s3, &error, bucket);
    if (result != S3_RESULT_OK) {
        log_f4("unable to initialize bucket ", s3_log_uri(NULL, bucket, NULL),
               ": ", s3ar_log_error(&error));
        s3ar_die(2);
    }
    remember_bucket(&context->ready_buckets, bucket);
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
        log_f2("invalid object metadata ", name);
        s3ar_die(2);
    }
    struct s3_metadata *items =
        realloc(*metadata, (*count + 1) * sizeof(**metadata));
    if (items == NULL) {
        log_f1("out of memory");
        s3ar_die(2);
    }
    *metadata = items;
    char *name_copy = strdup(name);
    char *value_copy = malloc(value_size + 1);
    if (name_copy == NULL || value_copy == NULL) {
        free(name_copy);
        free(value_copy);
        log_f1("out of memory");
        s3ar_die(2);
    }
    if (value_size > 0) { memcpy(value_copy, value, value_size); }
    value_copy[value_size] = '\0';
    items[*count] = (struct s3_metadata) {
        .name = name_copy,
        .value = value_copy,
    };
    ++*count;
}

static bool metadata_format(struct archive_entry *entry) {
    static const char format_name[] = "user.s3ar.format";
    static const char raw_format_name[] = "SCHILY.xattr.user.s3ar.format";
    archive_entry_xattr_reset(entry);
    const char *name;
    const void *value;
    size_t value_size;
    while (archive_entry_xattr_next(entry, &name, &value, &value_size) ==
           ARCHIVE_OK) {
        if (name == NULL || (strcmp(name, format_name) != 0 &&
                             strcmp(name, raw_format_name) != 0)) {
            continue;
        }
        if (value == NULL ||
            value_size != sizeof(S3AR_XATTR_FORMAT_VERSION) - 1 ||
            memcmp(value, S3AR_XATTR_FORMAT_VERSION,
                   sizeof(S3AR_XATTR_FORMAT_VERSION) - 1) != 0) {
            log_f1("unsupported archive metadata format");
            s3ar_die(2);
        }
        return true;
    }
    return false;
}

/* Validate the format-1 object contract; payload verification is separate. */
static void read_object_hash(struct archive_entry *entry, char *output) {
    bool found = false;
    const char *name;
    const void *value;
    size_t size;
    archive_entry_xattr_reset(entry);
    while (archive_entry_xattr_next(entry, &name, &value, &size) ==
           ARCHIVE_OK) {
        if (name == NULL || (strcmp(name, "user.s3ar.hash") != 0 &&
                             strcmp(name, "SCHILY.xattr.user.s3ar.hash") != 0))
            continue;
        bool valid =
            value != NULL && size == 4 && memcmp(value, "none", 4) == 0;
        if (value != NULL && size == 135 && memcmp(value, "sha512:", 7) == 0) {
            const unsigned char *text = value;
            valid = true;
            for (size_t i = 7; i < size; ++i) {
                if (!((text[i] >= '0' && text[i] <= '9') ||
                      (text[i] >= 'a' && text[i] <= 'f')))
                    valid = false;
            }
        }
        if (found || !valid) {
            log_f1("invalid or duplicate object hash in archive");
            s3ar_die(2);
        }
        if (output != NULL) {
            memcpy(output, value, size);
            output[size] = '\0';
        }
        found = true;
    }
    if (!found) {
        log_f1("missing object hash in format-1 archive");
        s3ar_die(2);
    }
}

static bool identity_safe(unsigned char value) {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
           (value >= '0' && value <= '9') || value == '-' || value == '.' ||
           value == '_' || value == '~' || value == '/';
}

static int hex_value(unsigned char value) {
    if (value >= '0' && value <= '9') { return value - '0'; }
    if (value >= 'A' && value <= 'F') { return value - 'A' + 10; }
    if (value >= 'a' && value <= 'f') { return value - 'a' + 10; }
    return -1;
}

static char *decode_identity(const char *header, const void *value,
                             size_t value_size) {
    if (value == NULL || value_size == 0) {
        log_f2("invalid URL-encoded PAX header ", header);
        s3ar_die(2);
    }
    char *decoded = malloc(value_size + 1);
    if (decoded == NULL) {
        log_f1("out of memory");
        s3ar_die(2);
    }
    const unsigned char *input = value;
    size_t output_size = 0;
    for (size_t i = 0; i < value_size; ++i) {
        unsigned char byte = input[i];
        if (byte == '%') {
            if (i + 2 >= value_size) {
                free(decoded);
                log_f2("invalid URL-encoded PAX header ", header);
                s3ar_die(2);
            }
            int high = hex_value(input[++i]);
            int low = hex_value(input[++i]);
            if (high < 0 || low < 0) {
                free(decoded);
                log_f2("invalid URL-encoded PAX header ", header);
                s3ar_die(2);
            }
            byte = (unsigned char) ((high << 4) | low);
        }
        else if (!identity_safe(byte)) {
            free(decoded);
            log_f2("invalid URL-encoded PAX header ", header);
            s3ar_die(2);
        }
        if (byte == '\0') {
            free(decoded);
            log_f2("invalid URL-encoded PAX header ", header);
            s3ar_die(2);
        }
        decoded[output_size++] = (char) byte;
    }
    decoded[output_size] = '\0';
    return decoded;
}

static char *read_identity_header(struct archive_entry *entry,
                                  const char *name) {
    char *decoded = NULL;
    archive_entry_xattr_reset(entry);
    const char *xattr_name;
    const void *value;
    size_t value_size;
    while (archive_entry_xattr_next(entry, &xattr_name, &value, &value_size) ==
           ARCHIVE_OK) {
        if (xattr_name == NULL || strcmp(xattr_name, name) != 0) { continue; }
        if (decoded != NULL) {
            free(decoded);
            log_f2("duplicate S3 identity PAX header ", name);
            s3ar_die(2);
        }
        decoded = decode_identity(name, value, value_size);
    }
    return decoded;
}

static void read_identity(struct archive_entry *entry, bool object,
                          char **bucket, char **key) {
    static const char bucket_name[] = "user.s3ar.bucket";
    static const char key_name[] = "user.s3ar.key";
    *bucket = NULL;
    *key = NULL;
    if (metadata_format(entry) && object) read_object_hash(entry, NULL);
    *bucket = read_identity_header(entry, bucket_name);
    *key = read_identity_header(entry, key_name);
    if (*bucket == NULL || (object ? *key == NULL : *key != NULL) ||
        strchr(*bucket, '/') != NULL) {
        free(*bucket);
        free(*key);
        *bucket = NULL;
        *key = NULL;
        log_f1("incomplete or invalid S3 identity PAX headers");
        s3ar_die(2);
    }
}

static char *read_etag(struct archive_entry *entry) {
    static const char etag_name[] = "user.s3ar.etag";
    static const char raw_etag_name[] = "SCHILY.xattr.user.s3ar.etag";
    char *etag = NULL;
    bool found = false;
    archive_entry_xattr_reset(entry);
    const char *name;
    const void *value;
    size_t value_size;
    while (archive_entry_xattr_next(entry, &name, &value, &value_size) ==
           ARCHIVE_OK) {
        if (name == NULL || (strcmp(name, etag_name) != 0 &&
                             strcmp(name, raw_etag_name) != 0)) {
            continue;
        }
        if (found || (value_size > 0 && value == NULL) ||
            (value_size > 0 && memchr(value, '\0', value_size) != NULL) ||
            (value_size > 0 && memchr(value, '\r', value_size) != NULL) ||
            (value_size > 0 && memchr(value, '\n', value_size) != NULL) ||
            value_size == SIZE_MAX) {
            free(etag);
            log_f1("invalid object ETag in archive");
            s3ar_die(2);
        }
        found = true;
        if (value_size == 0) { continue; }
        etag = malloc(value_size + 1);
        if (etag == NULL) {
            log_f1("out of memory");
            s3ar_die(2);
        }
        memcpy(etag, value, value_size);
        etag[value_size] = '\0';
    }
    return etag;
}

static struct s3_metadata *read_metadata(struct archive_entry *entry,
                                         size_t *count) {
    static const char metadata_prefix[] = "user.s3ar.metadata.";
    static const char raw_metadata_prefix[] =
        "SCHILY.xattr.user.s3ar.metadata.";
    static const char legacy_prefix[] = "user.";
    static const char raw_legacy_prefix[] = "SCHILY.xattr.user.";
    bool namespaced = metadata_format(entry);
    struct s3_metadata *metadata = NULL;
    archive_entry_xattr_reset(entry);
    const char *xattr_name;
    const void *value;
    size_t value_size;
    while (archive_entry_xattr_next(entry, &xattr_name, &value, &value_size) ==
           ARCHIVE_OK) {
        const char *name = NULL;
        if (xattr_name == NULL) { continue; }
        if (strcmp(xattr_name, "user.s3ar.bucket") == 0 ||
            strcmp(xattr_name, "user.s3ar.key") == 0) {
            continue;
        }
        if (namespaced && strncmp(xattr_name, metadata_prefix,
                                  sizeof(metadata_prefix) - 1) == 0) {
            name = xattr_name + sizeof(metadata_prefix) - 1;
        }
        else if (namespaced && strncmp(xattr_name, raw_metadata_prefix,
                                       sizeof(raw_metadata_prefix) - 1) == 0) {
            name = xattr_name + sizeof(raw_metadata_prefix) - 1;
        }
        else if (!namespaced && strncmp(xattr_name, legacy_prefix,
                                        sizeof(legacy_prefix) - 1) == 0) {
            name = xattr_name + sizeof(legacy_prefix) - 1;
        }
        else if (!namespaced && strncmp(xattr_name, raw_legacy_prefix,
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

static enum s3_read_result read_object_data(void *callback_data,
                                            unsigned char *data,
                                            size_t capacity, size_t *size) {
    struct put_context *put = callback_data;
    *size = 0;
    if (interrupted_signal != 0) {
        put->read_status = PUT_READ_INTERRUPTED;
        errno = EINTR;
        return S3_READ_ERROR;
    }
    if (put->remaining == 0) {
        /* The upload layer checks EOF before PUT or multipart completion.
         * Retries replay its part buffer, without re-reading archive data. */
        if (put->verify_hash && !put->hash_verified) {
            unsigned char digest[SHA512_DIGEST_SIZE];
            static const char hex[] = "0123456789abcdef";
            sha512_digest(&put->hash, sizeof(digest), digest);
            for (size_t i = 0; i < sizeof(digest); ++i) {
                if (put->expected_hash[2 * i] != hex[digest[i] >> 4] ||
                    put->expected_hash[2 * i + 1] != hex[digest[i] & 15]) {
                    put->read_status = PUT_READ_HASH_MISMATCH;
                    errno = EIO;
                    return S3_READ_ERROR;
                }
            }
            put->hash_verified = true;
        }
        return S3_READ_EOF;
    }
    size_t wanted = (uint64_t) capacity < put->remaining
                        ? capacity
                        : (size_t) put->remaining;
    la_ssize_t amount = archive_read_data(put->archive, data, wanted);
    if (interrupted_signal != 0) {
        put->read_status = PUT_READ_INTERRUPTED;
        errno = EINTR;
        return S3_READ_ERROR;
    }
    if (amount < 0) {
        const char *message = archive_error_string(put->archive);
        put->read_status = PUT_READ_ARCHIVE_ERROR;
        (void) snprintf(put->archive_error, sizeof(put->archive_error), "%s",
                        message != NULL ? message : "archive read failed");
        errno = archive_errno(put->archive);
        if (errno == 0) errno = EIO;
        return S3_READ_ERROR;
    }
    if (amount == 0) {
        put->read_status = PUT_READ_TRUNCATED;
        errno = EIO;
        return S3_READ_ERROR;
    }
    if (put->verify_hash) sha512_update(&put->hash, (size_t) amount, data);
    put->remaining -= (uint64_t) amount;
    *size = (size_t) amount;
    return S3_READ_DATA;
}

static _Noreturn void report_put_read_error(const struct put_context *put) {
    if (put->read_status == PUT_READ_ARCHIVE_ERROR) {
        log_f2("cannot read object data ", put->archive_error);
        s3ar_die(2);
    }
    if (put->read_status == PUT_READ_TRUNCATED) {
        log_f1("truncated object in archive");
        s3ar_die(2);
    }
    log_f1("interrupted");
    s3ar_die(2);
}

static void transform_identity(struct extract_context *context, char **bucket,
                               char **key) {
    if (context->config->transforms == NULL) return;
    bool object = *key != NULL;
    size_t bucket_size = strlen(*bucket);
    size_t key_size = object ? strlen(*key) : 0;
    if (bucket_size > SIZE_MAX - key_size - 2) {
        log_f1("archive identity is too long");
        s3ar_die(2);
    }
    char *name = malloc(bucket_size + key_size + 2);
    if (name == NULL) {
        log_f1("out of memory");
        s3ar_die(2);
    }
    memcpy(name, *bucket, bucket_size);
    name[bucket_size] = '/';
    if (object) memcpy(name + bucket_size + 1, *key, key_size);
    name[bucket_size + key_size + 1] = '\0';
    char error_text[256];
    char *result = s3ar_transform_apply(context->config->transforms, name,
                                        error_text, sizeof(error_text));
    free(name);
    if (result == NULL) {
        log_f2("unable to transform archive member: ", error_text);
        s3ar_die(2);
    }
    char *slash = strchr(result, '/');
    if (slash != NULL) *slash = '\0';
    const char *result_key = slash != NULL ? slash + 1 : NULL;
    struct s3_error error = {0};
    const char *validation_key = object ? result_key : "s3ar-bucket-member";
    enum s3_result status =
        context->list_only
            ? s3_url_validate_object_name_for_style(
                  S3_URI_STYLE_PATH, false, result, validation_key, &error)
            : s3_url_validate_object_name(context->config->s3, result,
                                          validation_key, &error);
    if (status != S3_RESULT_OK) {
        log_f4("invalid transformed archive member ",
               s3_log_uri(NULL, result, result_key), ": ",
               s3ar_log_error(&error));
        free(result);
        s3ar_die(2);
    }
    char *new_key = object ? strdup(result_key) : NULL;
    if (object && new_key == NULL) {
        free(result);
        log_f1("out of memory");
        s3ar_die(2);
    }
    log_d4("transform: ", s3_log_uri(NULL, *bucket, *key), " to ",
           s3_log_uri(NULL, result, new_key));
    free(*bucket);
    free(*key);
    *bucket = result;
    *key = new_key;
}

static void extract_bucket(struct extract_context *context,
                           struct archive_entry *entry) {
    char *header_bucket;
    char *header_key;
    read_identity(entry, false, &header_bucket, &header_key);
    const char *bucket = header_bucket;
    remember_bucket(&context->archive_buckets, bucket);
    transform_identity(context, &header_bucket, &header_key);
    bucket = header_bucket;
    if (!s3ar_selection_set_match(&context->selections, bucket, NULL)) {
        free(header_bucket);
        free(header_key);
        return;
    }
    if (context->list_only) {
        log_o1(s3_log_uri(NULL, bucket, NULL));
        free(header_bucket);
        free(header_key);
        return;
    }
    ensure_bucket(context, bucket);
    log_i1(s3_log_uri(NULL, bucket, NULL));
    free(header_bucket);
    free(header_key);
}

static void extract_object(struct extract_context *context,
                           struct archive_entry *entry) {
    char *header_bucket;
    char *header_key;
    read_identity(entry, true, &header_bucket, &header_key);
    const char *bucket = header_bucket;
    const char *key = header_key;
    if (!bucket_known(&context->archive_buckets, bucket)) {
        log_f3("object precedes bucket archive member", " ",
               s3_log_uri(NULL, bucket, key));
        s3ar_die(2);
    }
    transform_identity(context, &header_bucket, &header_key);
    bucket = header_bucket;
    key = header_key;
    if (!s3ar_selection_set_match(&context->selections, bucket, key)) {
        if (archive_read_data_skip(context->archive) != ARCHIVE_OK) {
            archive_fatal(context->archive, "cannot skip archive member");
        }
        free(header_bucket);
        free(header_key);
        return;
    }
    char hash_field[137] = " none";
    if (metadata_format(entry)) read_object_hash(entry, hash_field + 1);
    if (context->list_only) {
        la_int64_t archive_size = archive_entry_size(entry);
        if (archive_size < 0) {
            log_f3("object has an invalid size", " ",
                   s3_log_uri(NULL, bucket, key));
            s3ar_die(2);
        }
        char *etag = context->config->verbose ? read_etag(entry) : NULL;
        if (context->config->verbose) {
            log_o8(s3_log_uri(NULL, bucket, key), " ",
                   log_num((long long) archive_size), " ",
                   log_num((long long) archive_entry_mtime(entry)), " ",
                   etag != NULL ? etag : "-", hash_field);
        }
        else
            log_o1(s3_log_uri(NULL, bucket, key));
        free(etag);
        if (archive_read_data_skip(context->archive) != ARCHIVE_OK)
            archive_fatal(context->archive, "cannot skip archive member");
        free(header_bucket);
        free(header_key);
        return;
    }

    la_int64_t archive_size = archive_entry_size(entry);
    if (archive_size < 0) {
        log_f3("object has an invalid size", " ",
               s3_log_uri(NULL, bucket, key));
        s3ar_die(2);
    }
    size_t metadata_count = 0;
    struct s3_metadata *metadata = read_metadata(entry, &metadata_count);
    if (context->config->transforms != NULL)
        ensure_bucket(context, bucket);
    else if (!bucket_known(&context->ready_buckets, bucket)) {
        log_f3("selected bucket was not initialized", " ",
               s3_log_uri(NULL, bucket, NULL));
        s3ar_die(2);
    }
    struct put_context put = {
        .archive = context->archive,
        .remaining = (uint64_t) archive_size,
    };
    if (context->config->hash) {
        if (strcmp(hash_field + 1, "none") == 0) {
            log_w2("hash unavailable; content not verified for ",
                   s3_log_uri(NULL, bucket, key));
        }
        else {
            put.verify_hash = true;
            put.expected_hash = hash_field + 1 + 7;
            sha512_init(&put.hash);
        }
    }
    struct s3_object_properties properties = {
        .metadata = metadata,
        .metadata_count = metadata_count,
    };
    struct s3_error error = {0};
    enum s3_result result = s3_object_put(context->config->s3, &error, bucket,
                                          key, (uint64_t) archive_size,
                                          &properties, read_object_data, &put);
    free_metadata(metadata, metadata_count);
    if (interrupted_signal != 0 && put.read_status == PUT_READ_OK)
        put.read_status = PUT_READ_INTERRUPTED;
    if (put.read_status != PUT_READ_OK &&
        strncmp(error.message, "multipart abort failed:",
                sizeof("multipart abort failed:") - 1) == 0) {
        log_f4("unable to clean up multipart upload ",
               s3_log_uri(NULL, bucket, key), ": ", s3ar_log_error(&error));
        s3ar_die(2);
    }
    if (put.read_status == PUT_READ_HASH_MISMATCH) {
        log_f2("SHA-512 mismatch for ", s3_log_uri(NULL, bucket, key));
        s3ar_die(2);
    }
    if (put.read_status != PUT_READ_OK) report_put_read_error(&put);
    if (result != S3_RESULT_OK) {
        log_f4("unable to write object ", s3_log_uri(NULL, bucket, key), ": ",
               s3ar_log_error(&error));
        s3ar_die(2);
    }
    if (put.remaining != 0) {
        log_f3("incomplete object in archive", " ",
               s3_log_uri(NULL, bucket, key));
        s3ar_die(2);
    }
    log_i3(s3_log_uri(NULL, bucket, key), hash_field,
           context->config->hash
               ? (put.hash_verified ? " verified" : " unverified")
               : "");
    free(header_bucket);
    free(header_key);
}

static void extract_entry(struct extract_context *context,
                          struct archive_entry *entry) {
    const char *pathname = archive_entry_pathname(entry);
    if (pathname == NULL || pathname[0] == '\0') {
        log_f1("archive member has no name");
        s3ar_die(2);
    }
    if (archive_entry_hardlink(entry) != NULL ||
        archive_entry_symlink(entry) != NULL) {
        log_f2("archive links are not supported ", pathname);
        s3ar_die(2);
    }
    mode_t type = archive_entry_filetype(entry);
    if (type == AE_IFDIR) { extract_bucket(context, entry); }
    else if (type == AE_IFREG) { extract_object(context, entry); }
    else {
        log_f2("unsupported archive member type ", pathname);
        s3ar_die(2);
    }
}

void s3ar_archive_reader_read(const struct s3ar_config *config,
                              enum s3ar_archive_reader_mode mode) {
    struct s3ar_selection_set selections;
    if (s3ar_selection_set_parse(&selections, (size_t) config->operand_count,
                                 config->operands) != 0)
        s3ar_die(2);
    interrupted_signal = 0;
    s3ar_interrupt_bind(config->s3, &interrupted_signal);
    install_interrupt_handlers();
    struct archive *archive = archive_read_new();
    if (archive == NULL) {
        log_f1("out of memory");
        s3ar_die(2);
    }
    if (archive_read_support_filter_none(archive) != ARCHIVE_OK ||
        archive_read_support_filter_zstd(archive) != ARCHIVE_OK ||
        archive_read_support_format_tar(archive) != ARCHIVE_OK) {
        archive_fatal(archive, "cannot initialize archive reader");
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
    if (result != ARCHIVE_OK) { archive_fatal(archive, "cannot open archive"); }
    if (config->zstd &&
        archive_filter_code(archive, 0) != ARCHIVE_FILTER_ZSTD) {
        log_f1("archive is not zstd-compressed");
        s3ar_die(2);
    }

    struct extract_context context = {
        .config = config,
        .archive = archive,
        .list_only = mode == S3AR_ARCHIVE_READER_LIST,
    };
    context.selections = selections;

    struct archive_entry *entry;
    while ((result = archive_read_next_header(archive, &entry)) == ARCHIVE_OK) {
        if (interrupted_signal != 0) {
            log_f1("interrupted");
            s3ar_die(2);
        }
        extract_entry(&context, entry);
    }
    if (interrupted_signal != 0) {
        log_f1("interrupted");
        s3ar_die(2);
    }
    if (result != ARCHIVE_EOF) {
        archive_fatal(archive, "cannot read archive header");
    }
    for (size_t i = 0; i < context.selections.count; ++i) {
        if (!context.selections.items[i].matched) {
            log_f2("not found in archive ", context.selections.items[i].uri);
            s3ar_die(2);
        }
    }
    s3ar_selection_set_free(&context.selections);
    free_buckets(&context.archive_buckets);
    free_buckets(&context.ready_buckets);

    if (archive_read_close(archive) != ARCHIVE_OK) {
        archive_fatal(archive, "cannot close archive");
    }
    if (archive_read_free(archive) != ARCHIVE_OK) {
        log_f1("cannot free archive reader");
        s3ar_die(2);
    }
}
