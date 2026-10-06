/* SPDX-License-Identifier: MIT-0 */
#include "s3ar_hash.h"

#include <string.h>

uint64_t s3ar_hash_string(const char *text) {
    uint64_t hash = UINT64_C(5381);
    for (const unsigned char *p = (const unsigned char *) text; *p != 0; ++p)
        hash = (hash * UINT64_C(33)) ^ *p;
    return hash;
}

void s3ar_hash_text(struct sha512_ctx *hash, char output[S3AR_HASH_TEXT_SIZE]) {
    unsigned char digest[SHA512_DIGEST_SIZE];
    static const char hex[] = "0123456789abcdef";
    sha512_digest(hash, sizeof(digest), digest);
    memcpy(output, S3AR_HASH_PREFIX, S3AR_HASH_PREFIX_LENGTH);
    for (size_t i = 0; i < sizeof(digest); ++i) {
        output[S3AR_HASH_PREFIX_LENGTH + 2 * i] = hex[digest[i] >> 4];
        output[S3AR_HASH_PREFIX_LENGTH + 2 * i + 1] = hex[digest[i] & 15];
    }
    output[S3AR_HASH_TEXT_LENGTH] = '\0';
}

bool s3ar_hash_valid(const void *value, size_t size) {
    const unsigned char *text = value;
    if (value == NULL || size != S3AR_HASH_TEXT_LENGTH ||
        memcmp(value, S3AR_HASH_PREFIX, S3AR_HASH_PREFIX_LENGTH) != 0)
        return false;
    for (size_t i = S3AR_HASH_PREFIX_LENGTH; i < size; ++i) {
        if (!((text[i] >= '0' && text[i] <= '9') ||
              (text[i] >= 'a' && text[i] <= 'f')))
            return false;
    }
    return true;
}
