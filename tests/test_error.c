/* SPDX-License-Identifier: MIT-0 */
#include "s3_internal.h"
#include "s3_xml.h"
#include "s3_log.h"
#include "s3ar_config.h"
#include "s3ar_parse.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void poison(struct s3_error *error) {
    memset(error, 0xa5, sizeof(*error));
}

static void assert_configuration_error(enum s3_result result,
                                       const struct s3_error *error) {
    assert(result == S3_RESULT_CONFIGURATION_ERROR);
    assert(error->result == S3_RESULT_CONFIGURATION_ERROR);
    assert(error->http_status == 0);
    assert(error->attempts == 0);
    assert(error->callback_errno == 0);
    assert(error->s3_code[0] == '\0');
    assert(error->request_id[0] == '\0');
    assert(error->message[0] != '\0');
    assert(memchr(error->message, '\0', sizeof(error->message)) != NULL);
}

static void test_public_api_clears_error(void) {
    struct s3_error error;
    struct s3_client *client = NULL;
    struct s3_object_properties properties;
    struct s3_listing_page page = {0};
    struct s3_uri uri;
    enum s3_result result;

#define CHECK(call)                                                            \
    do {                                                                       \
        poison(&error);                                                        \
        result = (call);                                                       \
        assert_configuration_error(result, &error);                            \
    } while (0)

    CHECK(s3ar_config_from_env(NULL, &error));
    CHECK(s3_uri_parse_alloc(NULL, &uri, &error));
    CHECK(s3_client_open(&client, &error, NULL));
    CHECK(s3_url_validate_object_name(NULL, "bucket", "key", &error));
    CHECK(s3_bucket_list(NULL, &error, NULL, NULL));
    CHECK(s3_bucket_head(NULL, &error, NULL));
    CHECK(s3_bucket_create(NULL, &error, NULL));
    CHECK(s3_bucket_ensure(NULL, &error, NULL));
    CHECK(s3_bucket_delete(NULL, &error, NULL));
    CHECK(s3_bucket_acl(NULL, &error, NULL, NULL, NULL));
    CHECK(s3_object_list(NULL, &error, NULL, NULL, NULL, NULL, NULL));
    CHECK(
        s3_listing_versions_page(NULL, &error, NULL, NULL, NULL, NULL, &page));
    CHECK(s3_listing_uploads_page(NULL, &error, NULL, NULL, NULL, NULL, &page));
    CHECK(s3_object_delete_version(NULL, &error, NULL, NULL, NULL));
    CHECK(s3_object_delete_batch(NULL, &error, NULL, NULL, 0, NULL));
    CHECK(s3_multipart_abort(NULL, &error, NULL, NULL, NULL));
    CHECK(s3_object_head(NULL, &error, &properties, NULL, NULL));
    CHECK(s3_object_get(NULL, &error, NULL, NULL, NULL, NULL, NULL));
    CHECK(s3_object_put(NULL, &error, NULL, NULL, 0, NULL, NULL, NULL));
    CHECK(s3_object_put_stream(NULL, &error, NULL, NULL, S3_MULTIPART_PART_SIZE,
                               NULL, NULL, NULL));

#undef CHECK
}

static void test_uri_variants(void) {
    struct s3_uri owned = {0};
    struct s3_uri_buffer buffer = {0};
    struct s3_error error = {0};
    char long_key[1050];
    assert(s3_uri_parse_alloc("s3://bucket/a/b", &owned, &error) ==
           S3_RESULT_OK);
    assert(strcmp(owned.bucket, "bucket") == 0);
    assert(strcmp(owned.key, "a/b") == 0);
    s3_uri_free(&owned);
    assert(s3_uri_parse_into("s3://bucket/a/b", &buffer, &error) ==
           S3_RESULT_OK);
    assert(strcmp(buffer.bucket, "bucket") == 0);
    assert(strcmp(buffer.key, "a/b") == 0);
    memset(long_key, 'x', sizeof(long_key));
    memcpy(long_key, "s3://bucket/", 12);
    long_key[sizeof(long_key) - 1] = '\0';
    assert(s3_uri_parse_into(long_key, &buffer, &error) ==
           S3_RESULT_CONFIGURATION_ERROR);
    assert(buffer.bucket[0] == '\0' && buffer.key[0] == '\0');
    assert(s3_uri_parse_alloc(long_key, &owned, &error) == S3_RESULT_OK);
    s3_uri_free(&owned);
}

