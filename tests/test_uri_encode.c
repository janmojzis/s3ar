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

static void test_query(void) {
    const struct s3_query_param params[] = {
        {"omitted", NULL}, {"prefix", "a /+%&=?#ž"},
        {"empty", ""},     {"marker", "%2F"},
        {NULL, NULL},
    };
    char *query = s3_query_build("uploads&max-uploads=1000", params);
    assert(query != NULL);
    assert(strcmp(query, "uploads&max-uploads=1000&prefix=a%20%2F%2B%25%26%3D"
                         "%3F%23%C5%BE&empty=&marker=%252F") == 0);
    free(query);
    query = s3_query_build("", params);
    assert(query != NULL);
    assert(
        strcmp(query,
               "prefix=a%20%2F%2B%25%26%3D%3F%23%C5%BE&empty=&marker=%252F") ==
        0);
    free(query);
    const struct s3_query_param absent[] = {{"skip", NULL}, {NULL, NULL}};
    query = s3_query_build("", absent);
    assert(query != NULL && strcmp(query, "") == 0);
    free(query);
    query = s3_query_build("max-buckets=10000", absent);
    assert(query != NULL && strcmp(query, "max-buckets=10000") == 0);
    free(query);
    char value[4097];
    memset(value, '/', sizeof(value) - 1);
    value[sizeof(value) - 1] = '\0';
    const struct s3_query_param large[] = {{"uploadId", value}, {NULL, NULL}};
    query = s3_query_build("", large);
    assert(query != NULL && strlen(query) == 9 + 3 * 4096);
    for (size_t i = 9; i < strlen(query); i += 3)
        assert(memcmp(query + i, "%2F", 3) == 0);
    free(query);
}

int main(void) {
    test_decode();
    test_query();
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
