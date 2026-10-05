/* SPDX-License-Identifier: MIT-0 */
#ifndef S3AR_HASH_H____
#define S3AR_HASH_H____

#include <nettle/sha2.h>
#include <stdbool.h>
#include <stddef.h>

#define S3AR_HASH_PREFIX "sha512:"
#define S3AR_HASH_PREFIX_LENGTH (sizeof(S3AR_HASH_PREFIX) - 1)
#define S3AR_HASH_TEXT_LENGTH (S3AR_HASH_PREFIX_LENGTH + 2 * SHA512_DIGEST_SIZE)
#define S3AR_HASH_TEXT_SIZE (S3AR_HASH_TEXT_LENGTH + 1)

void s3ar_hash_text(struct sha512_ctx *hash, char output[S3AR_HASH_TEXT_SIZE]);
bool s3ar_hash_valid(const void *value, size_t size);

#endif
