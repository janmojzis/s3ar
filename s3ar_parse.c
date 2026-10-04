/* SPDX-License-Identifier: MIT-0 */
#include "s3ar_parse.h"
#include "s3.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>

bool s3ar_parse_multipart_size(const char *text, size_t *result) {
    char *end;
    uintmax_t value, multiplier;
    if (text == NULL || !isdigit((unsigned char) text[0])) return false;
    errno = 0;
    value = strtoumax(text, &end, 10);
    if (errno != 0 || end == text) return false;
    if (*end == 'M')
        multiplier = UINT64_C(1024) * 1024;
    else if (*end == 'G')
        multiplier = UINT64_C(1024) * 1024 * 1024;
    else
        return false;
    if (end[1] != '\0' || value > UINTMAX_MAX / multiplier) return false;
    value *= multiplier;
    if (value < 5 * UINT64_C(1024) * 1024 ||
        value > S3_MULTIPART_MAX_PART_SIZE || value > SIZE_MAX)
        return false;
    *result = (size_t) value;
    return true;
}
