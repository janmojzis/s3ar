/* SPDX-License-Identifier: MIT-0 */
#ifndef S3AR_IO_H
#define S3AR_IO_H

#include "s3.h"

#include <signal.h>

struct s3ar_io_read_context {
    int *fd;
    const volatile sig_atomic_t *interrupted;
};

struct s3ar_io_write_context {
    int *fd;
    const volatile sig_atomic_t *interrupted;
};

enum s3_read_result s3ar_io_read(void *data, unsigned char *buffer,
                                 size_t capacity, size_t *size);
bool s3ar_io_write(void *data, const unsigned char *buffer, size_t size);

#endif
