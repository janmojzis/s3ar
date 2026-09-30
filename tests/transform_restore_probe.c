/* SPDX-License-Identifier: MIT-0 */
#include "main.h"
#include "s3.h"

#include <assert.h>
#include <stdio.h>

enum s3_result __wrap_s3_bucket_ensure(struct s3_client *client,
                                       struct s3_error *error,
                                       const char *bucket) {
    assert(s3_url_validate_object_name(client, bucket, "probe", error) ==
           S3_RESULT_OK);
    (void) printf("BUCKET %s\n", bucket);
    return S3_RESULT_OK;
}

enum s3_result
__wrap_s3_object_put(struct s3_client *client, struct s3_error *error,
                     const char *bucket, const char *key, uint64_t size,
                     const struct s3_object_properties *properties,
                     s3_read_callback read, void *data) {
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
    return S3_RESULT_OK;
}

int main(int argc, char **argv) { return main_s3ar(argc, argv); }
