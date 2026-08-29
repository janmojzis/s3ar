/* SPDX-License-Identifier: MIT-0 */

#include "die.h"
#include "s3ar.h"

#include <stdio.h>

void log_s3_name(const char *bucket, const char *key) {
    int result = key == NULL ? fprintf(stdout, "s3://%s\n", bucket)
                             : fprintf(stdout, "s3://%s/%s\n", bucket, key);
    if (result < 0) {
        die_fatal("s3ar: unable to write standard output", bucket, key);
    }
}