static void test_multiple_clients(void) {
    struct s3_client_config config;
    struct s3_client *first = NULL, *second = NULL;
    struct s3_error error = {0};
    CURL *other_curl;
    assert(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    other_curl = curl_easy_init();
    assert(other_curl != NULL);
    s3_config_init(&config);
    config.endpoint = "http://127.0.0.1:1";
    config.region = "us-east-1";
    config.access_key = "test-access";
    config.secret_key = "test-secret";
    assert(s3_client_open(&first, &error, &config) == S3_RESULT_OK);
    assert(s3_client_open(&second, &error, &config) == S3_RESULT_OK);
    s3_client_close(first);
    assert(s3_url_validate_object_name(second, "bucket", "key", &error) ==
           S3_RESULT_OK);
    s3_client_close(second);
    assert(curl_easy_setopt(other_curl, CURLOPT_URL, "http://127.0.0.1:1") ==
           CURLE_OK);
    curl_easy_cleanup(other_curl);
    curl_global_cleanup();
}

static void test_error_set_preserves_response_details(void) {
    struct s3_error error = {
        .http_status = 503,
        .attempts = 4,
        .callback_errno = EIO,
    };

    (void) strcpy(error.s3_code, "SlowDown");
    (void) strcpy(error.request_id, "request-id");
    (void) strcpy(error.message, "old message");

    assert(s3_error_set(&error, S3_RESULT_RETRY_EXHAUSTED, "retry exhausted") ==
           S3_RESULT_RETRY_EXHAUSTED);
    assert(error.result == S3_RESULT_RETRY_EXHAUSTED);
    assert(error.http_status == 503);
    assert(error.attempts == 4);
    assert(error.callback_errno == EIO);
    assert(strcmp(error.s3_code, "SlowDown") == 0);
    assert(strcmp(error.request_id, "request-id") == 0);
    assert(strcmp(error.message, "retry exhausted") == 0);

    assert(s3_error_set(&error, S3_RESULT_ERROR, NULL) == S3_RESULT_ERROR);
    assert(error.message[0] == '\0');
    assert(error.http_status == 503);
    assert(error.attempts == 4);
    assert(error.callback_errno == EIO);
    assert(strcmp(error.s3_code, "SlowDown") == 0);
    assert(strcmp(error.request_id, "request-id") == 0);
}

static void test_error_xml_parser(void) {
    static const char valid[] =
        "<Error><Code>SlowDown</Code><Message>try later</Message>"
        "<RequestId>request-123</RequestId></Error>";
    static const char with_dtd[] =
        "<!DOCTYPE Error [<!ENTITY code 'SlowDown'>]>"
        "<Error><Code>&code;</Code></Error>";
    struct s3_error error = {0};

    s3_error_parse_xml(valid, sizeof(valid) - 1, &error);
    assert(strcmp(error.s3_code, "SlowDown") == 0);
    assert(strcmp(error.message, "try later") == 0);
    assert(strcmp(error.request_id, "request-123") == 0);

    memset(&error, 0, sizeof(error));
    s3_error_parse_xml(with_dtd, sizeof(with_dtd) - 1, &error);
    assert(error.s3_code[0] == '\0');
    assert(error.message[0] == '\0');
    assert(error.request_id[0] == '\0');
}

static void parse_header(struct s3_response *response, const char *text) {
    char buffer[256];
    size_t size = strlen(text);
    assert(size < sizeof(buffer));
    memcpy(buffer, text, size + 1);
    assert(s3_headers_callback(buffer, 1, size, response) == size);
}

static void test_response_take_properties(void) {
    struct s3_response response;
    struct s3_object_properties properties = {0};
    s3_response_reset(&response);
    parse_header(&response, "HTTP/1.1 200 OK\r\n");
    parse_header(&response, "Content-Length: 42\r\n");
    parse_header(&response, "ETag: \"owned\"\r\n");
    parse_header(&response, "Content-Type: text/plain\r\n");
    parse_header(&response, "Content-Encoding: identity\r\n");
    parse_header(&response, "Cache-Control: no-cache\r\n");
    parse_header(&response, "Content-Disposition: inline\r\n");
    parse_header(&response, "Content-Language: cs\r\n");
    parse_header(&response, "Expires: Thu, 01 Oct 2026 00:00:00 GMT\r\n");
    parse_header(&response, "x-amz-meta-origin: archive\r\n");
    parse_header(&response, "\r\n");
    assert(!response.invalid_headers);
    const char *content_type = response.properties.content_type;
    const struct s3_metadata *metadata = response.properties.metadata;
    s3_response_take_properties(&response, &properties);
    assert(properties.content_type == content_type);
    assert(properties.metadata == metadata);
    assert(response.metadata == NULL && response.metadata_count == 0 &&
           response.metadata_capacity == 0);
    assert(response.properties.content_type == NULL &&
           response.properties.metadata == NULL);
    assert(response.status == 200 && response.headers_done &&
           response.have_length && response.content_length == 42);
    s3_response_cleanup(&response);
    s3_response_reset(&response);
    parse_header(&response, "Content-Type: application/xml\r\n");
    parse_header(&response, "x-amz-meta-origin: replacement\r\n");
    s3_response_cleanup(&response);
    assert(properties.size == 42);
    assert(strcmp(properties.etag, "\"owned\"") == 0);
    assert(strcmp(properties.content_type, "text/plain") == 0);
    assert(strcmp(properties.content_encoding, "identity") == 0);
    assert(strcmp(properties.cache_control, "no-cache") == 0);
    assert(strcmp(properties.content_disposition, "inline") == 0);
    assert(strcmp(properties.content_language, "cs") == 0);
    assert(strcmp(properties.expires, "Thu, 01 Oct 2026 00:00:00 GMT") == 0);
    assert(properties.metadata_count == 1);
    assert(strcmp(properties.metadata[0].name, "origin") == 0);
    assert(strcmp(properties.metadata[0].value, "archive") == 0);
    s3_object_properties_free(&properties);
}

static void test_content_range_bounds(void) {
    static const struct {
        const char *header;
        uint64_t first, last, total;
    } valid[] = {
        {"Content-Range: bytes 0-0/1\r\n", 0, 0, 1},
        {"Content-Range: bytes 2-3/4\r\n", 2, 3, 4},
        {"Content-Range: bytes 2-3/*\r\n", 2, 3, UINT64_MAX},
        {"Content-Range: bytes 0-18446744073709551614/18446744073709551615\r\n",
         0, UINT64_MAX - 1, UINT64_MAX},
    };
    for (size_t i = 0; i < sizeof(valid) / sizeof(*valid); ++i) {
        struct s3_response response;
        s3_response_reset(&response);
        parse_header(&response, valid[i].header);
        assert(response.have_content_range && !response.invalid_headers);
        assert(response.range_first == valid[i].first);
        assert(response.range_last == valid[i].last);
        assert(response.range_total == valid[i].total);
        s3_response_cleanup(&response);
    }
    static const char *const invalid[] = {
        "Content-Range: bytes 3-2/4\r\n",
        "Content-Range: bytes 2-4/4\r\n",
        "Content-Range: bytes 2-999/4\r\n",
        "Content-Range: bytes 0-0/0\r\n",
        "Content-Range: bytes 3-2/*\r\n",
        "Content-Range: bytes 0-18446744073709551615/18446744073709551615\r\n",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        struct s3_response response;
        s3_response_reset(&response);
        parse_header(&response, invalid[i]);
        assert(!response.have_content_range && response.invalid_headers);
        s3_response_cleanup(&response);
    }
}

static void test_long_object_property_headers(void) {
    static const char *const names[] = {"Content-Type", "Content-Encoding",
                                        "Cache-Control"};
    struct s3_response response;
    char value[1801];
    char header[sizeof(value) + 32];

    memset(value, 'a', sizeof(value) - 1);
    value[sizeof(value) - 1] = '\0';
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        int length =
            snprintf(header, sizeof(header), "%s: %s\r\n", names[i], value);
        assert(length > 0 && (size_t) length < sizeof(header));
        s3_response_reset(&response);
        assert(s3_headers_callback(header, 1, (size_t) length, &response) ==
               (size_t) length);
        assert(!response.invalid_headers);
        if (i == 0)
            assert(strcmp(response.properties.content_type, value) == 0);
        else if (i == 1)
            assert(strcmp(response.properties.content_encoding, value) == 0);
        else
            assert(strcmp(response.properties.cache_control, value) == 0);
        s3_response_cleanup(&response);
    }
}

