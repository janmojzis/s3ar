/* SPDX-License-Identifier: MIT-0 */
#include "s3_internal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

enum { S3_HEADER_LIMIT = 256 * 1024, S3_METADATA_LIMIT = 128 };

static int compare_metadata(const void *left, const void *right) {
    const struct s3_metadata *a = left;
    const struct s3_metadata *b = right;
    int result = strcmp(a->name, b->name);
    return result != 0 ? result : strcmp(a->value, b->value);
}

void s3_object_properties_free(struct s3_object_properties *properties) {
    struct s3_metadata *metadata;
    if (properties == NULL) return;
    metadata = (struct s3_metadata *) properties->metadata;
    for (size_t i = 0; i < properties->metadata_count; ++i) {
        free((char *) metadata[i].name);
        free((char *) metadata[i].value);
    }
    free(metadata);
    properties->metadata = NULL;
    properties->metadata_count = 0;
}

void s3_response_cleanup(struct s3_response *response) {
    if (response == NULL) return;
    s3_object_properties_free(&response->properties);
    response->metadata = NULL;
    response->metadata_count = 0;
    response->metadata_capacity = 0;
}

void s3_response_reset(struct s3_response *response) {
    memset(response, 0, sizeof(*response));
    response->range_total = UINT64_MAX;
}

static bool parse_u64(const char *first, const char *last, uint64_t *value) {
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

static bool name_is(const char *line, size_t name_size, const char *name) {
    size_t expected = strlen(name);
    if (name_size != expected) return false;
    for (size_t i = 0; i < expected; ++i)
        if (tolower((unsigned char) line[i]) !=
            tolower((unsigned char) name[i]))
            return false;
    return true;
}

static bool copy_value(char *target, size_t capacity, const char *first,
                       const char *last) {
    size_t size = (size_t) (last - first);
    if (size >= capacity) return false;
    memcpy(target, first, size);
    target[size] = '\0';
    return true;
}

static bool append_metadata(struct s3_response *response, const char *name,
                            size_t name_size, const char *first,
                            const char *last) {
    struct s3_metadata *items;
    size_t value_size = (size_t) (last - first);
    size_t capacity;
    char *name_copy, *value_copy;
    if (name_size == 0 || response->metadata_count >= S3_METADATA_LIMIT)
        return false;
    if (response->metadata_count == response->metadata_capacity) {
        capacity = response->metadata_capacity == 0
                       ? 8
                       : response->metadata_capacity * 2;
        items = realloc(response->metadata, capacity * sizeof(*items));
        if (items == NULL) return false;
        response->metadata = items;
        response->metadata_capacity = capacity;
    }
    name_copy = malloc(name_size + 1);
    value_copy = malloc(value_size + 1);
    if (name_copy == NULL || value_copy == NULL) {
        free(name_copy);
        free(value_copy);
        return false;
    }
    for (size_t i = 0; i < name_size; ++i)
        name_copy[i] = (char) tolower((unsigned char) name[i]);
    name_copy[name_size] = '\0';
    memcpy(value_copy, first, value_size);
    value_copy[value_size] = '\0';
    response->metadata[response->metadata_count++] =
        (struct s3_metadata) {.name = name_copy, .value = value_copy};
    response->properties.metadata = response->metadata;
    response->properties.metadata_count = response->metadata_count;
    return true;
}

static void parse_content_range(struct s3_response *response, const char *first,
                                const char *last) {
    const char *dash, *slash;
    if ((size_t) (last - first) < 8 || memcmp(first, "bytes ", 6) != 0) return;
    first += 6;
    dash = memchr(first, '-', (size_t) (last - first));
    if (dash == NULL) return;
    slash = memchr(dash + 1, '/', (size_t) (last - dash - 1));
    if (slash == NULL || slash + 1 == last) return;
    if (!parse_u64(first, dash, &response->range_first) ||
        !parse_u64(dash + 1, slash, &response->range_last) ||
        (*(slash + 1) != '*' &&
         !parse_u64(slash + 1, last, &response->range_total)) ||
        (*(slash + 1) == '*' && slash + 2 != last))
        return;
    if (*(slash + 1) == '*') response->range_total = UINT64_MAX;
    response->have_content_range = true;
}

size_t s3_header_callback(char *buffer, size_t size, size_t count, void *data) {
    struct s3_response *response = data;
    size_t bytes;
    char *colon, *first, *last;
    if (size != 0 && count > SIZE_MAX / size) return 0;
    bytes = size * count;
    if (bytes > S3_HEADER_LIMIT - response->header_bytes) {
        response->invalid_headers = true;
        return 0;
    }
    response->header_bytes += bytes;
    if (bytes >= 5 && memcmp(buffer, "HTTP/", 5) == 0) {
        char *space = memchr(buffer, ' ', bytes);
        s3_response_cleanup(response);
        s3_response_reset(response);
        response->header_bytes = bytes;
        if (space != NULL) response->status = strtol(space + 1, NULL, 10);
        return bytes;
    }
    if ((bytes == 2 && buffer[0] == '\r' && buffer[1] == '\n') ||
        (bytes == 1 && buffer[0] == '\n')) {
        if (response->metadata_count > 1)
            qsort(response->metadata, response->metadata_count,
                  sizeof(*response->metadata), compare_metadata);
        response->headers_done = true;
        return bytes;
    }
    colon = memchr(buffer, ':', bytes);
    if (colon == NULL) return bytes;
    first = colon + 1;
    last = buffer + bytes;
    while (first < last && (*first == ' ' || *first == '\t')) ++first;
    while (last > first && (last[-1] == '\r' || last[-1] == '\n' ||
                            last[-1] == ' ' || last[-1] == '\t'))
        --last;
    if (name_is(buffer, (size_t) (colon - buffer), "Content-Length")) {
        response->have_length =
            parse_u64(first, last, &response->content_length);
        response->properties.size = response->content_length;
    }
    else if (name_is(buffer, (size_t) (colon - buffer), "Content-Range")) {
        parse_content_range(response, first, last);
    }
    else if (name_is(buffer, (size_t) (colon - buffer), "ETag")) {
        if (!copy_value(response->properties.etag,
                        sizeof(response->properties.etag), first, last))
            response->invalid_headers = true;
    }
    else if (name_is(buffer, (size_t) (colon - buffer), "Content-Type")) {
        if (!copy_value(response->properties.content_type,
                        sizeof(response->properties.content_type), first, last))
            response->invalid_headers = true;
    }
    else if (name_is(buffer, (size_t) (colon - buffer), "Content-Encoding")) {
        if (!copy_value(response->properties.content_encoding,
                        sizeof(response->properties.content_encoding), first,
                        last))
            response->invalid_headers = true;
    }
    else if (name_is(buffer, (size_t) (colon - buffer), "Cache-Control")) {
        if (!copy_value(response->properties.cache_control,
                        sizeof(response->properties.cache_control), first,
                        last))
            response->invalid_headers = true;
    }
    else if (name_is(buffer, (size_t) (colon - buffer), "Last-Modified")) {
        char date[128];
        if (copy_value(date, sizeof(date), first, last))
            response->properties.last_modified =
                (int64_t) curl_getdate(date, NULL);
        else
            response->invalid_headers = true;
    }
    else if ((size_t) (colon - buffer) > 11 &&
             name_is(buffer, 11, "x-amz-meta-")) {
        if (!append_metadata(response, buffer + 11,
                             (size_t) (colon - buffer) - 11, first, last))
            response->invalid_headers = true;
    }
    return bytes;
}
