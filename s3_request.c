/* SPDX-License-Identifier: MIT-0 */
#include "s3_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SETOPT(option, value)                                                  \
    do {                                                                       \
        CURLcode setopt_code = curl_easy_setopt(client->curl, option, value);  \
        if (setopt_code != CURLE_OK)                                           \
            return s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,          \
                                curl_easy_strerror(setopt_code));              \
    } while (0)

bool s3_add_header(struct curl_slist **headers, const char *name,
                   const char *value) {
    size_t size;
    char *line;
    struct curl_slist *next;
    if (headers == NULL || name == NULL || value == NULL ||
        strpbrk(name, "\r\n:") != NULL || strpbrk(value, "\r\n") != NULL)
        return false;
    if (strlen(name) > SIZE_MAX - strlen(value) - 3) return false;
    size = strlen(name) + strlen(value) + 3;
    line = malloc(size);
    if (line == NULL) return false;
    (void) snprintf(line, size, "%s: %s", name, value);
    next = curl_slist_append(*headers, line);
    free(line);
    if (next == NULL) return false;
    *headers = next;
    return true;
}

enum s3_result s3_prepare_request(struct s3_client *client, const char *url,
                                  struct curl_slist **headers,
                                  struct s3_error *error) {
    char sigv4[256];
    curl_easy_reset(client->curl);
    if (snprintf(sigv4, sizeof(sigv4), "aws:amz:%s:s3", client->region) >=
        (int) sizeof(sigv4))
        return s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                            "S3 region is too long");
    if (client->session_token != NULL) {
        size_t size = strlen(client->session_token) + 23;
        char *token = malloc(size);
        struct curl_slist *next;
        if (token == NULL)
            return s3_error_set(error, S3_RESULT_ERROR, "out of memory");
        (void) snprintf(token, size, "x-amz-security-token: %s",
                        client->session_token);
        next = curl_slist_append(*headers, token);
        free(token);
        if (next == NULL)
            return s3_error_set(error, S3_RESULT_ERROR, "out of memory");
        *headers = next;
    }
    SETOPT(CURLOPT_URL, url);
    SETOPT(CURLOPT_AWS_SIGV4, sigv4);
    SETOPT(CURLOPT_USERPWD, client->credentials);
    SETOPT(CURLOPT_HTTPHEADER, *headers);
    SETOPT(CURLOPT_USERAGENT, client->user_agent);
    /* 8.14+ canonicalizes the SigV4 path itself and rejects PATH_AS_IS when
     * SigV4 is enabled. Older supported releases need PATH_AS_IS to preserve
     * the already percent-encoded object key. */
#if LIBCURL_VERSION_NUM < 0x080e00
    SETOPT(CURLOPT_PATH_AS_IS, 1L);
#endif
    SETOPT(CURLOPT_FOLLOWLOCATION, 0L);
    SETOPT(CURLOPT_NOSIGNAL, 1L);
    SETOPT(CURLOPT_CONNECTTIMEOUT_MS, (long) client->connect_timeout_ms);
    SETOPT(CURLOPT_LOW_SPEED_LIMIT, 1L);
    SETOPT(CURLOPT_LOW_SPEED_TIME, (long) client->low_speed_time_s);
    SETOPT(CURLOPT_SSL_VERIFYPEER, 1L);
    SETOPT(CURLOPT_SSL_VERIFYHOST, 2L);
    return S3_RESULT_OK;
}