static void test_retry_response_headers(void) {
    struct s3_response response;
    s3_response_reset(&response);
    parse_header(&response, "HTTP/1.1 503 Slow Down\r\n");
    parse_header(&response, "Retry-After: 7\r\n");
    parse_header(&response, "x-amz-bucket-region: eu-central-1\r\n");
    assert(response.have_retry_after);
    assert(response.retry_after_ms == 7000);
    assert(strcmp(response.bucket_region, "eu-central-1") == 0);
    s3_response_cleanup(&response);
}

static void test_retry_after_header_variants(void) {
    static const char *const weekdays[] = {"Sun", "Mon", "Tue", "Wed",
                                           "Thu", "Fri", "Sat"};
    static const char *const months[] = {"Jan", "Feb", "Mar", "Apr",
                                         "May", "Jun", "Jul", "Aug",
                                         "Sep", "Oct", "Nov", "Dec"};
    struct s3_response response;
    char header[128];
    time_t deadline = time(NULL) + 120;
    struct tm utc;
    int length;

    s3_response_reset(&response);
    parse_header(&response, "Retry-After: invalid\r\n");
    assert(!response.have_retry_after);

    s3_response_reset(&response);
    parse_header(&response, "Retry-After: 999999\r\n");
    assert(response.have_retry_after);
    assert(response.retry_after_ms == 300000);

    assert(gmtime_r(&deadline, &utc) != NULL);
    length = snprintf(header, sizeof(header),
                      "Retry-After: %s, %02d %s %04d %02d:%02d:%02d GMT\r\n",
                      weekdays[utc.tm_wday], utc.tm_mday, months[utc.tm_mon],
                      utc.tm_year + 1900, utc.tm_hour, utc.tm_min, utc.tm_sec);
    assert(length > 0 && (size_t) length < sizeof(header));
    s3_response_reset(&response);
    parse_header(&response, header);
    assert(response.have_retry_after);
    assert(response.retry_after_ms >= 115000);
    assert(response.retry_after_ms <= 120000);
    s3_response_cleanup(&response);
}

