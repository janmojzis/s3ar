/* SPDX-License-Identifier: MIT-0 */

#include "die.h"
#include "s3ar.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

void s3ar_selection_parse(struct s3ar_selection *selection, const char *uri) {
    *selection = (struct s3ar_selection) {.uri = uri};
    if (strcmp(uri, "s3://") == 0) { return; }
    if (strncmp(uri, "s3://", 5) != 0 || uri[5] == '\0' || uri[5] == '/') {
        errno = 0;
        die_fatal("s3ar: invalid S3 operand", uri, NULL);
    }

    selection->storage = strdup(uri + 5);
    if (selection->storage == NULL) {
        die_fatal("s3ar: out of memory", NULL, NULL);
    }
    size_t length = strlen(selection->storage);
    while (length > 0 && selection->storage[length - 1] == '/') {
        selection->storage[--length] = '\0';
    }

    char *key = strchr(selection->storage, '/');
    selection->bucket = selection->storage;
    if (key != NULL) {
        *key++ = '\0';
        selection->key = key;
    }
}

void s3ar_selection_free(struct s3ar_selection *selection) {
    free(selection->storage);
    *selection = (struct s3ar_selection) {0};
}
