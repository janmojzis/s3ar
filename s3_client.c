/* SPDX-License-Identifier: MIT-0 */
#include "s3_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if LIBCURL_VERSION_NUM < 0x074b00
#error "s3ar requires libcurl 7.75.0 or newer"
#endif

char *s3_strdup(const char *value) {
    size_t size;
    char *copy;
    if (value == NULL) return NULL;
    size = strlen(value) + 1;
    copy = malloc(size);
    if (copy != NULL) memcpy(copy, value, size);
    return copy;
}

void s3_secure_free(char *value) {
    volatile unsigned char *p;
    size_t size;
    if (value == NULL) return;
    size = strlen(value);
    p = (volatile unsigned char *)value;
    while (size-- != 0) *p++ = 0;
    free(value);
}

static bool config_present(const char *value) {
    return value != NULL && value[0] != '\0';
}

enum s3_result s3_client_open(struct s3_client **result,
                              struct s3_error *error,
                              const struct s3_client_config *config) {
    struct s3_client *client = NULL;
    CURLcode code;
    s3_error_clear(error);
    if (result != NULL) *result = NULL;
    if (config == NULL || result == NULL || !config_present(config->endpoint) ||
        !config_present(config->region) ||
        !config_present(config->access_key) ||
        !config_present(config->secret_key) ||
        (config->uri_style != S3_URI_STYLE_PATH &&
         config->uri_style != S3_URI_STYLE_VIRTUAL))
        return s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                            "incomplete S3 client configuration");

    code = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (code != CURLE_OK)
        return s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                            "libcurl initialization failed");
    client = calloc(1, sizeof(*client));
    if (client == NULL) {
        curl_global_cleanup();
        return s3_error_set(error, S3_RESULT_ERROR, "out of memory");
    }
    client->endpoint = s3_strdup(config->endpoint);
    client->region = s3_strdup(config->region);
    client->access_key = s3_strdup(config->access_key);
    client->secret_key = s3_strdup(config->secret_key);
    client->session_token = s3_strdup(config->session_token);
    client->user_agent = s3_strdup(config_present(config->user_agent)
                                       ? config->user_agent
                                       : "s3/0");
    if (client->endpoint == NULL || client->region == NULL ||
        client->access_key == NULL || client->secret_key == NULL ||
        client->user_agent == NULL ||
        (config->session_token != NULL && client->session_token == NULL))
        goto oom;
    {
        size_t size = strlen(client->access_key) + strlen(client->secret_key) + 2;
        client->credentials = malloc(size);
        if (client->credentials == NULL) goto oom;
        (void)snprintf(client->credentials, size, "%s:%s", client->access_key,
                       client->secret_key);
    }
    client->uri_style = config->uri_style;
    client->max_attempts = config->max_attempts != 0 ? config->max_attempts : 5;
    client->connect_timeout_ms = config->connect_timeout_ms != 0
                                     ? config->connect_timeout_ms
                                     : 10000;
    client->low_speed_time_s = config->low_speed_time_s != 0
                                   ? config->low_speed_time_s
                                   : 30;
    client->curl = curl_easy_init();
    if (client->curl == NULL) {
        s3_client_close(client);
        return s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                            "cannot create libcurl handle");
    }
    /* Validate the endpoint independently of object names. */
    {
        char *probe = NULL;
        enum s3_result status = s3_build_url(client, "probe", "probe", &probe,
                                             error);
        free(probe);
        if (status != S3_RESULT_OK) {
            s3_client_close(client);
            return status;
        }
    }
    *result = client;
    return S3_RESULT_OK;

oom:
    s3_client_close(client);
    return s3_error_set(error, S3_RESULT_ERROR, "out of memory");
}

void s3_client_close(struct s3_client *client) {
    if (client == NULL) return;
    if (client->curl != NULL) curl_easy_cleanup(client->curl);
    free(client->endpoint);
    free(client->region);
    s3_secure_free(client->access_key);
    s3_secure_free(client->secret_key);
    s3_secure_free(client->credentials);
    s3_secure_free(client->session_token);
    free(client->user_agent);
    free(client);
    curl_global_cleanup();
}
