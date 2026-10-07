/* SPDX-License-Identifier: MIT-0 */
#include "s3ar.h"
#include "s3ar_client.h"
#include "s3ar_config.h"
#include "s3ar_transform.h"

#include <stdlib.h>

static struct s3ar_config config = {.multipart_size = S3_MULTIPART_PART_SIZE};
static struct s3ar_config_env s3_config;

struct s3ar_config *s3ar_config_get(void) { return &config; }

int s3ar_connect(void) { return s3ar_client_open(&config.s3, &s3_config); }

_Noreturn void s3ar_die(int status) {
    s3ar_create_cleanup();
    s3_client_close(config.s3);
    s3ar_config_free(&s3_config);
    s3ar_transform_free(config.transforms);
    exit(status);
}
