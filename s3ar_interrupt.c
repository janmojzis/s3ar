/* SPDX-License-Identifier: MIT-0 */
#include "s3ar_interrupt.h"

static bool interrupted(void *data) {
    return *(const volatile sig_atomic_t *) data != 0;
}

void s3ar_interrupt_bind(struct s3_client *client,
                         volatile sig_atomic_t *flag) {
    s3_client_set_cancel_callback(client, flag != NULL ? interrupted : NULL,
                                  (void *) flag);
}
