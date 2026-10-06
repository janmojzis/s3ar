/* SPDX-License-Identifier: MIT-0 */
#include "s3_internal.h"

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    struct s3_client *client;
    struct s3_memory_response *response;
    bool cancelled;
    bool during_transfer;
    unsigned abort_status;
    unsigned reads;
    unsigned requests;
    unsigned aborts;
    bool input_test;
    bool input_stream;
    const char *input_failure;
} scenario;

static bool testing_size_limit;
static size_t requested_allocation;
void *__real_malloc(size_t size);

void *__wrap_malloc(size_t size) {
    if (!testing_size_limit) return __real_malloc(size);
    requested_allocation = size;
    return NULL;
}

void __real_s3_response_memory_reset(struct s3_memory_response *response,
                                     size_t limit);

void __wrap_s3_response_memory_reset(struct s3_memory_response *response,
                                     size_t limit) {
    __real_s3_response_memory_reset(response, limit);
    scenario.response = response;
}

CURLcode __wrap_curl_easy_getinfo(CURL *curl, CURLINFO info, ...) {
    (void) curl;
    assert(info == CURLINFO_RESPONSE_CODE);
    va_list arguments;
    va_start(arguments, info);
    long *status = va_arg(arguments, long *);
    *status = scenario.response->response.status;
    va_end(arguments);
    return CURLE_OK;
}

CURLcode __wrap_curl_easy_perform(CURL *curl) {
    char *url = NULL;
    assert(curl == scenario.client->curl);
    assert(curl_url_get(scenario.client->url, CURLUPART_URL, &url, 0) ==
           CURLUE_OK);
    assert(url != NULL);
    struct s3_memory_response *response = scenario.response;
    ++scenario.requests;
    if (scenario.input_test) {
        const char *body = NULL;
        response->response.status = 200;
        if (!scenario.input_stream) {
            assert(scenario.requests == 1);
            assert(strcmp(url, "http://example.test/bucket/key") == 0);
        }
        else if (scenario.requests == 1) {
            assert(strstr(url, "?uploads") != NULL);
            body = "<InitiateMultipartUploadResult><UploadId>upload"
                   "</UploadId></InitiateMultipartUploadResult>";
        }
        else if (scenario.input_failure != NULL) {
            assert(scenario.requests == 2);
            assert(strstr(url, "?uploadId=upload") != NULL);
            response->response.status = 204;
            ++scenario.aborts;
        }
        else if (scenario.requests == 2) {
            assert(strstr(url, "?partNumber=1&uploadId=upload") != NULL);
            strcpy(response->response.properties.etag, "\"part\"");
        }
        else {
            assert(scenario.requests == 3);
            assert(strstr(url, "?uploadId=upload") != NULL);
            body = "<CompleteMultipartUploadResult><ETag>\"object\"</ETag>"
                   "</CompleteMultipartUploadResult>";
        }
        if (body != NULL)
            assert(s3_response_memory_collect((char *) body, 1, strlen(body),
                                              response) == strlen(body));
        curl_free(url);
        return CURLE_OK;
    }
    if (scenario.requests == 1) {
        assert(strcmp(url, "http://example.test/bucket/key?uploads") == 0);
        const char *body = "<InitiateMultipartUploadResult><UploadId>upload"
                           "</UploadId></InitiateMultipartUploadResult>";
        response->response.status = 200;
        assert(s3_response_memory_collect((char *) body, 1, strlen(body),
                                          response) == strlen(body));
    }
    else if (scenario.requests == 2) {
        assert(strcmp(url, "http://example.test/bucket/key?partNumber=1&"
                           "uploadId=upload") == 0);
        if (scenario.during_transfer) {
            scenario.cancelled = true;
            assert(
                scenario.client->cancel_callback(scenario.client->cancel_data));
            curl_free(url);
            return CURLE_ABORTED_BY_CALLBACK;
        }
        response->response.status = 200;
        strcpy(response->response.properties.etag, "\"part\"");
    }
    else {
        assert(scenario.requests == 3);
        assert(strcmp(url, "http://example.test/bucket/key?uploadId=upload") ==
               0);
        assert(scenario.cancelled);
        assert(scenario.client->cancel_callback == NULL);
        ++scenario.aborts;
        response->response.status = scenario.abort_status;
        if (scenario.abort_status == 403) {
            const char *body = "<Error><Code>AccessDenied</Code>"
                               "<Message>Abort denied.</Message></Error>";
            assert(s3_response_memory_collect((char *) body, 1, strlen(body),
                                              response) == strlen(body));
        }
    }
    curl_free(url);
    return CURLE_OK;
}

static bool cancelled(void *data) { return *(bool *) data; }

static enum s3_read_result read_input(void *data, unsigned char *buffer,
                                      size_t capacity, size_t *size) {
    (void) data;
    if (scenario.reads++ == 0) {
        memset(buffer, 'x', capacity);
        *size = capacity;
        return S3_READ_DATA;
    }
    scenario.cancelled = true;
    *size = 0;
    errno = EINTR;
    return S3_READ_ERROR;
}

