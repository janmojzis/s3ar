/* SPDX-License-Identifier: MIT-0 */
#ifndef S3AR_INTERRUPT_H
#define S3AR_INTERRUPT_H

#include "s3.h"

#include <signal.h>

/* The flag must remain valid until the client is closed or rebound. */
void s3ar_interrupt_bind(struct s3_client *client, volatile sig_atomic_t *flag);

#endif
