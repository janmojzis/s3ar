/* SPDX-License-Identifier: MIT-0 */
#include "s3_internal.h"

#include <stdlib.h>
#include <string.h>

char *s3_memory_strdup(const char *value) {
    size_t size;
    char *copy;
    if (value == NULL) return NULL;
    size = strlen(value) + 1;
    copy = malloc(size);
    if (copy != NULL) memcpy(copy, value, size);
    return copy;
}

void s3_memory_secure_free(char *value) {
    volatile unsigned char *p;
    size_t size;
    if (value == NULL) return;
    size = strlen(value);
    p = (volatile unsigned char *) value;
    while (size-- != 0) *p++ = 0;
    free(value);
}
