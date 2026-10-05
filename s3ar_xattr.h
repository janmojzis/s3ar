/* SPDX-License-Identifier: MIT-0 */
#ifndef S3AR_XATTR_H
#define S3AR_XATTR_H

#include <signal.h>
#include <stddef.h>
#include <sys/types.h>

/* Raw bytes, without adding or removing a terminating NUL.
 * Failures return -1 and set errno. set also logs success or failure. */
ssize_t s3ar_xattr_get(int fd, const char *name, void *value, size_t capacity);
int s3ar_xattr_set(int fd, const char *name, const void *value, size_t size);

/* Remove user.s3ar.* only. Unsupported xattrs are a successful no-op.
 * interrupted may be NULL; a set flag returns -1 with errno = EINTR. */
int s3ar_xattr_reset(int fd, const volatile sig_atomic_t *interrupted);

/* Log bytes safely at debug verbosity, with a caller-supplied status. */
void s3ar_xattr_debug(const char *name, const void *value, size_t size,
                      const char *status);

#endif
