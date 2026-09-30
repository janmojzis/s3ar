/* SPDX-License-Identifier: MIT-0 */
#include "s3_internal.h"

#include <stdlib.h>
#include <string.h>

int s3_uri_encode_isliteral(unsigned char c, int keep_slash) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
           c == '~' || (keep_slash && c == '/');
}

static void encode_into(char *output, const char *input, bool keep_slash) {
    static const char hex[] = "0123456789ABCDEF";
    size_t out_size = 0;
    for (size_t i = 0; input[i] != '\0'; ++i) {
        unsigned char c = (unsigned char) input[i];
        if (s3_uri_encode_isliteral(c, keep_slash))
            output[out_size++] = (char) c;
        else {
            output[out_size++] = '%';
            output[out_size++] = hex[c >> 4];
            output[out_size++] = hex[c & 15];
        }
    }
    output[out_size] = '\0';
}

char *s3_uri_encode_alloc(const char *input, bool keep_slash) {
    size_t length;
    char *output;
    if (input == NULL) return NULL;
    length = strlen(input);
    if (length > (SIZE_MAX - 1) / 3) return NULL;
    output = malloc(length * 3 + 1);
    if (output == NULL) return NULL;
    encode_into(output, input, keep_slash);
    return output;
}

bool s3_uri_encode_into(char *output, size_t capacity, const char *input,
                        bool keep_slash) {
    size_t remaining = capacity;
    if (output == NULL || input == NULL || remaining == 0) return false;
    for (size_t i = 0; input[i] != '\0'; ++i) {
        unsigned char c = (unsigned char) input[i];
        size_t needed = s3_uri_encode_isliteral(c, keep_slash) ? 1 : 3;
        if (needed >= remaining) return false;
        remaining -= needed;
    }
    encode_into(output, input, keep_slash);
    return true;
}
