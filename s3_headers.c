/* SPDX-License-Identifier: MIT-0 */
#include "s3_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { S3_HEADERS_LIMIT = 256 * 1024 };

static int compare_metadata(const void *left, const void *right) {
    const struct s3_metadata *a = left;
    const struct s3_metadata *b = right;
    int result = strcmp(a->name, b->name);
    return result != 0 ? result : strcmp(a->value, b->value);
}

static unsigned char ascii_lower(unsigned char value) {
    if (value >= 'A' && value <= 'Z') {
        return (unsigned char) (value + ('a' - 'A'));
    }
    return value;
}

static void parse_status(struct s3_response *response, const char *buffer,
                         size_t size) {
    const char *end = buffer + size;
    const char *first = memchr(buffer, ' ', size);
    if (first == NULL) return;
    ++first;
    const char *last = first;
    while (last < end && *last >= '0' && *last <= '9') ++last;
    uint64_t status;
    if (s3_parse_u64(first, last, &status) && status <= LONG_MAX)
        response->status = (long) status;
}

static bool name_is(const char *line, size_t name_size, const char *name) {
    size_t expected = strlen(name);
    if (name_size != expected) return false;
    for (size_t i = 0; i < expected; ++i)
        if (ascii_lower((unsigned char) line[i]) !=
            ascii_lower((unsigned char) name[i]))
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

static bool replace_property(const char **target, const char *first,
                             const char *last) {
    size_t length = (size_t) (last - first);
    char *value = malloc(length + 1);
    if (value == NULL) return false;
    memcpy(value, first, length);
    value[length] = '\0';
    free((char *) *target);
    *target = value;
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
        response->properties.metadata = items;
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
        name_copy[i] = (char) ascii_lower((unsigned char) name[i]);
    name_copy[name_size] = '\0';
    memcpy(value_copy, first, value_size);
    value_copy[value_size] = '\0';
    response->metadata[response->metadata_count++] =
        (struct s3_metadata) {.name = name_copy, .value = value_copy};
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
    if (!s3_parse_u64(first, dash, &response->range_first) ||
        !s3_parse_u64(dash + 1, slash, &response->range_last) ||
        (*(slash + 1) != '*' &&
         !s3_parse_u64(slash + 1, last, &response->range_total)) ||
        (*(slash + 1) == '*' && slash + 2 != last))
        return;
    if (*(slash + 1) == '*') response->range_total = UINT64_MAX;
    if (response->range_first > response->range_last ||
        (*(slash + 1) != '*' &&
         response->range_last >= response->range_total)) {
        response->have_content_range = false;
        response->invalid_headers = true;
        return;
    }
    response->have_content_range = true;
}

static void parse_retry_after(struct s3_response *response, const char *first,
                              const char *last) {
    /* Bound server-controlled sleeps while honoring ordinary S3 delays. */
    enum { RETRY_AFTER_LIMIT_S = 300 };
    uint64_t seconds;
    if (!s3_parse_u64(first, last, &seconds)) {
        char date[128];
        time_t now = time(NULL);
        time_t deadline;
        if (!copy_value(date, sizeof(date), first, last) || now == (time_t) -1)
            return;
        deadline = curl_getdate(date, NULL);
        if (deadline == (time_t) -1) return;
        seconds = deadline > now ? (uint64_t) (deadline - now) : 0;
    }
    if (seconds > RETRY_AFTER_LIMIT_S) seconds = RETRY_AFTER_LIMIT_S;
    response->retry_after_ms = seconds * 1000;
    response->have_retry_after = true;
}

size_t s3_headers_callback(char *buffer, size_t size, size_t count,
                           void *data) {
    struct s3_response *response = data;
    size_t bytes;
    char *colon, *first, *last;
    if (size != 0 && count > SIZE_MAX / size) return 0;
    bytes = size * count;
    if (bytes > S3_HEADERS_LIMIT - response->header_bytes) {
        response->invalid_headers = true;
        return 0;
    }
    response->header_bytes += bytes;
    if (bytes >= 5 && memcmp(buffer, "HTTP/", 5) == 0) {
        s3_response_cleanup(response);
        s3_response_reset(response);
        response->header_bytes = bytes;
        parse_status(response, buffer, bytes);
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
            s3_parse_u64(first, last, &response->content_length);
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
        if (!replace_property(&response->properties.content_type, first, last))
            response->invalid_headers = true;
    }
    else if (name_is(buffer, (size_t) (colon - buffer), "Content-Encoding")) {
        if (!replace_property(&response->properties.content_encoding, first,
                              last))
            response->invalid_headers = true;
    }
    else if (name_is(buffer, (size_t) (colon - buffer), "Cache-Control")) {
        if (!replace_property(&response->properties.cache_control, first, last))
            response->invalid_headers = true;
    }
    else if (name_is(buffer, (size_t) (colon - buffer), "Retry-After")) {
        parse_retry_after(response, first, last);
    }
    else if (name_is(buffer, (size_t) (colon - buffer),
                     "Content-Disposition")) {
        if (!replace_property(&response->properties.content_disposition, first,
                              last))
            response->invalid_headers = true;
    }
    else if (name_is(buffer, (size_t) (colon - buffer), "Content-Language")) {
        if (!replace_property(&response->properties.content_language, first,
                              last))
            response->invalid_headers = true;
    }
    else if (name_is(buffer, (size_t) (colon - buffer), "Expires")) {
        if (!replace_property(&response->properties.expires, first, last))
            response->invalid_headers = true;
    }
    else if (name_is(buffer, (size_t) (colon - buffer),
                     "x-amz-bucket-region")) {
        if (!copy_value(response->bucket_region,
                        sizeof(response->bucket_region), first, last))
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

bool s3_headers_add(struct curl_slist **headers, const char *name,
                    const char *value) {
    size_t size;
    char *line;
    struct curl_slist *next;
    if (headers == NULL || name == NULL || value == NULL ||
        strpbrk(name, "\r\n:") != NULL || strpbrk(value, "\r\n") != NULL)
        return false;
    if (strlen(name) > SIZE_MAX - strlen(value) - 3) return false;
    size = strlen(name) + strlen(value) + 3;
    line = malloc(size);
    if (line == NULL) return false;
    /* libcurl treats "Name:" as removal; "Name;" sends an empty value. */
    if (value[0] == '\0')
        (void) snprintf(line, size, "%s;", name);
    else
        (void) snprintf(line, size, "%s: %s", name, value);
    next = curl_slist_append(*headers, line);
    free(line);
    if (next == NULL) return false;
    *headers = next;
    return true;
}
