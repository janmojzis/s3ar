/* SPDX-License-Identifier: MIT-0 */
#ifndef SECURE_FREE_H
#define SECURE_FREE_H

/* Overwrite a NUL-terminated string before freeing it. NULL is a no-op. */
void secure_free(char *value);

#endif
