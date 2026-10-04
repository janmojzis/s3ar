/* SPDX-License-Identifier: MIT-0 */
#include "s3_internal.h"

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
