/* SPDX-License-Identifier: MIT-0 */
#include "s3ar_transform.h"
#include "log.h"

#include <ctype.h>
#include <regex.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct s3ar_transform {
    regex_t regex;
    char *expression;
    char *replacement;
    bool global;
    struct s3ar_transform *next;
};

static bool fail(char *error, size_t capacity, const char *message) {
    (void) snprintf(error, capacity, "%s", message);
    return false;
}

static char *read_segment(const char **cursor, char delimiter, bool pattern) {
    const char *p = *cursor;
    char *segment = malloc(strlen(p) + 1);
    if (segment == NULL) return NULL;
    size_t used = 0;
    for (; *p != '\0' && *p != delimiter; ++p) {
        if (*p == '\\') {
            if (p[1] == '\0') break;
            if (p[1] != delimiter || !pattern ||
                strchr(".[*^$", delimiter) != NULL)
                segment[used++] = *p;
            segment[used++] = *++p;
        }
        else
            segment[used++] = *p;
    }
    if (*p != delimiter) {
        free(segment);
        return NULL;
    }
    segment[used] = '\0';
    *cursor = p + 1;
    return segment;
}

bool s3ar_transform_add(struct s3ar_transform **transforms,
                        const char *expression, char *error, size_t capacity) {
    if (expression[0] != 's' || expression[1] == '\0' ||
        isalnum((unsigned char) expression[1]) ||
        isspace((unsigned char) expression[1]) || expression[1] == '\\')
        return fail(
            error, capacity,
            "expected s<delimiter>REGEX<delimiter>REPLACEMENT<delimiter>[gi]");
    char delimiter = expression[1];
    const char *cursor = expression + 2;
    char *pattern = read_segment(&cursor, delimiter, true);
    if (pattern == NULL)
        return fail(error, capacity, "invalid pattern or out of memory");
    char *replacement = read_segment(&cursor, delimiter, false);
    if (replacement == NULL) {
        free(pattern);
        return fail(error, capacity, "invalid replacement or out of memory");
    }
    int flags = 0;
    bool global = false;
    for (; *cursor != '\0'; ++cursor) {
        if (*cursor == 'g')
            global = true;
        else if (*cursor == 'i')
            flags |= REG_ICASE;
        else {
            free(pattern);
            free(replacement);
            return fail(
                error, capacity,
                "unsupported transform flag (only g and i are supported)");
        }
    }
    struct s3ar_transform *item = calloc(1, sizeof(*item));
    if (item == NULL) {
        free(pattern);
        free(replacement);
        return fail(error, capacity, "out of memory");
    }
    int result = regcomp(&item->regex, pattern, flags);
    free(pattern);
    if (result != 0) {
        (void) regerror(result, &item->regex, error, capacity);
        free(replacement);
        free(item);
        return false;
    }
    for (const char *p = replacement; *p != '\0'; ++p) {
        if (*p != '\\') continue;
        ++p;
        if (*p >= '1' && *p <= '9') {
            if ((size_t) (*p - '0') <= item->regex.re_nsub) continue;
            (void) fail(error, capacity,
                        "replacement refers to a nonexistent capture group");
        }
        else if (*p == '\\' || *p == '&' || *p == delimiter)
            continue;
        else
            (void) fail(error, capacity, "unsupported replacement escape");
        regfree(&item->regex);
        free(replacement);
        free(item);
        return false;
    }
    item->expression = strdup(expression);
    if (item->expression == NULL) {
        regfree(&item->regex);
        free(replacement);
        free(item);
        return fail(error, capacity, "out of memory");
    }
    item->replacement = replacement;
    item->global = global;
    while (*transforms != NULL) transforms = &(*transforms)->next;
    *transforms = item;
    return true;
}

struct text_buffer {
    char *text;
    size_t size;
    size_t capacity;
};

static bool append(struct text_buffer *buffer, const char *text, size_t size) {
    if (size >= SIZE_MAX - buffer->size) return false;
    size_t required = buffer->size + size + 1;
    if (required > buffer->capacity) {
        size_t capacity = buffer->capacity == 0 ? 64 : buffer->capacity;
        while (capacity < required) {
            if (capacity > SIZE_MAX / 2) {
                capacity = required;
                break;
            }
            capacity *= 2;
        }
        char *text = realloc(buffer->text, capacity);
        if (text == NULL) return false;
        buffer->text = text;
        buffer->capacity = capacity;
    }
    if (size != 0) memcpy(buffer->text + buffer->size, text, size);
    buffer->size += size;
    buffer->text[buffer->size] = '\0';
    return true;
}

