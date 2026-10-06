/* SPDX-License-Identifier: MIT-0 */
#include "s3_internal.h"
#include "secure_free.h"
#include <openssl/evp.h>

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

static int stop_interrupted_transfer(void *data, curl_off_t downloaded_total,
                                     curl_off_t downloaded_now,
                                     curl_off_t uploaded_total,
                                     curl_off_t uploaded_now) {
    const struct s3_client *client = data;
    (void) downloaded_total;
    (void) downloaded_now;
    (void) uploaded_total;
    (void) uploaded_now;
    return client->cancel_callback != NULL &&
           client->cancel_callback(client->cancel_data);
}

enum s3_result s3_request_prepare(struct s3_client *client, const char *url,
                                  struct curl_slist **headers,
                                  struct s3_error *error) {
    char sigv4[256];
    CURLUcode url_code;
    if (client->cancel_callback != NULL &&
        client->cancel_callback(client->cancel_data))
        return s3_error_set(error, S3_RESULT_ERROR, "interrupted");
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
        secure_free(token);
        if (next == NULL)
            return s3_error_set(error, S3_RESULT_ERROR, "out of memory");
        *headers = next;
    }
    url_code = curl_url_set(client->url, CURLUPART_URL, url, CURLU_PATH_AS_IS);
    if (url_code != CURLUE_OK)
        return s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                            curl_url_strerror(url_code));
    SETOPT(CURLOPT_CURLU, client->url);
    SETOPT(CURLOPT_AWS_SIGV4, sigv4);
    SETOPT(CURLOPT_USERPWD, client->credentials);
    SETOPT(CURLOPT_HTTPHEADER, *headers);
    SETOPT(CURLOPT_USERAGENT, client->user_agent);
    /* Preserve legal S3 keys containing dot components in the URL handle.
     * CURLOPT_PATH_AS_IS cannot be combined with SigV4 in libcurl 8.14+. */
    SETOPT(CURLOPT_FOLLOWLOCATION, 0L);
    SETOPT(CURLOPT_NOSIGNAL, 1L);
    SETOPT(CURLOPT_CONNECTTIMEOUT_MS, (long) client->connect_timeout_ms);
    SETOPT(CURLOPT_LOW_SPEED_LIMIT, 1L);
    SETOPT(CURLOPT_LOW_SPEED_TIME, (long) client->low_speed_time_s);
    SETOPT(CURLOPT_SSL_VERIFYPEER, 1L);
    SETOPT(CURLOPT_SSL_VERIFYHOST, 2L);
    if (client->cancel_callback != NULL) {
        SETOPT(CURLOPT_NOPROGRESS, 0L);
        SETOPT(CURLOPT_XFERINFOFUNCTION, stop_interrupted_transfer);
        SETOPT(CURLOPT_XFERINFODATA, client);
    }
    return S3_RESULT_OK;
}

CURLcode s3_request_perform(struct s3_client *client, unsigned attempt,
                            unsigned max_attempts, struct s3_response *response,
                            curl_write_callback write_callback,
                            void *write_data, char curl_error[CURL_ERROR_SIZE],
                            struct s3_error *error) {
    (void) curl_easy_setopt(client->curl, CURLOPT_HEADERFUNCTION,
                            s3_headers_callback);
    (void) curl_easy_setopt(client->curl, CURLOPT_HEADERDATA, response);
    (void) curl_easy_setopt(client->curl, CURLOPT_WRITEFUNCTION,
                            write_callback);
    (void) curl_easy_setopt(client->curl, CURLOPT_WRITEDATA, write_data);
    (void) curl_easy_setopt(client->curl, CURLOPT_ERRORBUFFER, curl_error);
    s3_trace_perform_start(client, attempt, max_attempts);
    CURLcode code = curl_easy_perform(client->curl);
    (void) curl_easy_getinfo(client->curl, CURLINFO_RESPONSE_CODE,
                             &response->status);
    s3_error_clear(error);
    error->attempts = attempt;
    error->http_status = response->status;
    s3_error_parse_xml(response->error_body, response->error_body_size, error);
    return code;
}

enum s3_result s3_request_result(CURLcode code,
                                 const struct s3_response *response,
                                 const char *curl_error,
                                 struct s3_error *error) {
    enum s3_result result =
        s3_result_from_response(code, response, false, error);
    if (code != CURLE_OK && curl_error[0] != '\0')
        (void) snprintf(error->message, sizeof(error->message), "%s",
                        curl_error);
    return result;
}