static void test_status_header_does_not_require_nul_termination(void) {
    char buffer[] = {'H', 'T', 'T', 'P', '/', '1', '.',
                     '1', ' ', '2', '0', '0', '9'};
    struct s3_response response;
    s3_response_reset(&response);
    assert(s3_headers_callback(buffer, 1, sizeof(buffer) - 1, &response) ==
           sizeof(buffer) - 1);
    assert(response.status == 200);
    s3_response_cleanup(&response);
}

static void test_redirect_retry_and_diagnostic(void) {
    struct s3_response response = {.status = 301};
    struct s3_error error = {0};
    (void) snprintf(response.bucket_region, sizeof(response.bucket_region),
                    "%s", "eu-central-1");
    assert(!s3_retry_allowed(CURLE_OK, 301, NULL));
    assert(s3_retry_allowed(CURLE_OK, 307, NULL));
    assert(s3_result_from_response(CURLE_OK, &response, false, &error) ==
           S3_RESULT_ERROR);
    assert(strstr(error.message, "eu-central-1") != NULL);
    assert(strcmp(error.bucket_region, "eu-central-1") == 0);
}

static void test_object_name_validator_clears_error(void) {
    struct s3_client client = {.endpoint = "https://example.test"};
    struct s3_error error;
    for (unsigned style = 0; style < 2; ++style) {
        client.uri_style =
            style == 0 ? S3_URI_STYLE_PATH : S3_URI_STYLE_VIRTUAL;
        poison(&error);
        assert_configuration_error(
            s3_url_validate_object_name(&client, NULL, "key", &error), &error);
        poison(&error);
        assert_configuration_error(
            s3_url_validate_object_name(&client, "bucket", NULL, &error),
            &error);
        poison(&error);
        assert_configuration_error(
            s3_url_validate_object_name(&client, "bucket", "", &error), &error);
        /* A successful retry must remove every field from the previous error.
         */
        assert(s3_url_validate_object_name(&client, "bucket", "key", &error) ==
               S3_RESULT_OK);
        assert(error.result == S3_RESULT_OK);
        assert(error.http_status == 0 && error.attempts == 0 &&
               error.callback_errno == 0);
        assert(error.message[0] == '\0' && error.s3_code[0] == '\0' &&
               error.request_id[0] == '\0' && error.bucket_region[0] == '\0');
        assert(s3_url_validate_object_name(&client, "bucket", "key", NULL) ==
               S3_RESULT_OK);
    }
    assert(s3_url_validate_object_name(NULL, "bucket", "key", NULL) ==
           S3_RESULT_CONFIGURATION_ERROR);
}