static void test_cancelled_put(bool stream, bool during_transfer,
                               unsigned abort_status) {
    memset(&scenario, 0, sizeof(scenario));
    scenario.during_transfer = during_transfer;
    scenario.abort_status = abort_status;
    struct s3_client_config config;
    struct s3_error error = {0};
    s3_config_init(&config);
    config.endpoint = "http://example.test";
    config.region = "us-east-1";
    config.access_key = "test";
    config.secret_key = "test";
    config.max_attempts = 1;
    assert(s3_client_open(&scenario.client, &error, &config) == S3_RESULT_OK);
    s3_client_set_cancel_callback(scenario.client, cancelled,
                                  &scenario.cancelled);
    enum s3_result result =
        stream
            ? s3_object_put_stream(scenario.client, &error, "bucket", "key",
                                   S3_MULTIPART_PART_SIZE, NULL, read_input,
                                   NULL)
            : s3_object_put(scenario.client, &error, "bucket", "key",
                            2 * S3_MULTIPART_PART_SIZE, NULL, read_input, NULL);
    assert(result ==
           (during_transfer ? S3_RESULT_ERROR : S3_RESULT_CALLBACK_ERROR));
    assert(error.result == result);
    if (!during_transfer) assert(error.callback_errno == EINTR);
    assert(scenario.aborts == 1 && scenario.requests == 3);
    assert(error.abort_result ==
           (abort_status == 403 ? S3_RESULT_ACCESS_DENIED : S3_RESULT_OK));
    assert((strstr(error.message, "multipart abort failed") != NULL) ==
           (abort_status == 403));
    if (abort_status == 403)
        assert(strstr(error.message, "Abort denied.") != NULL);
    else if (!during_transfer)
        assert(strcmp(error.message, "input callback failed") == 0);
    assert(scenario.client->cancel_callback == cancelled);
    assert(scenario.client->cancel_data == &scenario.cancelled);
    assert(s3_bucket_head(scenario.client, &error, "bucket") ==
           S3_RESULT_ERROR);
    assert(strcmp(error.message, "interrupted") == 0);
    assert(error.abort_result == S3_RESULT_OK);
    assert(scenario.requests == 3);
    s3_client_close(scenario.client);
}

static enum s3_read_result unexpected_read(void *data, unsigned char *buffer,
                                           size_t capacity, size_t *size) {
    (void) data;
    (void) buffer;
    (void) capacity;
    (void) size;
    assert(false);
    return S3_READ_ERROR;
}

static void test_put_size_limit(void) {
    const uint64_t maximum_part = S3_MULTIPART_MAX_PART_SIZE;
    const uint64_t maximum_object = maximum_part * 10000;
    const uint64_t sizes[] = {maximum_object - 1, maximum_object,
                              maximum_object + 1, UINT64_MAX};
    struct s3_client client = {0};
    testing_size_limit = true;
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        requested_allocation = 0;
        struct s3_error error = {0};
        enum s3_result result =
            s3_object_put(&client, &error, "bucket", "key", sizes[i], NULL,
                          unexpected_read, NULL);
        if (sizes[i] <= maximum_object && SIZE_MAX >= maximum_part) {
            /* The boundary is accepted, but the test never allocates 5 GiB. */
            assert(result == S3_RESULT_ERROR);
            assert(strcmp(error.message, "out of memory") == 0);
            assert(requested_allocation == maximum_part);
        }
        else {
            assert(result == S3_RESULT_CONFIGURATION_ERROR);
            assert(strcmp(error.message, "object is too large") == 0);
            assert(requested_allocation == 0);
        }
        assert(error.result == result);
    }
    testing_size_limit = false;
}

static void test_put_part_size_rounding(void) {
    const uint64_t mib = 1024 * 1024;
    const struct {
        uint64_t size;
        uint64_t part_size;
    } cases[] = {
        {0, 1},
        {1, 1},
        {S3_MULTIPART_PART_SIZE - 1, S3_MULTIPART_PART_SIZE - 1},
        {S3_MULTIPART_PART_SIZE, S3_MULTIPART_PART_SIZE},
        {16 * mib * 10000 - 1, 16 * mib},
        {16 * mib * 10000, 16 * mib},
        {16 * mib * 10000 + 1, 17 * mib},
        {17 * mib * 10000, 17 * mib},
        {17 * mib * 10000 + 1, 18 * mib},
        {4095 * mib * 10000, 4095 * mib},
        {4095 * mib * 10000 + 1, 4096 * mib},
    };
    struct s3_client client = {0};
    testing_size_limit = true;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        requested_allocation = 0;
        struct s3_error error = {0};
        enum s3_result result =
            s3_object_put(&client, &error, "bucket", "key", cases[i].size, NULL,
                          unexpected_read, NULL);
        if (cases[i].part_size <= SIZE_MAX) {
            assert(result == S3_RESULT_ERROR);
            assert(strcmp(error.message, "out of memory") == 0);
            assert(requested_allocation == cases[i].part_size);
        }
        else {
            assert(result == S3_RESULT_CONFIGURATION_ERROR);
            assert(requested_allocation == 0);
        }
        assert(error.result == result);
    }
    testing_size_limit = false;
}

