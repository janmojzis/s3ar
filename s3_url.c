/* SPDX-License-Identifier: MIT-0 */
#include "s3_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
           c == '~';
}

char *s3_uri_encode(const unsigned char *input, bool keep_slash) {
    static const char hex[] = "0123456789ABCDEF";
    size_t length = strlen((const char *) input), out_size = 0;
    char *output = malloc(length > (SIZE_MAX - 1) / 3 ? 0 : length * 3 + 1);
    if (output == NULL) return NULL;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = input[i];
        if (unreserved(c) || (keep_slash && c == '/'))
            output[out_size++] = (char) c;
        else {
            output[out_size++] = '%';
            output[out_size++] = hex[c >> 4];
            output[out_size++] = hex[c & 15];
        }
    }
    output[out_size] = '\0';
    return output;
}

static bool valid_endpoint(const char *endpoint, const char **authority) {
    const char *p;
    if (strncmp(endpoint, "http://", 7) == 0)
        *authority = endpoint + 7;
    else if (strncmp(endpoint, "https://", 8) == 0)
        *authority = endpoint + 8;
    else
        return false;
    if (**authority == '\0' || strchr(*authority, '/') != NULL ||
        strchr(*authority, '?') != NULL || strchr(*authority, '#') != NULL ||
        strchr(*authority, '@') != NULL)
        return false;
    p = *authority;
    if (*p == '[') {
        p = strchr(p, ']');
        if (p == NULL || (p[1] != '\0' && p[1] != ':')) return false;
    }
    return true;
}

enum s3_result s3_build_service_url(const struct s3_client *client, char **url,
                                    struct s3_error *error) {
    const char *authority;
    size_t size;
    *url = NULL;
    if (client == NULL || !valid_endpoint(client->endpoint, &authority))
        return s3_error_set(
            error, S3_RESULT_CONFIGURATION_ERROR,
            "S3 endpoint must be http(s)://host[:port] without a path");
    (void) authority;
    size = strlen(client->endpoint) + 2;
    *url = malloc(size);
    if (*url == NULL)
        return s3_error_set(error, S3_RESULT_ERROR, "out of memory");
    (void) snprintf(*url, size, "%s/", client->endpoint);
    return S3_RESULT_OK;
}

static bool has_suffix(const char *text, const char *suffix) {
    size_t a = strlen(text), b = strlen(suffix);
    return a >= b && memcmp(text + a - b, suffix, b) == 0;
}

static bool ipv4_shaped(const char *text) {
    unsigned groups = 0;
    const char *p = text;
    while (*p != '\0') {
        unsigned digits = 0;
        while (*p >= '0' && *p <= '9') {
            ++digits;
            ++p;
        }
        if (digits == 0) return false;
        ++groups;
        if (*p == '\0') break;
        if (*p++ != '.') return false;
    }
    return groups == 4;
}

static bool dns_bucket(const char *bucket) {
    size_t n = strlen(bucket);
    if (n < 3 || n > 63 || bucket[0] == '-' || bucket[n - 1] == '-' ||
        bucket[0] == '.' || bucket[n - 1] == '.' ||
        strstr(bucket, "..") != NULL || ipv4_shaped(bucket) ||
        strncmp(bucket, "xn--", 4) == 0 || strncmp(bucket, "sthree-", 7) == 0 ||
        strncmp(bucket, "amzn-s3-demo-", 13) == 0 ||
        has_suffix(bucket, "-s3alias") || has_suffix(bucket, "--ol-s3") ||
        has_suffix(bucket, ".mrap") || has_suffix(bucket, "--x-s3") ||
        has_suffix(bucket, "--table-s3"))
        return false;
    for (size_t i = 0; i < n; ++i) {
        if (!((bucket[i] >= 'a' && bucket[i] <= 'z') ||
              (bucket[i] >= '0' && bucket[i] <= '9') || bucket[i] == '-' ||
              bucket[i] == '.'))
            return false;
        if (bucket[i] == '.' && i != 0 && i + 1 < n &&
            (bucket[i - 1] == '-' || bucket[i + 1] == '-'))
            return false;
    }
    return true;
}

static bool valid_utf8(const unsigned char *text, size_t size) {
    size_t i = 0;
    while (i < size) {
        unsigned char c = text[i++];
        unsigned needed;
        uint32_t value;
        if (c < 0x80) continue;
        if (c >= 0xc2 && c <= 0xdf) {
            needed = 1;
            value = c & 0x1f;
        }
        else if (c >= 0xe0 && c <= 0xef) {
            needed = 2;
            value = c & 0x0f;
        }
        else if (c >= 0xf0 && c <= 0xf4) {
            needed = 3;
            value = c & 0x07;
        }
        else
            return false;
        if (needed > size - i) return false;
        for (unsigned j = 0; j < needed; ++j) {
            unsigned char next = text[i++];
            if ((next & 0xc0) != 0x80) return false;
            value = (value << 6) | (next & 0x3f);
        }
        if ((needed == 2 && value < 0x800) ||
            (needed == 3 && value < 0x10000) || value > 0x10ffff ||
            (value >= 0xd800 && value <= 0xdfff))
            return false;
    }
    return true;
}

