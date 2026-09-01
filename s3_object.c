/* SPDX-License-Identifier: MIT-0 */
#include "s3_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct get_context {
    struct s3_response response;
    s3_properties_callback properties_callback;
    s3_write_callback write_callback;
    void *callback_data;
    struct s3_object_properties original_properties;
    uint64_t delivered;
    bool original_known;
    bool properties_called;
    bool callback_failed;
    bool protocol_failed;
    bool response_validated;
    char protocol_error[256];
    int callback_errno;
};

static enum s3_result validate_get_headers(struct get_context *context,
                                           struct s3_error *error) {
    struct s3_response *r = &context->response;
    if (context->response_validated) return S3_RESULT_OK;
    if (!r->headers_done || r->invalid_headers)
        return s3_error_set(error, S3_RESULT_PROTOCOL_ERROR,
                            "response body arrived before complete headers");
    if (!context->original_known) {
        if (r->status != 200 || !r->have_length ||
            r->properties.etag[0] == '\0')
            return s3_error_set(error, S3_RESULT_PROTOCOL_ERROR,
                                "GET response lacks Content-Length or ETag");
        context->original_properties = r->properties;
        context->original_properties.size = r->content_length;
        context->original_known = true;
    }
    else if (context->delivered != 0) {
        uint64_t remaining =
            context->original_properties.size - context->delivered;
        if (r->status != 206 || !r->have_content_range || !r->have_length ||
            r->range_first != context->delivered ||
            r->range_total != context->original_properties.size ||
            r->content_length != remaining ||
            strcmp(r->properties.etag, context->original_properties.etag) !=
                0 ||
            r->properties.last_modified !=
                context->original_properties.last_modified ||
            strcmp(r->properties.content_type,
                   context->original_properties.content_type) != 0 ||
            strcmp(r->properties.content_encoding,
                   context->original_properties.content_encoding) != 0 ||
            strcmp(r->properties.cache_control,
                   context->original_properties.cache_control) != 0) {
            if (error != NULL) {
                error->result = S3_RESULT_PROTOCOL_ERROR;
                (void) snprintf(
                    error->message, sizeof(error->message),
                    "invalid resumed GET: status=%ld range=%d %llu-%llu/%llu "
                    "length=%llu offset=%llu total=%llu remaining=%llu "
                    "etag-match=%d",
                    r->status, r->have_content_range,
                    (unsigned long long) r->range_first,
                    (unsigned long long) r->range_last,
                    (unsigned long long) r->range_total,
                    (unsigned long long) r->content_length,
                    (unsigned long long) context->delivered,
                    (unsigned long long) context->original_properties.size,
                    (unsigned long long) remaining,
                    strcmp(r->properties.etag,
                           context->original_properties.etag) == 0);
            }
            return S3_RESULT_PROTOCOL_ERROR;
        }
    }
    if (!context->properties_called && context->properties_callback != NULL) {
        context->properties_called = true;
        if (!context->properties_callback(context->callback_data,
                                          &context->original_properties)) {
            context->callback_failed = true;
            context->callback_errno = errno != 0 ? errno : EIO;
            if (error != NULL) error->callback_errno = context->callback_errno;
            return s3_error_set(error, S3_RESULT_CALLBACK_ERROR,
                                "properties callback failed");
        }
    }
    context->response_validated = true;
    return S3_RESULT_OK;
}

static size_t collect_body(char *buffer, size_t size, size_t count,
                           void *data) {
    struct get_context *context = data;
    struct s3_response *r = &context->response;
    size_t bytes;
    struct s3_error ignored;
    if (size != 0 && count > SIZE_MAX / size) return 0;
    bytes = size * count;
    if (r->status < 200 || r->status >= 300) {
        size_t room = S3_ERROR_BODY_LIMIT - r->error_body_size;
        size_t copy = bytes < room ? bytes : room;
        memcpy(r->error_body + r->error_body_size, buffer, copy);
        r->error_body_size += copy;
        r->error_body[r->error_body_size] = '\0';
        return bytes;
    }
    s3_error_clear(&ignored);
    if (validate_get_headers(context, &ignored) != S3_RESULT_OK) {
        context->protocol_failed = true;
        (void) snprintf(context->protocol_error,
                        sizeof(context->protocol_error), "%s", ignored.message);
        return 0;
    }
    if (bytes > UINT64_MAX - context->delivered ||
        context->delivered + bytes > context->original_properties.size) {
        context->protocol_failed = true;
        return 0;
    }
    if (bytes != 0 &&
        !context->write_callback(context->callback_data,
                                 (const unsigned char *) buffer, bytes)) {
        context->callback_failed = true;
        context->callback_errno = errno != 0 ? errno : EIO;
        return 0;
    }
    context->delivered += bytes;
    return bytes;
}

