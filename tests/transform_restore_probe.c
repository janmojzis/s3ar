/* SPDX-License-Identifier: MIT-0 */
#include "main.h"
#include "s3.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum s3_result __wrap_s3_bucket_ensure(struct s3_client *client,
                                       struct s3_error *error,
                                       const char *bucket) {
    assert(s3_url_validate_object_name(client, bucket, "probe", error) ==
           S3_RESULT_OK);
    (void) printf("BUCKET %s\n", bucket);
    return S3_RESULT_OK;
}

enum s3_result __wrap_s3_object_put_with_part_size(
    struct s3_client *client, struct s3_error *error, const char *bucket,
    const char *key, uint64_t size, size_t part_size,
    const struct s3_object_properties *properties, s3_read_callback read,
    void *data) {
    assert(part_size >= 5 * 1024 * 1024);
    assert(s3_url_validate_object_name(client, bucket, key, error) ==
           S3_RESULT_OK);
    (void) printf("PUT %s/%s size=%llu\n", bucket, key,
                  (unsigned long long) size);
    for (size_t i = 0; i < properties->metadata_count; ++i)
        (void) printf("META %s=%s\n", properties->metadata[i].name,
                      properties->metadata[i].value);
    unsigned char buffer[64];
    while (size != 0) {
        size_t amount = 0;
        assert(read(data, buffer,
                    size < sizeof(buffer) ? (size_t) size : sizeof(buffer),
                    &amount) == S3_READ_DATA);
        assert(amount > 0 && amount <= size);
        assert(fwrite(buffer, 1, amount, stdout) == amount);
        size -= amount;
    }
    (void) fputc('\n', stdout);
    const char *abort_failure = getenv("S3AR_TEST_ABORT_FAILURE");
    if (abort_failure != NULL) {
        size_t amount = 0;
        assert(read(data, buffer, sizeof(buffer), &amount) == S3_READ_ERROR);
        error->result = S3_RESULT_CALLBACK_ERROR;
        error->abort_result = strcmp(abort_failure, "yes") == 0
                                  ? S3_RESULT_ACCESS_DENIED
                                  : S3_RESULT_OK;
        (void) snprintf(error->message, sizeof(error->message), "%s",
                        error->abort_result != S3_RESULT_OK
                            ? "cleanup refused"
                            : "multipart abort failed: text only");
        return error->result;
    }
    return S3_RESULT_OK;
}

int main(int argc, char **argv) { return main_s3ar(argc, argv); }
