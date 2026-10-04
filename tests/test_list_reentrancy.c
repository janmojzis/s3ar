/* SPDX-License-Identifier: MIT-0 */
#include "s3_internal.h"

#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static struct s3_client *client;
static struct s3_memory_response *response;
static bool transferring;
static unsigned requests, objects, buckets;

void __real_s3_response_memory_reset(struct s3_memory_response *context,
                                     size_t limit);
void __wrap_s3_response_memory_reset(struct s3_memory_response *context,
                                     size_t limit) {
    assert(!transferring);
    __real_s3_response_memory_reset(context, limit);
    response = context;
}

CURLcode __wrap_curl_easy_getinfo(CURL *curl, CURLINFO info, ...) {
    assert(curl == client->curl && info == CURLINFO_RESPONSE_CODE);
    va_list arguments;
    va_start(arguments, info);
    *va_arg(arguments, long *) = response->response.status;
    va_end(arguments);
    return CURLE_OK;
}

CURLcode __wrap_curl_easy_perform(CURL *curl) {
    assert(curl == client->curl && !transferring);
    transferring = true;
    char *url = NULL;
    assert(curl_url_get(client->url, CURLUPART_URL, &url, 0) == CURLUE_OK);
    const char *body = "";
    switch (++requests) {
        case 1:
            assert(strcmp(url, "http://example.test/?max-buckets=10000") == 0);
            body =
                "<ListAllMyBucketsResult><Buckets><Bucket><Name>bucket</Name>"
                "</Bucket></Buckets></ListAllMyBucketsResult>";
            break;
        case 2:
            assert(strcmp(url, "http://example.test/bucket?list-type=2&"
                               "max-keys=1000&encoding-type=url") == 0);
            body = "<ListBucketResult><EncodingType>url</EncodingType>"
                   "<IsTruncated>true</IsTruncated>"
                   "<NextContinuationToken>next</NextContinuationToken>"
                   "<Contents><Key>a</Key><Size>1</Size>"
                   "<LastModified>2026-10-04T00:00:00Z</LastModified>"
                   "<ETag>etag-a</ETag></Contents>"
                   "<Contents><Key>b</Key><Size>1</Size>"
                   "<LastModified>2026-10-04T00:00:00Z</LastModified>"
                   "<ETag>etag-b</ETag></Contents></ListBucketResult>";
            break;
        case 3:
        case 4:
        case 6:
            assert(strcmp(url, "http://example.test/bucket") == 0);
            break;
        case 5:
            assert(strcmp(url, "http://example.test/bucket?list-type=2&"
                               "max-keys=1000&encoding-type=url&"
                               "continuation-token=next") == 0);
            body = "<ListBucketResult><EncodingType>url</EncodingType>"
                   "<IsTruncated>false</IsTruncated>"
                   "<Contents><Key>c</Key><Size>1</Size>"
                   "<LastModified>2026-10-04T00:00:00Z</LastModified>"
                   "<ETag>etag-c</ETag></Contents></ListBucketResult>";
            break;
        default:
            assert(false);
    }
    response->response.status = 200;
    assert(s3_response_memory_collect((char *) body, 1, strlen(body),
                                      response) == strlen(body));
    curl_free(url);
    transferring = false;
    return CURLE_OK;
}

static bool visit_object(void *data, const struct s3_object *object) {
    assert(data == client && !transferring);
    assert(objects < 3);
    char key[] = {(char) ('a' + objects), '\0'};
    char etag[] = "etag-a";
    etag[5] = key[0];
    assert(strcmp(object->bucket, "bucket") == 0);
    assert(strcmp(object->key, key) == 0);
    assert(strcmp(object->etag, etag) == 0);
    struct s3_error nested_error = {0};
    assert(s3_bucket_head(client, &nested_error, object->bucket) ==
           S3_RESULT_OK);
    assert(nested_error.result == S3_RESULT_OK);
    /* Nested requests must preserve the borrowed listing item. */
    assert(strcmp(object->key, key) == 0);
    assert(strcmp(object->etag, etag) == 0);
    ++objects;
    return true;
}

static bool visit_bucket(void *data, const struct s3_bucket *bucket) {
    assert(data == client && !transferring);
    assert(requests == 1 && buckets == 0);
    assert(strcmp(bucket->name, "bucket") == 0);
    struct s3_error nested_error = {0};
    size_t count = 0;
    assert(s3_object_list(client, &nested_error, bucket->name, NULL,
                          visit_object, client, &count) == S3_RESULT_OK);
    assert(nested_error.result == S3_RESULT_OK && count == 3);
    assert(strcmp(bucket->name, "bucket") == 0);
    ++buckets;
    return true;
}

int main(void) {
    struct s3_client_config config;
    struct s3_error error = {0};
    s3_config_init(&config);
    config.endpoint = "http://example.test";
    config.region = "us-east-1";
    config.access_key = "test";
    config.secret_key = "test";
    config.max_attempts = 1;
    assert(s3_client_open(&client, &error, &config) == S3_RESULT_OK);
    assert(s3_bucket_list(client, &error, visit_bucket, client) ==
           S3_RESULT_OK);
    assert(error.result == S3_RESULT_OK);
    assert(buckets == 1 && objects == 3 && requests == 6);
    s3_client_close(client);
    return 0;
}