static bool add_header(struct curl_slist **headers, const char *name,
                       const char *value) {
    size_t size = strlen(name) + strlen(value) + 3;
    char *line = malloc(size);
    struct curl_slist *next;
    if (line == NULL) return false;
    (void) snprintf(line, size, "%s: %s", name, value);
    next = curl_slist_append(*headers, line);
    free(line);
    if (next == NULL) return false;
    *headers = next;
    return true;
}

enum s3_result s3_object_get(struct s3_client *client, struct s3_error *error,
                             s3_properties_callback properties_callback,
                             s3_write_callback write_callback, void *data,
                             const char *bucket, const char *key) {
    struct get_context context = {.properties_callback = properties_callback,
                                  .write_callback = write_callback,
                                  .callback_data = data};
    char *url = NULL;
    enum s3_result result;
    if (client == NULL || write_callback == NULL)
        return s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                            "invalid GET arguments");
    s3_error_clear(error);
    result = s3_build_url(client, bucket, key, &url, error);
    if (result != S3_RESULT_OK) return result;

    for (unsigned attempt = 1; attempt <= client->max_attempts; ++attempt) {
        struct curl_slist *headers = NULL;
        CURLcode code;
        char curl_error[CURL_ERROR_SIZE] = {0};
        char range[64];
        bool retryable;
        if (attempt > 1) s3_response_cleanup(&context.response);
        s3_response_reset(&context.response);
        context.callback_failed = false;
        context.protocol_failed = false;
        context.response_validated = false;
        context.protocol_error[0] = '\0';
        context.callback_errno = 0;
        if (context.delivered != 0) {
            (void) snprintf(range, sizeof(range), "bytes=%llu-",
                            (unsigned long long) context.delivered);
            if (!add_header(&headers, "Range", range) ||
                !add_header(&headers, "If-Match",
                            context.original_properties.etag)) {
                curl_slist_free_all(headers);
                free(url);
                return s3_error_set(error, S3_RESULT_ERROR, "out of memory");
            }
        }
        result = s3_prepare_request(client, url, &headers, error);
        if (result != S3_RESULT_OK) {
            curl_slist_free_all(headers);
            free(url);
            return result;
        }
        (void) curl_easy_setopt(client->curl, CURLOPT_HTTPGET, 1L);
        (void) curl_easy_setopt(client->curl, CURLOPT_HEADERFUNCTION,
                                s3_header_callback);
        (void) curl_easy_setopt(client->curl, CURLOPT_HEADERDATA,
                                &context.response);
        (void) curl_easy_setopt(client->curl, CURLOPT_WRITEFUNCTION,
                                collect_body);
        (void) curl_easy_setopt(client->curl, CURLOPT_WRITEDATA, &context);
        (void) curl_easy_setopt(client->curl, CURLOPT_ERRORBUFFER, curl_error);
        code = curl_easy_perform(client->curl);
        (void) curl_easy_getinfo(client->curl, CURLINFO_RESPONSE_CODE,
                                 &context.response.status);
        curl_slist_free_all(headers);
        s3_error_clear(error);
        error->attempts = attempt;
        error->http_status = context.response.status;
        error->callback_errno = context.callback_errno;
        s3_parse_error_xml(context.response.error_body,
                           context.response.error_body_size, error);

        if (context.callback_failed) {
            free(url);
            s3_response_cleanup(&context.response);
            return s3_error_set(error, S3_RESULT_CALLBACK_ERROR,
                                "output callback failed");
        }
        if (context.protocol_failed) {
            free(url);
            s3_response_cleanup(&context.response);
            return s3_error_set(error, S3_RESULT_PROTOCOL_ERROR,
                                context.protocol_error[0] != '\0'
                                    ? context.protocol_error
                                    : "invalid GET response headers or length");
        }
        if (code == CURLE_OK && context.response.status >= 200 &&
            context.response.status < 300) {
            result = validate_get_headers(&context, error);
            if (result == S3_RESULT_OK &&
                context.delivered != context.original_properties.size)
                result = s3_error_set(error, S3_RESULT_PROTOCOL_ERROR,
                                      "GET response has an incorrect length");
            free(url);
            s3_response_cleanup(&context.response);
            return result;
        }
        if (context.response.status == 412) {
            free(url);
            s3_response_cleanup(&context.response);
            return s3_error_set(error, S3_RESULT_PRECONDITION_FAILED,
                                "object changed during download");
        }
        retryable =
            s3_is_retryable(code, context.response.status, error->s3_code);
        if (!retryable) {
            result =
                s3_result_from_response(code, &context.response, false, error);
            if (code != CURLE_OK && curl_error[0] != '\0')
                (void) snprintf(error->message, sizeof(error->message), "%s",
                                curl_error);
            free(url);
            s3_response_cleanup(&context.response);
            return result;
        }
        if (attempt == client->max_attempts) {
            free(url);
            s3_response_cleanup(&context.response);
            return s3_error_set(error, S3_RESULT_RETRY_EXHAUSTED,
                                "S3 GET retry limit exhausted");
        }
        s3_retry_delay(attempt - 1);
    }
    free(url);
    s3_response_cleanup(&context.response);
    return s3_error_set(error, S3_RESULT_ERROR, "unreachable GET state");
}

