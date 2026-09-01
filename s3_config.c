/* SPDX-License-Identifier: MIT-0 */
#include "s3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static enum s3_result copy_env(char **target, const char *name, bool required,
                               struct s3_error *error) {
    const char *value = getenv(name);
    size_t size;
    if (value == NULL || value[0] == '\0') {
        if (!required) return S3_RESULT_OK;
        error->result = S3_RESULT_CONFIGURATION_ERROR;
        (void)snprintf(error->message, sizeof(error->message), "$%s not set",
                       name);
        return error->result;
    }
    size = strlen(value) + 1;
    *target = malloc(size);
    if (*target == NULL) {
        error->result = S3_RESULT_ERROR;
        (void)snprintf(error->message, sizeof(error->message), "out of memory");
        return error->result;
    }
    memcpy(*target, value, size);
    return S3_RESULT_OK;
}

enum s3_result s3_config_from_env(struct s3_config *config,
                                  struct s3_error *error) {
    const char *style;
    enum s3_result status;
    if (config == NULL || error == NULL) return S3_RESULT_CONFIGURATION_ERROR;
    memset(config, 0, sizeof(*config));
    memset(error, 0, sizeof(*error));
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
    } else {
        config->region = strdup("us-east-1");
        if (config->region == NULL) {
            status = S3_RESULT_ERROR;
            error->result = status;
            (void)snprintf(error->message, sizeof(error->message),
                           "out of memory");
            goto fail;
        }
    }
    style = getenv("S3AR_URI_STYLE");
    if (style == NULL || style[0] == '\0' || strcmp(style, "path") == 0)
        config->client.uri_style = S3_URI_STYLE_PATH;
    else if (strcmp(style, "virtual") == 0)
        config->client.uri_style = S3_URI_STYLE_VIRTUAL;
    else {
        status = S3_RESULT_CONFIGURATION_ERROR;
        error->result = status;
        (void)snprintf(error->message, sizeof(error->message),
                       "$S3AR_URI_STYLE must be 'path' or 'virtual'");
        goto fail;
    }
    config->client.endpoint = config->endpoint;
    config->client.region = config->region;
    config->client.access_key = config->access_key;
    config->client.secret_key = config->secret_key;
    config->client.session_token = config->session_token;
    config->client.user_agent = "s3ar/0.1";
    config->client.max_attempts = 5;
    config->client.connect_timeout_ms = 10000;
    config->client.low_speed_time_s = 30;
    return S3_RESULT_OK;
fail:
    s3_config_free(config);
    return status;
}

void s3_config_free(struct s3_config *config) {
    volatile unsigned char *p;
    size_t n;
    if (config == NULL) return;
    free(config->endpoint);
    free(config->region);
    free(config->access_key);
    if (config->secret_key != NULL) {
        p = (volatile unsigned char *)config->secret_key;
        n = strlen(config->secret_key);
        while (n-- != 0) *p++ = 0;
        free(config->secret_key);
    }
    if (config->session_token != NULL) {
        p = (volatile unsigned char *)config->session_token;
        n = strlen(config->session_token);
        while (n-- != 0) *p++ = 0;
        free(config->session_token);
    }
    memset(config, 0, sizeof(*config));
}
