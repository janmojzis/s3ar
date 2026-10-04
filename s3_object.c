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

static bool metadata_equal(const struct s3_object_properties *left,
                           const struct s3_object_properties *right) {
    if (left->metadata_count != right->metadata_count) return false;
    for (size_t i = 0; i < left->metadata_count; ++i) {
        if (strcmp(left->metadata[i].name, right->metadata[i].name) != 0 ||
            strcmp(left->metadata[i].value, right->metadata[i].value) != 0)
            return false;
    }
    return true;
}

static bool property_equal(const char *left, const char *right) {
    return strcmp(left != NULL ? left : "", right != NULL ? right : "") == 0;
}

static void preserve_original_properties(struct get_context *context) {
    struct s3_response *response = &context->response;
    context->original_properties = response->properties;
    response->properties.content_type = NULL;
    response->properties.content_encoding = NULL;
    response->properties.cache_control = NULL;
    response->properties.content_disposition = NULL;
    response->properties.content_language = NULL;
    response->properties.expires = NULL;
    response->properties.metadata = NULL;
    response->properties.metadata_count = 0;
    response->metadata = NULL;
    response->metadata_count = 0;
    response->metadata_capacity = 0;
}

static void cleanup_get_context(struct get_context *context) {
    s3_response_cleanup(&context->response);
    s3_object_properties_free(&context->original_properties);
}

