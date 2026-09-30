/* SPDX-License-Identifier: MIT-0 */
#include "s3.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
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
