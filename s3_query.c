/* SPDX-License-Identifier: MIT-0 */
#include "s3_internal.h"

#include <stdlib.h>
#include <string.h>

char *s3_query_build(const char *base, const struct s3_query_param *params) {
    size_t capacity = strlen(base) + 1;
    for (const struct s3_query_param *p = params; p->name != NULL; ++p) {
        if (p->value == NULL) continue;
        size_t name_size = strlen(p->name), value_size = strlen(p->value);
        if (capacity > SIZE_MAX - 2 || name_size > SIZE_MAX - capacity - 2 ||
            value_size > (SIZE_MAX - capacity - name_size - 2) / 3)
            return NULL;
        capacity += name_size + 2 + value_size * 3;
    }
    char *query = malloc(capacity);
    if (query == NULL) return NULL;
    strcpy(query, base);
    char *end = query + strlen(query);
    for (const struct s3_query_param *p = params; p->name != NULL; ++p) {
        if (p->value == NULL) continue;
        if (end != query) *end++ = '&';
        size_t name_size = strlen(p->name);
        memcpy(end, p->name, name_size);
        end += name_size;
        *end++ = '=';
        if (!s3_uri_encode_into(end, capacity - (size_t) (end - query),
                                p->value, false)) {
            free(query);
            return NULL;
        }
        end += strlen(end);
    }
    return query;
}