static size_t discard_body(char *buffer, size_t size, size_t count,
                           void *data) {
    size_t bytes;
    (void) buffer;
    (void) data;
    if (size != 0 && count > SIZE_MAX / size) return 0;
    bytes = size * count;
    return bytes;
}

enum s3_result s3_object_head(struct s3_client *client, struct s3_error *error,
                              struct s3_object_properties *properties,
                              const char *bucket, const char *key) {
    char *url = NULL;
    enum s3_result result;
    struct s3_response response = {0};
    if (client == NULL || properties == NULL)
        return s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                            "invalid HEAD arguments");
    s3_error_clear(error);
    result = s3_build_url(client, bucket, key, &url, error);
    if (result != S3_RESULT_OK) return result;
    for (unsigned attempt = 1; attempt <= client->max_attempts; ++attempt) {
        struct curl_slist *headers = NULL;
        CURLcode code;
        char curl_error[CURL_ERROR_SIZE] = {0};
        if (attempt > 1) s3_response_cleanup(&response);
        s3_response_reset(&response);
        result = s3_prepare_request(client, url, &headers, error);
        if (result != S3_RESULT_OK) {
            curl_slist_free_all(headers);
            free(url);
            return result;
        }
        (void) curl_easy_setopt(client->curl, CURLOPT_NOBODY, 1L);
        (void) curl_easy_setopt(client->curl, CURLOPT_HEADERFUNCTION,
                                s3_header_callback);
        (void) curl_easy_setopt(client->curl, CURLOPT_HEADERDATA, &response);
        (void) curl_easy_setopt(client->curl, CURLOPT_WRITEFUNCTION,
                                discard_body);
        (void) curl_easy_setopt(client->curl, CURLOPT_WRITEDATA, &response);
        (void) curl_easy_setopt(client->curl, CURLOPT_ERRORBUFFER, curl_error);
        code = curl_easy_perform(client->curl);
        (void) curl_easy_getinfo(client->curl, CURLINFO_RESPONSE_CODE,
                                 &response.status);
        curl_slist_free_all(headers);
        s3_error_clear(error);
        error->attempts = attempt;
        error->http_status = response.status;
        if (code == CURLE_OK && response.status >= 200 &&
            response.status < 300) {
            if (!response.have_length || response.invalid_headers) {
                free(url);
                return s3_error_set(error, S3_RESULT_PROTOCOL_ERROR,
                                    "HEAD response lacks Content-Length");
            }
            *properties = response.properties;
            response.properties.metadata = NULL;
            response.properties.metadata_count = 0;
            response.metadata = NULL;
            response.metadata_count = 0;
            response.metadata_capacity = 0;
            free(url);
            return S3_RESULT_OK;
        }
        if (!s3_is_retryable(code, response.status, NULL)) {
            result = s3_result_from_response(code, &response, false, error);
            if (code != CURLE_OK && curl_error[0] != '\0')
                (void) snprintf(error->message, sizeof(error->message), "%s",
                                curl_error);
            free(url);
            s3_response_cleanup(&response);
            return result;
        }
        if (attempt == client->max_attempts) {
            free(url);
            s3_response_cleanup(&response);
            return s3_error_set(error, S3_RESULT_RETRY_EXHAUSTED,
                                "S3 HEAD retry limit exhausted");
        }
        s3_retry_delay(attempt - 1);
    }
    free(url);
    s3_response_cleanup(&response);
    return S3_RESULT_ERROR;
}
