/* SPDX-License-Identifier: MIT-0 */
#include <s3ar/s3.h>
#include <s3ar/s3_log.h>

#include <stdio.h>

int main(void) {
    struct s3_client_config config;
    struct s3_client *client = NULL;
    struct s3_error error = {0};

    s3_config_init(&config);
    config.endpoint = "http://127.0.0.1:1";
    config.region = "us-east-1";
    config.access_key = "example";
    config.secret_key = "example";
    if (s3_client_open(&client, &error, &config) != S3_RESULT_OK) {
        log_f2("unable to initialize S3 client: ", s3_log_error(&error));
        return 1;
    }
    s3_client_close(client);
    (void) puts("S3 client initialized");
    return 0;
}
