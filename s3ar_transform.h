/* SPDX-License-Identifier: MIT-0 */
#ifndef S3AR_TRANSFORM_H____
#define S3AR_TRANSFORM_H____

#include <stdbool.h>
#include <stddef.h>

struct s3ar_transform;

bool s3ar_transform_add(struct s3ar_transform **transforms,
                        const char *expression, char *error, size_t capacity);
char *s3ar_transform_apply(const struct s3ar_transform *transforms,
                           const char *name, char *error, size_t capacity);
void s3ar_transform_log(const struct s3ar_transform *transforms);
void s3ar_transform_free(struct s3ar_transform *transforms);

#endif
