/* SPDX-License-Identifier: MIT-0 */
/* Include the writer to exercise its callbacks without multi-GiB transfers. */
#include "s3ar_create.c"
#include <assert.h>

static size_t rejected_size;
static unsigned rejected_calls;
void *__real_malloc(size_t size);
void *__wrap_malloc(size_t size) {
    if (rejected_size != 0 && size == rejected_size) {
        ++rejected_calls;
        errno = ENOMEM;
        return NULL;
    }
    return __real_malloc(size);
}

static const unsigned char *body;
static size_t body_size;

enum s3_result __wrap_s3_object_get(struct s3_client *client,
                                    struct s3_error *error,
                                    s3_properties_callback properties,
                                    s3_write_callback data, void *context,
                                    const char *bucket, const char *key) {
    (void) client;
    (void) error;
    (void) bucket;
    (void) key;
    struct s3_object_properties object = {.size = body_size};
    assert(properties(context, &object));
    if (body_size != 0) {
        assert(data(context, body, 1));
        assert(data(context, body + 1, body_size - 1));
    }
    return S3_RESULT_OK;
}

static void check_hash(struct archive_entry *entry, const char *expected) {
    const char *name;
    const void *value;
    size_t size;
    unsigned count = 0;
    archive_entry_xattr_reset(entry);
    while (archive_entry_xattr_next(entry, &name, &value, &size) ==
           ARCHIVE_OK) {
        if (strcmp(name, "user.s3ar.hash") != 0) continue;
        assert(size == strlen(expected));
        assert(memcmp(value, expected, size) == 0);
        ++count;
    }
    assert(count == 1);
}

static void roundtrip(const unsigned char *data, size_t size, bool fail,
                      bool hashing, const char *expected) {
    unsigned char output[16384];
    size_t used;
    struct archive *writer = archive_write_new();
    assert(archive_write_set_format_pax_restricted(writer) == ARCHIVE_OK);
    assert(archive_write_set_options(writer, "xattrheader=SCHILY") ==
           ARCHIVE_OK);
    assert(archive_write_open_memory(writer, output, sizeof(output), &used) ==
           ARCHIVE_OK);
    struct s3ar_config config = {.hash = hashing};
    struct create_context context = {.config = &config, .archive = writer};
    body = data;
    body_size = size;
    rejected_size = fail ? size : 0;
    rejected_calls = 0;
    assert(write_object(&context, "bucket", "key", false));
    assert(active_get == NULL);
    assert(rejected_calls == (fail && hashing ? 1u : 0u));
    rejected_size = 0;
    assert(archive_write_close(writer) == ARCHIVE_OK);
    assert(archive_write_free(writer) == ARCHIVE_OK);
    struct archive *reader = archive_read_new();
    archive_read_support_format_tar(reader);
    assert(archive_read_open_memory(reader, output, used) == ARCHIVE_OK);
    struct archive_entry *entry;
    assert(archive_read_next_header(reader, &entry) == ARCHIVE_OK);
    check_hash(entry, expected);
    unsigned char restored[4096];
    assert(archive_read_data(reader, restored, sizeof(restored)) ==
           (la_ssize_t) size);
    if (size) assert(memcmp(restored, data, size) == 0);
    assert(archive_read_next_header(reader, &entry) == ARCHIVE_EOF);
    archive_read_free(reader);
}

static void boundary(uint64_t size, bool allocation) {
    unsigned char output[16384];
    size_t used;
    struct archive *writer = archive_write_new();
    archive_write_set_format_pax_restricted(writer);
    assert(archive_write_open_memory(writer, output, sizeof(output), &used) ==
           ARCHIVE_OK);
    struct s3ar_config config = {.hash = true};
    struct create_context context = {.config = &config, .archive = writer};
    struct get_context get = {
        .create = &context, .bucket = "bucket", .key = "key"};
    struct s3_object_properties object = {.size = size};
    rejected_size = (size_t) size;
    rejected_calls = 0;
    assert(write_object_header(&get, &object));
    assert(!get.buffered);
    assert(rejected_calls == (allocation ? 1u : 0u));
    check_hash(get.entry, "none");
    cleanup_object(&get);
    rejected_size = 0;
    /* Abort without padding the advertised multi-GiB body. */
    archive_write_fail(writer);
    archive_write_free(writer);
}

int main(void) {
    roundtrip(
        (const unsigned char *) "abc", 3, false, true,
        "sha512:"
        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
        "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    roundtrip(
        NULL, 0, false, true,
        "sha512:"
        "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
        "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");
    unsigned char binary[4096];
    for (size_t i = 0; i < sizeof(binary); ++i) binary[i] = (unsigned char) i;
    roundtrip(binary, sizeof(binary), true, true, "none");
    roundtrip(binary, sizeof(binary), true, false, "none");
    boundary(S3_MULTIPART_PART_SIZE - 1, true);
    boundary(S3_MULTIPART_PART_SIZE, true);
    boundary(S3_MULTIPART_PART_SIZE + 1, false);
    boundary(S3_MULTIPART_MAX_PART_SIZE - 1, false);
    boundary(S3_MULTIPART_MAX_PART_SIZE, false);
    boundary(S3_MULTIPART_MAX_PART_SIZE + 1, false);
    return 0;
}
