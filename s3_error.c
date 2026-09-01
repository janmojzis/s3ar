/* SPDX-License-Identifier: MIT-0 */
#include "s3_internal.h"

#include <stdio.h>
#include <string.h>

void s3_error_clear(struct s3_error *error) {
    if (error != NULL) memset(error, 0, sizeof(*error));
}

enum s3_result s3_error_set(struct s3_error *error, enum s3_result result,
                            const char *message) {
    if (error != NULL) {
        error->result = result;
        if (message != NULL)
            (void)snprintf(error->message, sizeof(error->message), "%s",
                           message);
    }
    return result;
}

const char *s3_result_name(enum s3_result result) {
    static const char *const names[] = {
        "ok",          "not found",      "precondition failed",
        "access denied", "callback error", "retry exhausted",
        "protocol error", "configuration error", "error"};
    if ((unsigned)result >= sizeof(names) / sizeof(names[0])) return "error";
    return names[result];
}
