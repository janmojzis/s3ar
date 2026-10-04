/* SPDX-License-Identifier: MIT-0 */
#include "s3_internal.h"

#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static struct s3_memory_response *response;
static unsigned requests;
static bool allocation_failure;
static bool fail_next_malloc;
static bool not_modified;
static struct s3_response *get_response;

void __real_s3_response_reset(struct s3_response *context);
void __wrap_s3_response_reset(struct s3_response *context) {
    __real_s3_response_reset(context);
    if (not_modified) get_response = context;
}

void *__real_malloc(size_t size);
void *__wrap_malloc(size_t size) {
    if (fail_next_malloc) {
        fail_next_malloc = false;
        return NULL;
    }
    return __real_malloc(size);
}

void __real_s3_response_memory_reset(struct s3_memory_response *context,
                                     size_t limit);
void __wrap_s3_response_memory_reset(struct s3_memory_response *context,
                                     size_t limit) {
    __real_s3_response_memory_reset(context, limit);
    response = context;
}

CURLcode __wrap_curl_easy_getinfo(CURL *curl, CURLINFO info, ...) {
    (void) curl;
    assert(info == CURLINFO_RESPONSE_CODE);
    va_list arguments;
    va_start(arguments, info);
    *va_arg(arguments, long *) =
        not_modified ? get_response->status : response->response.status;
    va_end(arguments);
    return CURLE_OK;
}

CURLcode __wrap_curl_easy_perform(CURL *curl) {
    (void) curl;
    ++requests;
    if (not_modified) {
        char status[] = "HTTP/1.1 304 Not Modified\r\n";
        char end[] = "\r\n";
        assert(s3_headers_callback(status, 1, strlen(status), get_response) ==
               strlen(status));
        assert(s3_headers_callback(end, 1, strlen(end), get_response) ==
               strlen(end));
        return CURLE_OK;
    }
    char header[320];
    if (allocation_failure) {
        strcpy(header, "Content-Type: application/xml\r\n");
        fail_next_malloc = true;
    }
    else {
        strcpy(header, "ETag: ");
        memset(header + 6, 'x', 300);
        strcpy(header + 306, "\r\n");
    }
    size_t size = strlen(header);
    assert(s3_headers_callback(header, 1, size, &response->response) == size);
    assert(!fail_next_malloc);
    assert(response->response.invalid_headers);
    response->response.status = 200;
    return CURLE_OK;
}

static enum s3_read_result read_empty(void *data, unsigned char *buffer,
                                      size_t capacity, size_t *size) {
    (void) data;
    (void) buffer;
    (void) capacity;
    *size = 0;
    return S3_READ_EOF;
}

static bool
unexpected_properties(void *data,
                      const struct s3_object_properties *properties) {
    (void) data;
    (void) properties;
    assert(false);
    return false;
}

static bool unexpected_write(void *data, const unsigned char *buffer,
                             size_t size) {
    (void) data;
    (void) buffer;
    (void) size;
    assert(false);
    return false;
}

int main(void) {
    struct s3_client_config config;
    struct s3_client *client = NULL;
    struct s3_error error = {0};
    s3_config_init(&config);
    config.endpoint = "http://example.test";
    config.region = "us-east-1";
    config.access_key = "test";
    config.secret_key = "test";
    config.max_attempts = 3;
    assert(s3_client_open(&client, &error, &config) == S3_RESULT_OK);
    for (unsigned failure = 0; failure < 2; ++failure) {
        allocation_failure = failure != 0;
        requests = 0;
        enum s3_result result = s3_bucket_head(client, &error, "bucket");
        assert(result == S3_RESULT_PROTOCOL_ERROR);
        assert(error.result == result);
        assert(error.http_status == 200);
        assert(error.attempts == 1 && requests == 1);
        assert(strcmp(error.message,
                      "invalid or oversized S3 response headers") == 0);

        char *body = (char *) "unchanged";
        size_t size = 123;
        requests = 0;
        result = s3_request_url(client, &error, "http://example.test/bucket",
                                "GET", NULL, &body, &size);
        assert(result == S3_RESULT_PROTOCOL_ERROR);
        assert(error.result == result);
        assert(error.http_status == 200);
        assert(error.attempts == 1 && requests == 1);
        assert(body == NULL && size == 0);

        requests = 0;
        result = s3_object_put(client, &error, "bucket", "key", 0, NULL,
                               read_empty, NULL);
        assert(result == S3_RESULT_PROTOCOL_ERROR);
        assert(error.result == result);
        assert(error.http_status == 200);
        assert(error.attempts == 1 && requests == 1);
        assert(strcmp(error.message,
                      "invalid or oversized S3 upload response headers") == 0);
    }
    not_modified = true;
    requests = 0;
    error.result = S3_RESULT_ERROR;
    strcpy(error.message, "stale error");
    enum s3_result result = s3_object_get_conditional(
        client, &error, unexpected_properties, unexpected_write, NULL, "bucket",
        "key", "\"cached\"");
    assert(result == S3_RESULT_NOT_MODIFIED);
    assert(error.result == result);
    assert(error.http_status == 304);
    assert(error.attempts == 1 && requests == 1);
    assert(error.message[0] == '\0');
    s3_client_close(client);
    return 0;
}
