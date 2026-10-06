/* SPDX-License-Identifier: MIT-0 */
#include "secure_free.h"

#include <stdlib.h>
#include <string.h>

void secure_free(char *value) {
    if (value == NULL) return;
    volatile unsigned char *p = (volatile unsigned char *) value;
    size_t size = strlen(value);
    while (size-- != 0) *p++ = 0;
    free(value);
}
