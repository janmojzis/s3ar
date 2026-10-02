/* SPDX-License-Identifier: MIT-0 */
#include "s3_internal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *__real_malloc(size_t size);
void *__real_realloc(void *pointer, size_t size);

static bool move_next_realloc;
static bool fail_next_realloc;
static unsigned fail_malloc_after;

void *__wrap_malloc(size_t size) {
    if (fail_malloc_after != 0 && --fail_malloc_after == 0) return NULL;
    return __real_malloc(size);
}

void *__wrap_realloc(void *pointer, size_t size) {
    if (fail_next_realloc) {
        fail_next_realloc = false;
        return NULL;
    }
    if (move_next_realloc && pointer != NULL) {
        void *moved = __real_malloc(size);
        if (moved == NULL) return NULL;
        /* The ninth metadata field grows an eight-element allocation. */
        memcpy(moved, pointer, 8 * sizeof(struct s3_metadata));
        free(pointer);
        move_next_realloc = false;
        return moved;
    }
    return __real_realloc(pointer, size);
}

static int test_growth_failure(unsigned allocation) {
    struct s3_response response = {0};
    char header[64];
    int failed = 0;
    s3_response_reset(&response);
    for (unsigned i = 0; i < 8; ++i) {
        int size = snprintf(header, sizeof(header),
                            "x-amz-meta-field%u: value\r\n", i);
        if (size <= 0 ||
            s3_headers_callback(header, 1, (size_t) size, &response) !=
                (size_t) size ||
            response.invalid_headers) {
            fprintf(stderr, "cannot prepare metadata allocation test\n");
            s3_response_cleanup(&response);
            return 1;
        }
    }

    move_next_realloc = true;
    fail_malloc_after = allocation;
    strcpy(header, "x-amz-meta-ninth: value\r\n");
    (void) s3_headers_callback(header, 1, strlen(header), &response);
    if (!response.invalid_headers || response.metadata_count != 8 ||
        response.properties.metadata != response.metadata ||
        response.properties.metadata_count != response.metadata_count) {
        fprintf(stderr,
                "metadata ownership inconsistent after allocation %u fails\n",
                allocation);
        failed = 1;
        /* Allow the unfixed implementation to report failure safely. */
        response.properties.metadata = response.metadata;
        response.properties.metadata_count = response.metadata_count;
    }
    fail_malloc_after = 0;
    s3_response_cleanup(&response);
    if (response.metadata != NULL || response.properties.metadata != NULL ||
        response.metadata_count != 0 ||
        response.properties.metadata_count != 0) {
        fprintf(stderr, "metadata cleanup did not release ownership\n");
        failed = 1;
    }
    return failed;
}

static int test_property_replacement_failure(void) {
    struct s3_response response = {0};
    char first[] = "Content-Type: application/x-first\r\n";
    char second[] = "Content-Type: application/x-second\r\n";
    int failed = 0;

    s3_response_reset(&response);
    (void) s3_headers_callback(first, 1, strlen(first), &response);
    if (response.invalid_headers || response.properties.content_type == NULL)
        failed = 1;
    fail_malloc_after = 1;
    (void) s3_headers_callback(second, 1, strlen(second), &response);
    fail_malloc_after = 0;
    if (!response.invalid_headers || response.properties.content_type == NULL ||
        strcmp(response.properties.content_type, "application/x-first") != 0)
        failed = 1;
    s3_response_cleanup(&response);
    if (response.properties.content_type != NULL) failed = 1;
    if (failed)
        fprintf(stderr,
                "HTTP property ownership inconsistent after malloc failure\n");
    return failed;
}

static void test_array_growth(void) {
    for (size_t initial = 8; initial <= 16; initial *= 2) {
        size_t capacity = 0;
        fail_next_realloc = true;
        assert(s3_memory_grow(NULL, &capacity, sizeof(int), initial) == NULL);
        assert(capacity == 0);
        int *items = s3_memory_grow(NULL, &capacity, sizeof(*items), initial);
        assert(items != NULL && capacity == initial);
        for (size_t i = 0; i < capacity; ++i) items[i] = (int) i;
        fail_next_realloc = true;
        assert(s3_memory_grow(items, &capacity, sizeof(*items), initial) ==
               NULL);
        assert(capacity == initial);
        for (size_t i = 0; i < capacity; ++i) assert(items[i] == (int) i);
        int *grown = s3_memory_grow(items, &capacity, sizeof(*items), initial);
        assert(grown != NULL && capacity == initial * 2);
        for (size_t i = 0; i < initial; ++i) assert(grown[i] == (int) i);
        free(grown);
    }
    const size_t overflows[] = {SIZE_MAX, SIZE_MAX / 2, SIZE_MAX / 2 + 1};
    for (size_t i = 0; i < sizeof(overflows) / sizeof(*overflows); ++i) {
        size_t capacity = overflows[i];
        assert(s3_memory_grow(NULL, &capacity, 2, 8) == NULL);
        assert(capacity == overflows[i]);
    }
    size_t capacity = 0;
    assert(s3_memory_grow(NULL, &capacity, 2, SIZE_MAX) == NULL);
    assert(capacity == 0);
}

int main(void) {
    test_array_growth();
    int failed = test_growth_failure(1);
    failed |= test_growth_failure(2);
    failed |= test_property_replacement_failure();
    return failed;
}
