/* SPDX-License-Identifier: MIT-0 */
#ifndef S3AR_CLIENT_H
#define S3AR_CLIENT_H

struct s3_client;
struct s3ar_config_env;

/* Initialize from the environment and diagnose failures. Return 0 on success,
 * 2 for environment errors, 1 for client errors. Caller owns client/config. */
int s3ar_client_open(struct s3_client **client, struct s3ar_config_env *config);

#endif
