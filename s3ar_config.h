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
/* Initialize from the environment and diagnose failures. Return 0 on success,
 * 2 for environment errors, 1 for client errors. Caller owns client/config. */
int s3ar_client_open(struct s3_client **client, struct s3ar_config_env *config);

/* SIZE uses an integer with an M/G suffix, between 5 MiB and 5 GiB. */
bool s3ar_parse_multipart_size(const char *text, size_t *result);

#endif
