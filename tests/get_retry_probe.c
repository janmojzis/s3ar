/* SPDX-License-Identifier: MIT-0 */
#include "s3ar_config.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct output {
    bool fail;
};

static bool write_output(void *data, const unsigned char *buffer, size_t size) {
    struct output *output = data;
    if (output->fail) {
        errno = ENOSPC;
        return false;
    }
    while (size != 0) {
        ssize_t written = write(STDOUT_FILENO, buffer, size);
        if (written < 0) {
            if (errno == EINTR) continue;
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

int main(int argc, char **argv) {
    struct s3ar_config_env config;
    struct s3_client *client = NULL;
    struct s3_error error = {0};
    struct output output = {0};
    if (argc == 2 && strcmp(argv[1], "fail-write") == 0)
        output.fail = true;
    else if (argc != 1) {
        (void) fprintf(stderr, "usage: test-get-retry [fail-write]\n");
        return 2;
    }
    enum s3_result result = s3ar_config_from_env(&config, &error);

    if (result == S3_RESULT_OK) {
        config.client.max_attempts = 2;
        result = s3_client_open(&client, &error, &config.client);
    }
    if (result == S3_RESULT_OK)
        result = s3_object_get(client, &error, NULL, write_output, &output,
                               "bucket", "key");

    if (result != S3_RESULT_OK) {
        (void) fprintf(stderr,
                       "result=%s attempts=%u http=%ld s3=%s callback=%d "
                       "message=%s\n",
                       s3_result_name(result), error.attempts,
                       error.http_status, error.s3_code, error.callback_errno,
                       error.message);
    }
    s3_client_close(client);
    s3ar_config_free(&config);
    return result == S3_RESULT_OK ? 0 : 1;
}
