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

void *s3_memory_grow(void *items, size_t *capacity, size_t item_size,
                     size_t initial_capacity) {
    if (*capacity > SIZE_MAX / 2) return NULL;
    size_t next = *capacity != 0 ? *capacity * 2 : initial_capacity;
    if (item_size == 0 || next == 0 || next > SIZE_MAX / item_size) return NULL;
    void *grown = realloc(items, next * item_size);
    if (grown != NULL) *capacity = next;
    return grown;
}