static void test_object_name_validator_without_client(void) {
    struct s3_error error;
    poison(&error);
    assert(s3_url_validate_object_name_for_style(S3_URI_STYLE_PATH, false,
                                                 "Upper_under", "key",
                                                 &error) == S3_RESULT_OK);
    assert(error.result == S3_RESULT_OK && error.message[0] == '\0' &&
           error.http_status == 0 && error.attempts == 0);
    assert_configuration_error(
        s3_url_validate_object_name_for_style(S3_URI_STYLE_VIRTUAL, false,
                                              "Upper_under", "key", &error),
        &error);
    assert(s3_url_validate_object_name_for_style(S3_URI_STYLE_VIRTUAL, false,
                                                 "bucket.with-dot", "key",
                                                 NULL) == S3_RESULT_OK);
    assert_configuration_error(
        s3_url_validate_object_name_for_style(S3_URI_STYLE_VIRTUAL, true,
                                              "bucket.with-dot", "key", &error),
        &error);
    assert_configuration_error(
        s3_url_validate_object_name_for_style(S3_URI_STYLE_PATH, false,
                                              "bucket", NULL, &error),
        &error);
    assert_configuration_error(
        s3_url_validate_object_name_for_style(S3_URI_STYLE_PATH, false,
                                              "contains/slash", "key", &error),
        &error);
    struct s3_client client = {.uri_style = S3_URI_STYLE_PATH};
    assert(s3_url_validate_object_name(&client, "bucket", "key", &error) ==
           S3_RESULT_OK);
}

static void test_bucket_validation_depends_on_uri_style(void) {
    struct s3_client client = {
        .endpoint = "https://example.test",
        .uri_style = S3_URI_STYLE_PATH,
    };
    struct s3_error error = {0};
    char *url = NULL;
    char oversized[257];

    assert(s3_url_build(&client, "Upper_under", "key", &url, &error) ==
           S3_RESULT_OK);
    assert(strcmp(url, "https://example.test/Upper_under/key") == 0);
    free(url);

    assert(s3_url_build_bucket(&client, "ab", NULL, &url, &error) ==
           S3_RESULT_OK);
    assert(strcmp(url, "https://example.test/ab") == 0);
    free(url);

    assert(s3_url_build(&client, "contains/slash", "key", &url, &error) ==
           S3_RESULT_CONFIGURATION_ERROR);
    assert(s3_url_build(&client, "", "key", &url, &error) ==
           S3_RESULT_CONFIGURATION_ERROR);
    memset(oversized, 'a', sizeof(oversized) - 1);
    oversized[sizeof(oversized) - 1] = '\0';
    assert(s3_url_build(&client, oversized, "key", &url, &error) ==
           S3_RESULT_CONFIGURATION_ERROR);

    client.uri_style = S3_URI_STYLE_VIRTUAL;
    assert(s3_url_build(&client, "Upper_under", "key", &url, &error) ==
           S3_RESULT_CONFIGURATION_ERROR);
    assert(s3_url_build(&client, "bucket.with-dot", "key", &url, &error) ==
           S3_RESULT_CONFIGURATION_ERROR);

    client.endpoint = "http://example.test";
    assert(s3_url_build(&client, "bucket.with-dot", "key", &url, &error) ==
           S3_RESULT_OK);
    assert(strcmp(url, "http://bucket.with-dot.example.test/key") == 0);
    free(url);
}