static bool utf8_boundary(const char *text, size_t offset) {
    return ((unsigned char) text[offset] & 0xc0) != 0x80;
}

static bool replace_match(struct text_buffer *buffer, const char *replacement,
                          const char *text, const regmatch_t matches[10],
                          char *error, size_t capacity) {
    for (const char *p = replacement; *p != '\0'; ++p) {
        int group = -1;
        if (*p == '&')
            group = 0;
        else if (*p == '\\') {
            ++p;
            if (*p >= '1' && *p <= '9') group = *p - '0';
        }
        if (group >= 0) {
            if (matches[group].rm_so < 0) continue;
            if (!utf8_boundary(text, (size_t) matches[group].rm_so) ||
                !utf8_boundary(text, (size_t) matches[group].rm_eo))
                return fail(error, capacity,
                            "transform capture group splits a UTF-8 character");
            if (!append(buffer, text + matches[group].rm_so,
                        (size_t) (matches[group].rm_eo - matches[group].rm_so)))
                return fail(error, capacity, "out of memory");
        }
        else if (!append(buffer, p, 1))
            return fail(error, capacity, "out of memory");
    }
    return true;
}

static char *apply_one(const struct s3ar_transform *item, const char *name,
                       char *error, size_t capacity) {
    struct text_buffer output = {0};
    size_t length = strlen(name), cursor = 0;
    bool previous_nonempty = false;
    for (;;) {
        regmatch_t matches[10];
        int result = regexec(&item->regex, name + cursor, 10, matches,
                             cursor != 0 ? REG_NOTBOL : 0);
        if (result == REG_NOMATCH) {
            if (!append(&output, name + cursor, length - cursor)) goto oom;
            return output.text;
        }
        if (result != 0) {
            (void) regerror(result, &item->regex, error, capacity);
            free(output.text);
            return NULL;
        }
        size_t start = cursor + (size_t) matches[0].rm_so;
        size_t end = cursor + (size_t) matches[0].rm_eo;
        if (!utf8_boundary(name, start) || !utf8_boundary(name, end)) {
            (void) fail(error, capacity,
                        "transform match splits a UTF-8 character");
            goto failure;
        }
        if (!append(&output, name + cursor, start - cursor)) goto oom;
        /* sed does not replace an empty match immediately following a
         * nonempty match. Advancing also makes empty global matches finite. */
        bool skip = previous_nonempty && start == cursor && start == end;
        if (!skip && !replace_match(&output, item->replacement, name + cursor,
                                    matches, error, capacity))
            goto failure;
        if (!item->global) {
            if (!append(&output, name + end, length - end)) goto oom;
            return output.text;
        }
        if (start == end) {
            if (end == length) return output.text;
            cursor = end + 1;
            while (cursor < length && !utf8_boundary(name, cursor)) ++cursor;
            if (!append(&output, name + end, cursor - end)) goto oom;
            previous_nonempty = false;
        }
        else {
            cursor = end;
            previous_nonempty = true;
        }
    }
oom:
    (void) fail(error, capacity, "out of memory");
failure:
    free(output.text);
    return NULL;
}

char *s3ar_transform_apply(const struct s3ar_transform *transforms,
                           const char *name, char *error, size_t capacity) {
    char *result = strdup(name);
    if (result == NULL) {
        (void) fail(error, capacity, "out of memory");
        return NULL;
    }
    for (; transforms != NULL; transforms = transforms->next) {
        char *next = apply_one(transforms, result, error, capacity);
        free(result);
        if (next == NULL) return NULL;
        result = next;
    }
    return result;
}

void s3ar_transform_log(const struct s3ar_transform *transforms) {
    if (transforms == NULL) log_d1("(option --transform) transform = '(none)'");
    for (; transforms != NULL; transforms = transforms->next)
        log_d3("(option --transform) transform = '", transforms->expression,
               "'");
}

void s3ar_transform_free(struct s3ar_transform *transforms) {
    while (transforms != NULL) {
        struct s3ar_transform *next = transforms->next;
        regfree(&transforms->regex);
        free(transforms->expression);
        free(transforms->replacement);
        free(transforms);
        transforms = next;
    }
}
