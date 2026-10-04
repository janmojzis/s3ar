/* SPDX-License-Identifier: MIT-0 */
#include "s3ar_client.h"
#include "s3ar_config.h"
#include "s3ar_log.h"

int s3ar_client_open(struct s3_client **client,
                     struct s3ar_config_env *config) {
    struct s3_error error = {0};
    *client = NULL;
    if (s3ar_config_from_env(config, &error) != S3_RESULT_OK) {
        log_f2("invalid configuration: ", s3ar_log_error(&error));
        return 2;
    }
    if (s3_client_open(client, &error, &config->client) != S3_RESULT_OK) {
        log_f2("unable to initialize S3 client: ", s3ar_log_error(&error));
        return 1;
    }
    return 0;
}
