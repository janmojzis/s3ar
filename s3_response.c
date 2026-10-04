/* SPDX-License-Identifier: MIT-0 */
#include "s3_internal.h"

#include <stdlib.h>
#include <string.h>

bool s3_parse_u64(const char *first, const char *last, uint64_t *value) {
    uint64_t n = 0;
    if (first == last) return false;
    for (const char *p = first; p != last; ++p) {
        unsigned digit;
        if (*p < '0' || *p > '9') return false;
        digit = (unsigned) (*p - '0');
        if (n > (UINT64_MAX - digit) / 10) return false;
        n = n * 10 + digit;
    }
    *value = n;
    return true;
}

void s3_response_cleanup(struct s3_response *response) {
    if (response == NULL) return;
    s3_object_properties_free(&response->properties);
    response->metadata = NULL;
    response->metadata_count = 0;
    response->metadata_capacity = 0;
}

void s3_response_take_properties(struct s3_response *response,
                                 struct s3_object_properties *properties) {
    *properties = response->properties;
    response->properties = (struct s3_object_properties) {0};
    response->metadata = NULL;
    response->metadata_count = 0;
    response->metadata_capacity = 0;
}

enum s3_result s3_response_check_headers(const struct s3_response *response,
                                         const char *message,
                                         struct s3_error *error) {
    if (response->invalid_headers)
        return s3_error_set(error, S3_RESULT_PROTOCOL_ERROR, message);
    return S3_RESULT_OK;
}

void s3_response_reset(struct s3_response *response) {
    memset(response, 0, sizeof(*response));
    response->range_total = UINT64_MAX;
}

void s3_response_memory_reset(struct s3_memory_response *response,
                              size_t limit) {
    s3_response_cleanup(&response->response);
    free(response->body);
    memset(response, 0, sizeof(*response));
    response->limit = limit;
    s3_response_reset(&response->response);
}

void s3_response_memory_cleanup(struct s3_memory_response *response) {
    if (response == NULL) return;
    s3_response_cleanup(&response->response);
    free(response->body);
    memset(response, 0, sizeof(*response));
}

size_t s3_response_memory_collect(char *buffer, size_t size, size_t count,
                                  void *data) {
    struct s3_memory_response *output = data;
    size_t bytes;
    size_t needed;
    size_t capacity;
    char *body;
    if (size != 0 && count > SIZE_MAX / size) return 0;
    bytes = size * count;
    if (output->response.status < 200 || output->response.status >= 300) {
        size_t room = S3_ERROR_BODY_LIMIT - output->response.error_body_size;
        size_t copy = bytes < room ? bytes : room;
        memcpy(output->response.error_body + output->response.error_body_size,
               buffer, copy);
        output->response.error_body_size += copy;
        output->response.error_body[output->response.error_body_size] = '\0';
        return bytes;
    }
    if (bytes > output->limit - output->size) {
        output->body_error = S3_RESULT_PROTOCOL_ERROR;
        return 0;
    }
    needed = output->size + bytes + 1;
    if (needed > output->capacity) {
        capacity = output->capacity == 0 ? 4096 : output->capacity;
        while (capacity < needed) {
            size_t maximum = output->limit + 1;
            if (capacity > maximum / 2) {
                capacity = maximum;
                break;
            }
            capacity *= 2;
        }
        body = realloc(output->body, capacity);
        if (body == NULL) {
            output->body_error = S3_RESULT_ERROR;
            return 0;
        }
        output->body = body;
        output->capacity = capacity;
    }
    memcpy(output->body + output->size, buffer, bytes);
    output->size += bytes;
    output->body[output->size] = '\0';
    return bytes;
}
