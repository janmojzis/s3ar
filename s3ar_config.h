/* SPDX-License-Identifier: MIT-0 */
#ifndef S3AR_CONFIG_H
#define S3AR_CONFIG_H

#include "s3.h"

/* Pointers in client remain valid until s3ar_config_free(). */
struct s3ar_config_env {
    struct s3_client_config client;
    char *endpoint;
    char *region;
    char *access_key;
    char *secret_key;
    char *session_token;
};

enum s3_result s3ar_config_from_env(struct s3ar_config_env *config,
                                    struct s3_error *error);
void s3ar_config_free(struct s3ar_config_env *config);

#endif