static void test_object_head_sets_unreachable_state_error(void) {
    const struct s3_client_config config = {
        .endpoint = "http://127.0.0.1:1",
        .region = "us-east-1",
        .access_key = "test-access",
        .secret_key = "test-secret",
        .uri_style = S3_URI_STYLE_PATH,
    };
    struct s3_client *client = NULL;
    struct s3_error error = {0};
    struct s3_object_properties properties = {0};
    assert(s3_client_open(&client, &error, &config) == S3_RESULT_OK);
    client->max_attempts = 0;
    assert(s3_object_head(client, &error, &properties, "bucket", "key") ==
           S3_RESULT_ERROR);
    assert(error.result == S3_RESULT_ERROR);
    assert(strcmp(error.message, "unreachable HEAD state") == 0);
    s3_client_close(client);
}

static bool accept_bucket(void *data, const struct s3_bucket *bucket) {
    (void) data;
    (void) bucket;
    return true;
}

static void test_bucket_list_sets_unreachable_state_error(void) {
    const struct s3_client_config config = {
        .endpoint = "http://127.0.0.1:1",
        .region = "us-east-1",
        .access_key = "test-access",
        .secret_key = "test-secret",
        .uri_style = S3_URI_STYLE_PATH,
    };
    struct s3_client *client = NULL;
    struct s3_error error = {0};
    assert(s3_client_open(&client, &error, &config) == S3_RESULT_OK);
    client->max_attempts = 0;
    assert(s3_bucket_list(client, &error, accept_bucket, NULL) ==
           S3_RESULT_ERROR);
    assert(error.result == S3_RESULT_ERROR);
    assert(strcmp(error.message, "unreachable S3 request state") == 0);
    s3_client_close(client);
}

static bool cancel_request(void *data) { return *(bool *) data; }

static void test_client_cancellation_before_request(void) {
    struct s3_client_config config;
    struct s3_client *client = NULL;
    struct s3_error error = {0};
    bool cancelled = true;
    s3_config_init(&config);
    assert(config.max_attempts == 8);
    config.endpoint = "http://127.0.0.1:1";
    config.region = "us-east-1";
    config.access_key = "test-access";
    config.secret_key = "test-secret";
    assert(s3_client_open(&client, &error, &config) == S3_RESULT_OK);
    s3_client_set_cancel_callback(client, cancel_request, &cancelled);
    assert(s3_bucket_head(client, &error, "bucket") == S3_RESULT_ERROR);
    assert(strcmp(error.message, "interrupted") == 0);
    s3_client_close(client);
}

static void test_s3_error_log_part(void) {
    struct s3_error error = {
        .result = S3_RESULT_ACCESS_DENIED, .http_status = 403, .attempts = 2};
    struct log_part part = s3_log_error(&error);
    char *output = NULL;
    size_t size = 0;
    FILE *stream = open_memstream(&output, &size);
    assert(stream != NULL);
    part.value.custom.format(stream, part.value.custom.data);
    assert(fclose(stream) == 0);
    assert(strcmp(output, "access denied [HTTP 403] [after 2 attempts]") == 0);
    free(output);

    strcpy(error.message, "bad\nmessage");
    strcpy(error.s3_code, "code\x1b");
    strcpy(error.request_id, "id\\\t");
    stream = open_memstream(&output, &size);
    assert(stream != NULL);
    part.value.custom.format(stream, part.value.custom.data);
    assert(fclose(stream) == 0);
    assert(strcmp(output, "bad\\x0Amessage (S3 code\\x1B) [HTTP 403] "
                          "[request id\\\\\\x09] [after 2 attempts]") == 0);
    free(output);
}

static void assert_s3_logged_uri(const char *scheme, const char *bucket,
                                 const char *key, const char *expected) {
    struct log_part part = s3_log_uri(scheme, bucket, key);
    char *output = NULL;
    size_t size = 0;
    FILE *stream = open_memstream(&output, &size);
    assert(stream != NULL);
    assert(part.kind == log_PART_CUSTOM);
    part.value.custom.format(stream, part.value.custom.data);
    assert(fclose(stream) == 0);
    assert(strcmp(output, expected) == 0);
    free(output);
}

static void test_s3_uri_log_part(void) {
    assert_s3_logged_uri("s3", "bucket", "path/a b/%",
                         "s3://bucket/path/a%20b/%25");
    assert_s3_logged_uri(NULL, "a b", NULL, "a%20b");
    assert_s3_logged_uri("s3\n", "bucket", NULL, "s3\\x0A://bucket");
    assert_s3_logged_uri("s3", "bucket", NULL, "s3://bucket");
}

