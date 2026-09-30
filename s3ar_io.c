/* SPDX-License-Identifier: MIT-0 */
#include "s3ar_io.h"

#include <errno.h>
#include <unistd.h>

enum s3_read_result s3ar_io_read(void *data, unsigned char *buffer,
                                 size_t capacity, size_t *size) {
    const struct s3ar_io_read_context *context = data;
    ssize_t amount;
    if (*context->interrupted != 0) {
        errno = EINTR;
        *size = 0;
        return S3_READ_ERROR;
    }
    do {
        amount = read(*context->fd, buffer, capacity);
    } while (amount < 0 && errno == EINTR && *context->interrupted == 0);
    if (amount < 0) {
        *size = 0;
        return S3_READ_ERROR;
    }
    *size = (size_t) amount;
    return amount == 0 ? S3_READ_EOF : S3_READ_DATA;
}

#include <errno.h>
#include <unistd.h>

bool s3ar_io_write(void *data, const unsigned char *buffer, size_t size) {
    const struct s3ar_io_write_context *context = data;
    while (size != 0) {
        ssize_t written;
        if (*context->interrupted != 0) {
            errno = EINTR;
            return false;
        }
        written = write(*context->fd, buffer, size);
        if (written < 0) {
            if (errno == EINTR && *context->interrupted == 0) continue;
            return false;
        }
        if (written == 0) {
            errno = EIO;
            return false;
        }
        buffer += (size_t) written;
        size -= (size_t) written;
    }
    return true;
}