static bool valid_key(const char *key) {
    size_t size = key != NULL ? strlen(key) : 0;
    return size >= 1 && size <= 1024 &&
           valid_utf8((const unsigned char *) key, size);
}

enum s3_result s3_build_url(const struct s3_client *client, const char *bucket,
                            const char *key, char **url,
                            struct s3_error *error) {
    const char *authority;
    char *encoded_bucket = NULL, *encoded_key = NULL;
    size_t size;
    *url = NULL;
    if (!valid_endpoint(client->endpoint, &authority))
        return s3_error_set(
            error, S3_RESULT_CONFIGURATION_ERROR,
            "S3 endpoint must be http(s)://host[:port] without a path");
    if (bucket == NULL || !dns_bucket(bucket) || !valid_key(key))
        return s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                            "invalid S3 bucket or object key");
    encoded_key = s3_uri_encode((const unsigned char *) key, true);
    if (client->uri_style == S3_URI_STYLE_PATH)
        encoded_bucket = s3_uri_encode((const unsigned char *) bucket, false);
    else if (!dns_bucket(bucket)) {
        free(encoded_key);
        return s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                            "bucket is not valid for virtual-host URI style");
    }
    if (encoded_key == NULL ||
        (client->uri_style == S3_URI_STYLE_PATH && encoded_bucket == NULL))
        goto oom;
    size = strlen(client->endpoint) + strlen(bucket) + strlen(encoded_key) + 3;
    if (client->uri_style == S3_URI_STYLE_PATH)
        size += strlen(encoded_bucket) - strlen(bucket);
    *url = malloc(size);
    if (*url == NULL) goto oom;
    if (client->uri_style == S3_URI_STYLE_PATH)
        (void) snprintf(*url, size, "%s/%s/%s", client->endpoint,
                        encoded_bucket, encoded_key);
    else {
        size_t scheme = (size_t) (authority - client->endpoint);
        (void) snprintf(*url, size, "%.*s%s.%s/%s", (int) scheme,
                        client->endpoint, bucket, authority, encoded_key);
    }
    free(encoded_bucket);
    free(encoded_key);
    return S3_RESULT_OK;
oom:
    free(encoded_bucket);
    free(encoded_key);
    return s3_error_set(error, S3_RESULT_ERROR, "out of memory");
}

static enum s3_result append_query(char **url, const char *query,
                                   struct s3_error *error) {
    char *joined;
    size_t a, b;
    if (query == NULL || query[0] == '\0') return S3_RESULT_OK;
    a = strlen(*url);
    b = strlen(query);
    if (a > SIZE_MAX - b - 2)
        return s3_error_set(error, S3_RESULT_ERROR, "out of memory");
    joined = realloc(*url, a + b + 2);
    if (joined == NULL)
        return s3_error_set(error, S3_RESULT_ERROR, "out of memory");
    joined[a] = '?';
    memcpy(joined + a + 1, query, b + 1);
    *url = joined;
    return S3_RESULT_OK;
}

enum s3_result s3_build_object_url(const struct s3_client *client,
                                   const char *bucket, const char *key,
                                   const char *query, char **url,
                                   struct s3_error *error) {
    enum s3_result result = s3_build_url(client, bucket, key, url, error);
    if (result == S3_RESULT_OK) result = append_query(url, query, error);
    if (result != S3_RESULT_OK) {
        free(*url);
        *url = NULL;
    }
    return result;
}

enum s3_result s3_build_bucket_url(const struct s3_client *client,
                                   const char *bucket, const char *query,
                                   char **url, struct s3_error *error) {
    const char *authority;
    char *encoded = NULL;
    size_t size;
    if (url == NULL)
        return s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                            "invalid bucket URL arguments");
    *url = NULL;
    if (client == NULL || !valid_endpoint(client->endpoint, &authority) ||
        bucket == NULL || !dns_bucket(bucket))
        return s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                            "invalid S3 bucket or endpoint");
    if (client->uri_style == S3_URI_STYLE_PATH) {
        encoded = s3_uri_encode((const unsigned char *) bucket, false);
        if (encoded == NULL)
            return s3_error_set(error, S3_RESULT_ERROR, "out of memory");
        size = strlen(client->endpoint) + strlen(encoded) + 2;
        *url = malloc(size);
        if (*url != NULL)
            (void) snprintf(*url, size, "%s/%s", client->endpoint, encoded);
    }
    else {
        size_t scheme;
        if (!dns_bucket(bucket))
            return s3_error_set(
                error, S3_RESULT_CONFIGURATION_ERROR,
                "bucket is not valid for virtual-host URI style");
        scheme = (size_t) (authority - client->endpoint);
        size = strlen(client->endpoint) + strlen(bucket) + 3;
        *url = malloc(size);
        if (*url != NULL)
            (void) snprintf(*url, size, "%.*s%s.%s/", (int) scheme,
                            client->endpoint, bucket, authority);
    }
    free(encoded);
    if (*url == NULL)
        return s3_error_set(error, S3_RESULT_ERROR, "out of memory");
    {
        enum s3_result result = append_query(url, query, error);
        if (result != S3_RESULT_OK) {
            free(*url);
            *url = NULL;
        }
        return result;
    }
}
