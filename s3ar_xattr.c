/* SPDX-License-Identifier: MIT-0 */
#include "s3ar_xattr.h"
#include "log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/xattr.h>

struct xattr_log_value {
    const char *value;
    size_t size;
    const char *status;
};

static void format_xattr_value(FILE *stream, const void *data) {
    const struct xattr_log_value *value = data;
    if (value->value == NULL)
        (void) fputs(" = (unavailable)", stream);
    else {
        (void) fputs(" = '", stream);
        for (size_t i = 0; i < value->size; ++i) {
            if (value->value[i] == '\'') (void) fputc('\\', stream);
            log_write_data(stream, value->value + i, 1);
        }
        (void) fputc('\'', stream);
    }
    (void) fputs(" (", stream);
    log_write_text(stream, value->status);
    (void) fputc(')', stream);
}

void s3ar_xattr_debug(const char *name, const void *value, size_t size,
                      const char *status) {
    struct xattr_log_value data = {value, size, status};
    log_d3("xattr ", name, log_custom(format_xattr_value, &data));
}

ssize_t s3ar_xattr_get(int fd, const char *name, void *value, size_t capacity) {
    return fgetxattr(fd, name, value, capacity);
}

int s3ar_xattr_set(int fd, const char *name, const void *value, size_t size) {
    int result = fsetxattr(fd, name, value, size, 0);
    int saved_errno = errno;
    if (result == 0)
        s3ar_xattr_debug(name, value, size, "saved");
    else
        log_w4("xattr ", name, ": write failed: ", log_errno());
    errno = saved_errno;
    return result;
}

/* Other tools and archive extraction may have added further s3ar attributes. */
int s3ar_xattr_reset(int output_fd, const volatile sig_atomic_t *interrupted) {
    char *names;
    ssize_t size;
    for (;;) {
        if (interrupted != NULL && *interrupted != 0) {
            errno = EINTR;
            return -1;
        }
        size = flistxattr(output_fd, NULL, 0);
        if (size < 0) return errno == ENOTSUP ? 0 : -1;
        if (size == 0) return 0;
        names = malloc((size_t) size);
        if (names == NULL) return -1;
        size = flistxattr(output_fd, names, (size_t) size);
        if (size >= 0) break;
        int saved_errno = errno;
        free(names);
        if (saved_errno == ERANGE) continue;
        errno = saved_errno;
        return saved_errno == ENOTSUP ? 0 : -1;
    }
    for (char *name = names; name < names + size; name += strlen(name) + 1) {
        if (strncmp(name, "user.s3ar.", sizeof("user.s3ar.") - 1) != 0)
            continue;
        if (fremovexattr(output_fd, name) != 0 && errno != ENODATA) {
            int saved_errno = errno;
            free(names);
            errno = saved_errno;
            return -1;
        }
    }
    free(names);
    return 0;
}
