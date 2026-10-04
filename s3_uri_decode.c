/* SPDX-License-Identifier: MIT-0 */
#include "s3_internal.h"

#include <stdlib.h>
#include <string.h>

static int hex_value(unsigned char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

enum s3_result s3_uri_decode_alloc(const char *encoded, char **decoded) {
    *decoded = NULL;
    size_t input_size = strlen(encoded), output_size = 0;
    char *output = malloc(input_size + 1);
    if (output == NULL) return S3_RESULT_ERROR;
    for (size_t i = 0; i < input_size; ++i) {
        unsigned char byte = (unsigned char) encoded[i];
        if (byte == '%') {
            int high, low;
            if (i + 2 >= input_size) goto invalid;
            high = hex_value((unsigned char) encoded[++i]);
            low = hex_value((unsigned char) encoded[++i]);
            if (high < 0 || low < 0) goto invalid;
            byte = (unsigned char) ((high << 4) | low);
        }
        if (byte == '\0') goto invalid;
        output[output_size++] = (char) byte;
    }
    output[output_size] = '\0';
    *decoded = output;
    return S3_RESULT_OK;
invalid:
    free(output);
    return S3_RESULT_PROTOCOL_ERROR;
}
