/* SPDX-License-Identifier: MIT-0 */
#include "s3_internal.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static void test_decode(void) {
    char input[256], *encoded, *decoded = NULL;
    for (size_t i = 0; i < 255; ++i) input[i] = (char) (i + 1);
    input[255] = '\0';
    encoded = s3_uri_encode_alloc(input, false);
    assert(encoded != NULL);
    assert(s3_uri_decode_alloc(encoded, &decoded) == S3_RESULT_OK);
    assert(memcmp(input, decoded, sizeof(input)) == 0);
    free(encoded);
    free(decoded);

    assert(s3_uri_decode_alloc("a+b%2fc%2F%252F", &decoded) == S3_RESULT_OK);
    assert(strcmp(decoded, "a+b/c/%2F") == 0);
    free(decoded);
    assert(s3_uri_decode_alloc("", &decoded) == S3_RESULT_OK);
    assert(strcmp(decoded, "") == 0);
    free(decoded);

    const char *invalid[] = {"%", "%2", "%GG", "%2G", "%G2", "%00", "a%00b"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        decoded = input;
        assert(s3_uri_decode_alloc(invalid[i], &decoded) ==
               S3_RESULT_PROTOCOL_ERROR);
        assert(decoded == NULL);
    }
}

int main(void) {
    test_decode();
    char input[1026];
    struct {
        char value[S3_URI_ENCODED_MAX_BYTES];
        char guard;
    } result = {.guard = 'X'};
    char *allocated;

    memset(input, ' ', 1024);
    input[1024] = '\0';
    assert(s3_uri_encode_into(result.value, sizeof(result.value), input, 0) ==
           1);
    assert(strlen(result.value) == S3_URI_ENCODED_MAX_BYTES - 1);
    assert(result.value[0] == '%' && result.value[1] == '2' &&
           result.value[2] == '0');
    assert(result.guard == 'X');

    input[1024] = ' ';
    input[1025] = '\0';
    result.value[0] = 'X';
    assert(s3_uri_encode_into(result.value, sizeof(result.value), input, 0) ==
           0);
    assert(result.value[0] == 'X');

    assert(s3_uri_encode_into(result.value, sizeof(result.value), "a/\x80\xff",
                              1) == 1);
    assert(strcmp(result.value, "a/%80%FF") == 0);
    allocated = s3_uri_encode_alloc("a/\x80\xff", true);
    assert(allocated != NULL && strcmp(result.value, allocated) == 0);
    free(allocated);
    assert(s3_uri_encode_into(result.value, sizeof(result.value), "a/\x80\xff",
                              0) == 1);
    assert(strcmp(result.value, "a%2F%80%FF") == 0);
    result.value[0] = 'X';
    assert(!s3_uri_encode_into(result.value, 7, "a/\x80\xff", false));
    assert(result.value[0] == 'X');
    assert(s3_uri_encode_into(result.value, 11, "a/\x80\xff", false));
    assert(
        !s3_uri_encode_into(result.value, sizeof(result.value), NULL, false));
    assert(s3_uri_encode_alloc(NULL, false) == NULL);
    return 0;
}
