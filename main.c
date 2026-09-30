/* SPDX-License-Identifier: MIT-0 */
/* Select the command from the name used to invoke this binary. */
#include <string.h>
#include <unistd.h>

#include "main.h"

int main(int argc, char **argv) {
    if (argc < 1 || argv == NULL || argv[0] == NULL) _exit(100);

    const char *name = strrchr(argv[0], '/');
    name = name != NULL ? name + 1 : argv[0];

    if (strcmp(name, "s3ar-put") == 0) return main_s3ar_put(argc, argv);
    if (strcmp(name, "s3ar-get") == 0) return main_s3ar_get(argc, argv);
    if (strcmp(name, "s3ar-delete") == 0) return main_s3ar_delete(argc, argv);
    if (strcmp(name, "s3ar-list") == 0) return main_s3ar_list(argc, argv);
    return main_s3ar(argc, argv);
}
