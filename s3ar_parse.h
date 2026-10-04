/* SPDX-License-Identifier: MIT-0 */
#ifndef S3AR_PARSE_H
#define S3AR_PARSE_H

#include <stdbool.h>
#include <stddef.h>

/* SIZE uses an integer with an M/G suffix, between 5 MiB and 5 GiB. */
bool s3ar_parse_multipart_size(const char *text, size_t *result);

#endif