enum s3_result s3_request_url(struct s3_client *client, struct s3_error *error,
                              const char *url, const char *method,
                              const char *request_body, char **response_body,
                              size_t *response_size) {
    struct s3_memory_response context = {0};
    enum s3_result result =
        s3_error_set(error, S3_RESULT_ERROR, "unreachable S3 request state");
    if (response_body != NULL) *response_body = NULL;
    if (response_size != NULL) *response_size = 0;
    for (unsigned attempt = 1; attempt <= client->max_attempts; ++attempt) {
        struct curl_slist *headers = NULL;
        CURLcode code;
        char curl_error[CURL_ERROR_SIZE] = {0};
        s3_response_memory_reset(&context, S3_XML_BODY_LIMIT);
        if (request_body != NULL &&
            !s3_headers_add(&headers, "Content-Type", "application/xml")) {
            curl_slist_free_all(headers);
            result = s3_error_set(error, S3_RESULT_ERROR,
                                  "cannot prepare XML request headers");
            break;
        }
        if (request_body != NULL && strcmp(method, "POST") == 0) {
            unsigned char digest[EVP_MAX_MD_SIZE];
            unsigned int digest_size = 0;
            unsigned char encoded[25];
            if (EVP_Digest(request_body, strlen(request_body), digest,
                           &digest_size, EVP_md5(), NULL) != 1 ||
                digest_size != 16 ||
                EVP_EncodeBlock(encoded, digest, (int) digest_size) != 24 ||
                !s3_headers_add(&headers, "Content-MD5", (char *) encoded)) {
                curl_slist_free_all(headers);
                result = s3_error_set(error, S3_RESULT_ERROR,
                                      "cannot prepare XML POST headers");
                break;
            }
        }
        result = s3_request_prepare(client, url, &headers, error);
        if (result != S3_RESULT_OK) {
            curl_slist_free_all(headers);
            break;
        }
        (void) curl_easy_setopt(client->curl, CURLOPT_CUSTOMREQUEST, method);
        if (strcmp(method, "HEAD") == 0)
            (void) curl_easy_setopt(client->curl, CURLOPT_NOBODY, 1L);
        if (request_body != NULL) {
            (void) curl_easy_setopt(client->curl, CURLOPT_POSTFIELDS,
                                    request_body);
            (void) curl_easy_setopt(client->curl, CURLOPT_POSTFIELDSIZE_LARGE,
                                    (curl_off_t) strlen(request_body));
        }
        else if (strcmp(method, "PUT") == 0) {
            (void) s3_headers_add(&headers, "Content-Length", "0");
            (void) curl_easy_setopt(client->curl, CURLOPT_HTTPHEADER, headers);
        }
        code = s3_request_perform(client, attempt, client->max_attempts,
                                  &context.response, s3_response_memory_collect,
                                  &context, curl_error, error);
        curl_slist_free_all(headers);
        s3_trace_perform_end(client, attempt, client->max_attempts, code,
                             &context.response, error);
        result = s3_response_check_headers(
            &context.response, "invalid or oversized S3 response headers",
            error);
        if (result != S3_RESULT_OK) break;
        if (context.body_error != S3_RESULT_OK) {
            result = s3_error_set(error, context.body_error,
                                  context.body_error == S3_RESULT_ERROR
                                      ? "out of memory"
                                      : "S3 XML response is too large");
            break;
        }
        if (code == CURLE_OK && context.response.status >= 200 &&
            context.response.status < 300) {
            result = S3_RESULT_OK;
            break;
        }
        if (!s3_retry_allowed(code, context.response.status, error->s3_code)) {
            result =
                s3_request_result(code, &context.response, curl_error, error);
            break;
        }
        if (attempt == client->max_attempts) {
            result = s3_error_set(error, S3_RESULT_RETRY_EXHAUSTED,
                                  "S3 request retry limit exhausted");
            break;
        }
        s3_retry_delay(client, attempt, client->max_attempts, code,
                       &context.response, error);
    }
    if (result == S3_RESULT_OK && response_body != NULL) {
        *response_body = context.body;
        if (response_size != NULL) *response_size = context.size;
        context.body = NULL;
    }
    s3_response_memory_cleanup(&context);
    return result;
}

enum s3_result s3_request_bucket(struct s3_client *client,
                                 struct s3_error *error, const char *bucket,
                                 const char *query, const char *method,
                                 const char *request_body, char **response_body,
                                 size_t *response_size) {
    char *url = NULL;
    enum s3_result result =
        s3_url_build_bucket(client, bucket, query, &url, error);
    if (result != S3_RESULT_OK) return result;
    result = s3_request_url(client, error, url, method, request_body,
                            response_body, response_size);
    free(url);
    return result;
}