enum input_case {
    INPUT_SHORT_READS,
    INPUT_EARLY_EOF,
    INPUT_EOF_WITH_DATA,
    INPUT_EMPTY_DATA,
    INPUT_OVER_CAPACITY,
    INPUT_ERROR,
    INPUT_ERROR_WITHOUT_ERRNO,
    INPUT_EXTRA_DATA,
};

static enum s3_read_result read_test_input(void *data, unsigned char *buffer,
                                           size_t capacity, size_t *size) {
    enum input_case input = *(enum input_case *) data;
    ++scenario.reads;
    *size = 0;
    switch (input) {
        case INPUT_EARLY_EOF:
            return S3_READ_EOF;
        case INPUT_EOF_WITH_DATA:
            buffer[0] = 'x';
            *size = 1;
            return S3_READ_EOF;
        case INPUT_EMPTY_DATA:
            return S3_READ_DATA;
        case INPUT_OVER_CAPACITY:
            *size = capacity + 1;
            return S3_READ_DATA;
        case INPUT_ERROR:
        case INPUT_ERROR_WITHOUT_ERRNO:
            errno = input == INPUT_ERROR ? EPIPE : 0;
            return S3_READ_ERROR;
        default:
            if (scenario.reads > (input == INPUT_EXTRA_DATA ? 5u : 4u))
                return S3_READ_EOF;
            buffer[0] = 'x';
            *size = 1;
            return S3_READ_DATA;
    }
}

static void test_input_contract(bool stream, enum input_case input) {
    memset(&scenario, 0, sizeof(scenario));
    scenario.input_test = true;
    scenario.input_stream = stream;
    if (!stream && (input == INPUT_EARLY_EOF || input == INPUT_EOF_WITH_DATA))
        scenario.input_failure = "input ended before declared object size";
    else if (!stream && input == INPUT_EXTRA_DATA)
        scenario.input_failure = "input exceeds declared object size";
    else if (input == INPUT_EMPTY_DATA || input == INPUT_OVER_CAPACITY)
        scenario.input_failure = "input callback returned invalid data";
    else if (input == INPUT_ERROR || input == INPUT_ERROR_WITHOUT_ERRNO)
        scenario.input_failure = "input callback failed";
    struct s3_client_config config;
    struct s3_error error = {0};
    s3_config_init(&config);
    config.endpoint = "http://example.test";
    config.region = "us-east-1";
    config.access_key = "test";
    config.secret_key = "test";
    config.max_attempts = 1;
    assert(s3_client_open(&scenario.client, &error, &config) == S3_RESULT_OK);
    enum s3_result result =
        stream ? s3_object_put_stream(scenario.client, &error, "bucket", "key",
                                      S3_MULTIPART_PART_SIZE, NULL,
                                      read_test_input, &input)
               : s3_object_put(scenario.client, &error, "bucket", "key", 4,
                               NULL, read_test_input, &input);
    assert(result == (scenario.input_failure != NULL ? S3_RESULT_CALLBACK_ERROR
                                                     : S3_RESULT_OK));
    assert(error.result == result);
    if (scenario.input_failure != NULL) {
        assert(strcmp(error.message, scenario.input_failure) == 0);
        assert(scenario.requests == (stream ? 2u : 0u));
        assert(scenario.aborts == (stream ? 1u : 0u));
    }
    else {
        assert(scenario.requests == (stream ? 3u : 1u));
        assert(scenario.aborts == 0);
    }
    assert(error.callback_errno == (input == INPUT_ERROR                 ? EPIPE
                                    : input == INPUT_ERROR_WITHOUT_ERRNO ? EIO
                                                                         : 0));
    assert(error.abort_result == S3_RESULT_OK);
    s3_client_close(scenario.client);
}

int main(void) {
    test_put_size_limit();
    test_put_part_size_rounding();
    for (unsigned stream = 0; stream < 2; ++stream)
        for (enum input_case input = INPUT_SHORT_READS;
             input <= INPUT_EXTRA_DATA; ++input)
            test_input_contract(stream != 0, input);
    const unsigned abort_statuses[] = {204, 404, 403};
    for (unsigned stream = 0; stream < 2; ++stream)
        for (unsigned transfer = 0; transfer < 2; ++transfer)
            for (size_t i = 0;
                 i < sizeof(abort_statuses) / sizeof(abort_statuses[0]); ++i)
                test_cancelled_put(stream != 0, transfer != 0,
                                   abort_statuses[i]);
    return 0;
}