static void test_multipart_size_parser(void) {
    const struct {
        const char *text;
        uint64_t bytes;
    } valid[] = {
        {"5M", UINT64_C(5) * 1024 * 1024},
        {"0005M", UINT64_C(5) * 1024 * 1024},
        {"1G", UINT64_C(1024) * 1024 * 1024},
        {"5120M", UINT64_C(5) * 1024 * 1024 * 1024},
        {"5G", UINT64_C(5) * 1024 * 1024 * 1024},
    };
    size_t bytes = 0;
    assert(!s3ar_parse_multipart_size(NULL, &bytes));
    for (size_t i = 0; i < sizeof(valid) / sizeof(*valid); ++i) {
        bool fits = valid[i].bytes <= SIZE_MAX;
        errno = ERANGE;
        assert(s3ar_parse_multipart_size(valid[i].text, &bytes) == fits);
        if (fits) assert(bytes == valid[i].bytes);
    }
}

static void test_xml_child_text(void) {
    const char body[] = "<Root xmlns='urn:test'><Empty/><Value>a&amp;b</Value>"
                        "<Nested><Value>hidden</Value></Nested>"
                        "<Value>second</Value></Root>";
    xmlDoc *doc = s3_xml_read(body, sizeof(body) - 1, sizeof(body), "test.xml");
    assert(doc != NULL);
    xmlNode *root = xmlDocGetRootElement(doc);
    assert(s3_xml_content(NULL, "Value") == NULL);
    assert(s3_xml_text(NULL, "Value") == NULL);
    assert(s3_xml_content(root, "Missing") == NULL);
    assert(s3_xml_text(root, "Missing") == NULL);
    xmlChar *content = s3_xml_content(root, "Empty");
    char *empty = s3_xml_text(root, "Empty");
    assert(content != NULL && content[0] == '\0');
    assert(empty != NULL && empty[0] == '\0');
    xmlFree(content);
    free(empty);
    content = s3_xml_content(root, "Value");
    char *text = s3_xml_text(root, "Value");
    xmlFreeDoc(doc);
    assert(content != NULL && strcmp((const char *) content, "a&b") == 0);
    assert(text != NULL && strcmp(text, "a&b") == 0);
    xmlFree(content);
    free(text);
}

static void test_parse_u64(void) {
    const struct {
        const char *text;
        uint64_t value;
    } valid[] = {{"0", 0}, {"00042", 42}, {"18446744073709551615", UINT64_MAX}};
    uint64_t value;
    for (size_t i = 0; i < sizeof(valid) / sizeof(*valid); ++i) {
        assert(s3_parse_u64(valid[i].text,
                            valid[i].text + strlen(valid[i].text), &value));
        assert(value == valid[i].value);
    }
    const char *invalid[] = {"",
                             "+1",
                             "-1",
                             " 1",
                             "1 ",
                             "1a",
                             "1.0",
                             "18446744073709551616",
                             "999999999999999999999"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        value = 123;
        assert(
            !s3_parse_u64(invalid[i], invalid[i] + strlen(invalid[i]), &value));
        assert(value == 123);
    }
    const char range[] = {'4', '2', 'x'};
    assert(s3_parse_u64(range, range + 2, &value) && value == 42);
    const char embedded_nul[] = {'1', '\0', '2'};
    assert(!s3_parse_u64(embedded_nul, embedded_nul + sizeof(embedded_nul),
                         &value));
    assert(value == 42);
}

int main(void) {
    test_response_take_properties();
    test_content_range_bounds();
    test_parse_u64();
    test_xml_child_text();
    test_multipart_size_parser();
    test_public_api_clears_error();
    test_uri_variants();
    test_multiple_clients();
    test_error_set_preserves_response_details();
    test_error_xml_parser();
    test_retry_response_headers();
    test_long_object_property_headers();
    test_retry_after_header_variants();
    test_status_header_does_not_require_nul_termination();
    test_redirect_retry_and_diagnostic();
    test_object_name_validator_clears_error();
    test_bucket_validation_depends_on_uri_style();
    test_object_name_validator_without_client();
    test_object_head_sets_unreachable_state_error();
    test_bucket_list_sets_unreachable_state_error();
    test_client_cancellation_before_request();
    test_s3_error_log_part();
    test_s3_uri_log_part();
    return 0;
}