static enum s3_result validate_get_headers(struct get_context *context,
                                           struct s3_error *error) {
    struct s3_response *r = &context->response;
    if (context->response_validated) return S3_RESULT_OK;
    if (r->invalid_headers)
        return s3_error_set(error, S3_RESULT_PROTOCOL_ERROR,
                            "invalid or oversized GET response headers");
    if (!r->headers_done)
        return s3_error_set(error, S3_RESULT_PROTOCOL_ERROR,
                            "response body arrived before complete headers");
    if (!context->original_known) {
        if (r->status != 200 || !r->have_length ||
            r->properties.etag[0] == '\0')
            return s3_error_set(error, S3_RESULT_PROTOCOL_ERROR,
                                "GET response lacks Content-Length or ETag");
        preserve_original_properties(context);
        context->original_properties.size = r->content_length;
        context->original_known = true;
    }
    else if (context->delivered != 0) {
        uint64_t remaining =
            context->original_properties.size - context->delivered;
        bool same_metadata =
            metadata_equal(&r->properties, &context->original_properties);
        if (r->status != 206 || !r->have_content_range || !r->have_length ||
            r->range_first != context->delivered ||
            r->range_total != context->original_properties.size ||
            r->range_last != context->original_properties.size - 1 ||
            r->range_last - r->range_first + 1 != r->content_length ||
            r->content_length != remaining ||
            strcmp(r->properties.etag, context->original_properties.etag) !=
                0 ||
            r->properties.last_modified !=
                context->original_properties.last_modified ||
            !property_equal(r->properties.content_type,
                            context->original_properties.content_type) ||
            !property_equal(r->properties.content_encoding,
                            context->original_properties.content_encoding) ||
            !property_equal(r->properties.cache_control,
                            context->original_properties.cache_control) ||
            !property_equal(r->properties.content_disposition,
                            context->original_properties.content_disposition) ||
            !property_equal(r->properties.content_language,
                            context->original_properties.content_language) ||
            !property_equal(r->properties.expires,
                            context->original_properties.expires) ||
            !same_metadata) {
            if (error != NULL) {
                error->result = S3_RESULT_PROTOCOL_ERROR;
                (void) snprintf(
                    error->message, sizeof(error->message),
                    "invalid resumed GET: status=%ld range=%d %llu-%llu/%llu "
                    "length=%llu offset=%llu total=%llu remaining=%llu "
                    "etag-match=%d metadata-match=%d",
                    r->status, r->have_content_range,
                    (unsigned long long) r->range_first,
                    (unsigned long long) r->range_last,
                    (unsigned long long) r->range_total,
                    (unsigned long long) r->content_length,
                    (unsigned long long) context->delivered,
                    (unsigned long long) context->original_properties.size,
                    (unsigned long long) remaining,
                    strcmp(r->properties.etag,
                           context->original_properties.etag) == 0,
                    same_metadata);
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
    struct s3_error ignored = {0};
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

enum s3_result
s3_object_get_conditional(struct s3_client *client, struct s3_error *error,
                          s3_properties_callback properties_callback,
                          s3_write_callback write_callback, void *data,
                          const char *bucket, const char *key,
                          const char *if_none_match) {
    struct get_context context = {.properties_callback = properties_callback,
                                  .write_callback = write_callback,
                                  .callback_data = data};
    char *url = NULL;
    enum s3_result result;
    s3_error_clear(error);
    if (client == NULL || error == NULL || write_callback == NULL)
        return s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                            "invalid GET arguments");
    result = s3_url_build(client, bucket, key, &url, error);
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
            if (!s3_headers_add(&headers, "Range", range) ||
                !s3_headers_add(&headers, "If-Match",
                                context.original_properties.etag)) {
                curl_slist_free_all(headers);
                free(url);
                cleanup_get_context(&context);
                return s3_error_set(error, S3_RESULT_ERROR, "out of memory");
            }
        }
        else if (if_none_match != NULL &&
                 !s3_headers_add(&headers, "If-None-Match", if_none_match)) {
            curl_slist_free_all(headers);
            free(url);
            cleanup_get_context(&context);
            return s3_error_set(error, S3_RESULT_ERROR, "out of memory");
        }
        result = s3_request_prepare(client, url, &headers, error);
        if (result != S3_RESULT_OK) {
            curl_slist_free_all(headers);
            free(url);
            cleanup_get_context(&context);
            return result;
        }
        (void) curl_easy_setopt(client->curl, CURLOPT_HTTPGET, 1L);
        (void) curl_easy_setopt(client->curl, CURLOPT_HEADERFUNCTION,
                                s3_headers_callback);
        (void) curl_easy_setopt(client->curl, CURLOPT_HEADERDATA,
                                &context.response);
        (void) curl_easy_setopt(client->curl, CURLOPT_WRITEFUNCTION,
                                collect_body);
        (void) curl_easy_setopt(client->curl, CURLOPT_WRITEDATA, &context);
        (void) curl_easy_setopt(client->curl, CURLOPT_ERRORBUFFER, curl_error);
        s3_trace_perform_start(client, attempt, client->max_attempts);
        code = curl_easy_perform(client->curl);
        (void) curl_easy_getinfo(client->curl, CURLINFO_RESPONSE_CODE,
                                 &context.response.status);
        curl_slist_free_all(headers);
        s3_error_clear(error);
        error->attempts = attempt;
        error->http_status = context.response.status;
        error->callback_errno = context.callback_errno;
        s3_error_parse_xml(context.response.error_body,
                           context.response.error_body_size, error);
        s3_trace_perform_end(client, attempt, client->max_attempts, code,
                             &context.response, error);

        if (context.response.invalid_headers) {
            free(url);
            cleanup_get_context(&context);
            return s3_error_set(error, S3_RESULT_PROTOCOL_ERROR,
                                "invalid or oversized GET response headers");
        }
        if (context.callback_failed) {
            free(url);
            cleanup_get_context(&context);
            return s3_error_set(error, S3_RESULT_CALLBACK_ERROR,
                                "output callback failed");
        }
        if (context.protocol_failed) {
            free(url);
            cleanup_get_context(&context);
            return s3_error_set(error, S3_RESULT_PROTOCOL_ERROR,
                                context.protocol_error[0] != '\0'
                                    ? context.protocol_error
                                    : "invalid GET response headers or length");
        }
        if (code == CURLE_OK && context.response.status == 304 &&
            if_none_match != NULL && !context.original_known &&
            context.response.headers_done &&
            !context.response.invalid_headers &&
            context.response.error_body_size == 0) {
            free(url);
            cleanup_get_context(&context);
            return s3_error_set(error, S3_RESULT_NOT_MODIFIED, NULL);
        }
        if (code == CURLE_OK && context.response.status >= 200 &&
            context.response.status < 300) {
            result = validate_get_headers(&context, error);
            if (result == S3_RESULT_OK &&
                context.delivered != context.original_properties.size)
                result = s3_error_set(error, S3_RESULT_PROTOCOL_ERROR,
                                      "GET response has an incorrect length");
            free(url);
            cleanup_get_context(&context);
            return result;
        }
        if (context.response.status == 412) {
            free(url);
            cleanup_get_context(&context);
            return s3_error_set(error, S3_RESULT_PRECONDITION_FAILED,
                                "object changed during download");
        }
        retryable =
            s3_retry_allowed(code, context.response.status, error->s3_code);
        if (!retryable) {
            result =
                s3_result_from_response(code, &context.response, false, error);
            if (code != CURLE_OK && curl_error[0] != '\0')
                (void) snprintf(error->message, sizeof(error->message), "%s",
                                curl_error);
            free(url);
            cleanup_get_context(&context);
            return result;
        }
        if (attempt == client->max_attempts) {
            free(url);
            cleanup_get_context(&context);
            return s3_error_set(error, S3_RESULT_RETRY_EXHAUSTED,
                                "S3 GET retry limit exhausted");
        }
        s3_retry_delay(client, attempt, client->max_attempts, code,
                       &context.response, error);
    }
    free(url);
    cleanup_get_context(&context);
    return s3_error_set(error, S3_RESULT_ERROR, "unreachable GET state");
}

enum s3_result s3_object_get(struct s3_client *client, struct s3_error *error,
                             s3_properties_callback properties_callback,
                             s3_write_callback write_callback, void *data,
                             const char *bucket, const char *key) {
    return s3_object_get_conditional(client, error, properties_callback,
                                     write_callback, data, bucket, key, NULL);
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
    s3_error_clear(error);
    if (properties != NULL) memset(properties, 0, sizeof(*properties));
    if (client == NULL || error == NULL || properties == NULL)
        return s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                            "invalid HEAD arguments");
    result = s3_url_build(client, bucket, key, &url, error);
    if (result != S3_RESULT_OK) return result;
    for (unsigned attempt = 1; attempt <= client->max_attempts; ++attempt) {
        struct curl_slist *headers = NULL;
        CURLcode code;
        char curl_error[CURL_ERROR_SIZE] = {0};
        if (attempt > 1) s3_response_cleanup(&response);
        s3_response_reset(&response);
        result = s3_request_prepare(client, url, &headers, error);
        if (result != S3_RESULT_OK) {
            curl_slist_free_all(headers);
            free(url);
            return result;
        }
        (void) curl_easy_setopt(client->curl, CURLOPT_NOBODY, 1L);
        (void) curl_easy_setopt(client->curl, CURLOPT_HEADERFUNCTION,
                                s3_headers_callback);
        (void) curl_easy_setopt(client->curl, CURLOPT_HEADERDATA, &response);
        (void) curl_easy_setopt(client->curl, CURLOPT_WRITEFUNCTION,
                                discard_body);
        (void) curl_easy_setopt(client->curl, CURLOPT_WRITEDATA, &response);
        (void) curl_easy_setopt(client->curl, CURLOPT_ERRORBUFFER, curl_error);
        s3_trace_perform_start(client, attempt, client->max_attempts);
        code = curl_easy_perform(client->curl);
        (void) curl_easy_getinfo(client->curl, CURLINFO_RESPONSE_CODE,
                                 &response.status);
        curl_slist_free_all(headers);
        s3_error_clear(error);
        error->attempts = attempt;
        error->http_status = response.status;
        s3_error_parse_xml(response.error_body, response.error_body_size,
                           error);
        s3_trace_perform_end(client, attempt, client->max_attempts, code,
                             &response, error);
        if (response.invalid_headers) {
            free(url);
            s3_response_cleanup(&response);
            return s3_error_set(error, S3_RESULT_PROTOCOL_ERROR,
                                "invalid or oversized HEAD response headers");
        }
        if (code == CURLE_OK && response.status >= 200 &&
            response.status < 300) {
            if (!response.have_length) {
                free(url);
                s3_response_cleanup(&response);
                return s3_error_set(error, S3_RESULT_PROTOCOL_ERROR,
                                    "HEAD response lacks Content-Length");
            }
            *properties = response.properties;
            response.properties.content_type = NULL;
            response.properties.content_encoding = NULL;
            response.properties.cache_control = NULL;
            response.properties.content_disposition = NULL;
            response.properties.content_language = NULL;
            response.properties.expires = NULL;
            response.properties.metadata = NULL;
            response.properties.metadata_count = 0;
            response.metadata = NULL;
            response.metadata_count = 0;
            response.metadata_capacity = 0;
            free(url);
            return S3_RESULT_OK;
        }
        if (!s3_retry_allowed(code, response.status, error->s3_code)) {
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
        s3_retry_delay(client, attempt, client->max_attempts, code, &response,
                       error);
    }
    free(url);
    s3_response_cleanup(&response);
    return s3_error_set(error, S3_RESULT_ERROR, "unreachable HEAD state");
}
