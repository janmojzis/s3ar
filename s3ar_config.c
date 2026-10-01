/* SPDX-License-Identifier: MIT-0 */
#include "s3ar_config.h"
#include "s3ar_log.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static enum s3_result config_error_set(struct s3_error *error,
                                       enum s3_result result,
                                       const char *message) {
    if (error != NULL) {
        error->result = result;
        (void) snprintf(error->message, sizeof(error->message), "%s", message);
    }
    return result;
}

static void secure_free(char *value) {
    if (value == NULL) return;
    volatile unsigned char *p = (volatile unsigned char *) value;
    size_t size = strlen(value);
    while (size-- != 0) *p++ = 0;
    free(value);
}

static enum s3_result copy_env(char **target, const char *name, bool required,
                               struct s3_error *error) {
    const char *value = getenv(name);
    size_t size;
    if (value == NULL || value[0] == '\0') {
        if (!required) return S3_RESULT_OK;
        char message[256];
        (void) snprintf(message, sizeof(message), "$%s not set", name);
        return config_error_set(error, S3_RESULT_CONFIGURATION_ERROR, message);
    }
    size = strlen(value) + 1;
    *target = malloc(size);
    if (*target == NULL)
        return config_error_set(error, S3_RESULT_ERROR, "out of memory");
    memcpy(*target, value, size);
    return S3_RESULT_OK;
}

enum s3_result s3ar_config_from_env(struct s3ar_config_env *config,
                                    struct s3_error *error) {
    const char *style;
    enum s3_result status;
    if (error != NULL) memset(error, 0, sizeof(*error));
    if (config == NULL || error == NULL)
        return config_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                                "invalid S3 configuration arguments");
    memset(config, 0, sizeof(*config));
    s3_config_init(&config->client);
    if ((status = copy_env(&config->endpoint, "S3AR_ENDPOINT", true, error)) !=
            S3_RESULT_OK ||
        (status = copy_env(&config->access_key, "S3AR_ACCESS_KEY", true,
                           error)) != S3_RESULT_OK ||
        (status = copy_env(&config->secret_key, "S3AR_SECRET_KEY", true,
                           error)) != S3_RESULT_OK ||
        (status = copy_env(&config->session_token, "S3AR_SESSION_TOKEN", false,
                           error)) != S3_RESULT_OK)
        goto fail;
    if (getenv("S3AR_REGION") != NULL && getenv("S3AR_REGION")[0] != '\0') {
        status = copy_env(&config->region, "S3AR_REGION", true, error);
        if (status != S3_RESULT_OK) goto fail;
    }
    else {
        config->region = strdup("us-east-1");
        if (config->region == NULL) {
            status = config_error_set(error, S3_RESULT_ERROR, "out of memory");
            goto fail;
        }
    }
    style = getenv("S3AR_URI_STYLE");
    if (style == NULL || style[0] == '\0' || strcmp(style, "path") == 0)
        config->client.uri_style = S3_URI_STYLE_PATH;
    else if (strcmp(style, "virtual") == 0)
        config->client.uri_style = S3_URI_STYLE_VIRTUAL;
    else {
        status =
            config_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                             "$S3AR_URI_STYLE must be 'path' or 'virtual'");
        goto fail;
    }
    config->client.endpoint = config->endpoint;
    config->client.region = config->region;
    config->client.access_key = config->access_key;
    config->client.secret_key = config->secret_key;
    config->client.session_token = config->session_token;
    config->client.user_agent = "s3ar/0.1";
    return S3_RESULT_OK;
fail:
    s3ar_config_free(config);
    return status;
}

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

void s3ar_config_free(struct s3ar_config_env *config) {
    if (config == NULL) return;
    free(config->endpoint);
    free(config->region);
    secure_free(config->access_key);
    secure_free(config->secret_key);
    secure_free(config->session_token);
    memset(config, 0, sizeof(*config));
}

bool s3ar_parse_multipart_size(const char *text, size_t *result) {
    char *end;
    uintmax_t value, multiplier;
    if (text == NULL || !isdigit((unsigned char) text[0])) return false;
    errno = 0;
    value = strtoumax(text, &end, 10);
    if (errno != 0 || end == text) return false;
    if (*end == 'M')
        multiplier = UINT64_C(1024) * 1024;
    else if (*end == 'G')
        multiplier = UINT64_C(1024) * 1024 * 1024;
    else
        return false;
    if (end[1] != '\0' || value > UINTMAX_MAX / multiplier) return false;
    value *= multiplier;
    if (value < 5 * UINT64_C(1024) * 1024 ||
        value > S3_MULTIPART_MAX_PART_SIZE || value > SIZE_MAX)
        return false;
    *result = (size_t) value;
    return true;
}
