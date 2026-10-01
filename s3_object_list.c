/* SPDX-License-Identifier: MIT-0 */
#include "s3_internal.h"
#include "s3_xml.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct listed_object {
    char *key;
    char *etag;
    uint64_t size;
    int64_t modified;
};

struct list_page {
    struct listed_object *items;
    size_t count;
    size_t capacity;
    bool truncated;
    char *token;
};

static bool leap_year(unsigned year) {
    return year % 4U == 0 && (year % 100U != 0 || year % 400U == 0);
}

static bool parse_timestamp(const char *text, int64_t *timestamp) {
    static const unsigned month_days[] = {31, 28, 31, 30, 31, 30,
                                          31, 31, 30, 31, 30, 31};
    unsigned year, month, day, hour, minute, second;
    int consumed = 0;
    const char *end;
    int64_t adjusted_year, era, days;
    unsigned year_of_era, day_of_year, day_of_era;
    if (strlen(text) < 20 || text[4] != '-' || text[7] != '-' ||
        text[10] != 'T' || text[13] != ':' || text[16] != ':' ||
        sscanf(text, "%4u-%2u-%2uT%2u:%2u:%2u%n", &year, &month, &day, &hour,
               &minute, &second, &consumed) != 6 ||
        consumed != 19)
        return false;
    end = text + consumed;
    if (*end == '.') {
        ++end;
        if (*end < '0' || *end > '9') return false;
        while (*end >= '0' && *end <= '9') ++end;
    }
    if (end[0] != 'Z' || end[1] != '\0' || year == 0 || month == 0 ||
        month > 12 || day == 0 ||
        day >
            month_days[month - 1] + (month == 2 && leap_year(year) ? 1U : 0U) ||
        hour > 23 || minute > 59 || second > 59)
        return false;
    adjusted_year = (int64_t) year - (month <= 2 ? 1 : 0);
    era = adjusted_year / 400;
    year_of_era = (unsigned) (adjusted_year - era * 400);
    day_of_year =
        (153U * (month > 2 ? month - 3U : month + 9U) + 2U) / 5U + day - 1U;
    day_of_era = year_of_era * 365U + year_of_era / 4U - year_of_era / 100U +
                 day_of_year;
    days = era * 146097 + (int64_t) day_of_era - 719468;
    *timestamp =
        days * 86400 + (int64_t) hour * 3600 + (int64_t) minute * 60 + second;
    return true;
}

static void free_page(struct list_page *page) {
    for (size_t i = 0; i < page->count; ++i) {
        free(page->items[i].key);
        free(page->items[i].etag);
    }
    free(page->items);
    free(page->token);
    memset(page, 0, sizeof(*page));
}

static bool parse_u64_text(const char *text, uint64_t *result) {
    uint64_t value = 0;
    if (text == NULL || *text == '\0') return false;
    for (; *text != '\0'; ++text) {
        unsigned digit;
        if (*text < '0' || *text > '9') return false;
        digit = (unsigned) (*text - '0');
        if (value > (UINT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    *result = value;
    return true;
}

static int hex_value(unsigned char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

static enum s3_result decode_list_key(const char *encoded, char **decoded) {
    size_t input_size = strlen(encoded), output_size = 0;
    char *output = malloc(input_size + 1);
    if (output == NULL) return S3_RESULT_ERROR;
    for (size_t i = 0; i < input_size; ++i) {
        unsigned char byte = (unsigned char) encoded[i];
        if (byte == '%') {
            int high, low;
            if (i + 2 >= input_size) goto invalid;
            high = hex_value((unsigned char) encoded[++i]);
            low = hex_value((unsigned char) encoded[++i]);
            if (high < 0 || low < 0) goto invalid;
            byte = (unsigned char) ((high << 4) | low);
        }
        if (byte == '\0') goto invalid;
        output[output_size++] = (char) byte;
    }
    output[output_size] = '\0';
    *decoded = output;
    return S3_RESULT_OK;
invalid:
    free(output);
    return S3_RESULT_PROTOCOL_ERROR;
}

static enum s3_result append_xml_object(struct list_page *page, xmlNode *node) {
    xmlNode *key_node = s3_xml_child(node, "Key");
    xmlNode *size_node = s3_xml_child(node, "Size");
    xmlNode *date_node = s3_xml_child(node, "LastModified");
    xmlNode *etag_node = s3_xml_child(node, "ETag");
    xmlChar *key = key_node != NULL ? xmlNodeGetContent(key_node) : NULL;
    xmlChar *size = size_node != NULL ? xmlNodeGetContent(size_node) : NULL;
    xmlChar *date = date_node != NULL ? xmlNodeGetContent(date_node) : NULL;
    xmlChar *etag = etag_node != NULL ? xmlNodeGetContent(etag_node) : NULL;
    struct listed_object *items;
    uint64_t bytes;
    int64_t modified;
    size_t capacity;
    char *decoded_key = NULL;
    enum s3_result result = S3_RESULT_PROTOCOL_ERROR;
    if (key == NULL || key[0] == '\0' || size == NULL || date == NULL ||
        !parse_u64_text((const char *) size, &bytes))
        goto done;
    if (!parse_timestamp((const char *) date, &modified)) goto done;
    result = decode_list_key((const char *) key, &decoded_key);
    if (result != S3_RESULT_OK) goto done;
    if (!s3_url_key_valid(decoded_key)) {
        result = S3_RESULT_PROTOCOL_ERROR;
        goto done;
    }
    if (page->count == page->capacity) {
        capacity = page->capacity == 0 ? 16 : page->capacity * 2;
        if (capacity < page->capacity || capacity > SIZE_MAX / sizeof(*items)) {
            result = S3_RESULT_ERROR;
            goto done;
        }
        items = realloc(page->items, capacity * sizeof(*items));
        if (items == NULL) {
            result = S3_RESULT_ERROR;
            goto done;
        }
        page->items = items;
        page->capacity = capacity;
    }
    page->items[page->count].key = decoded_key;
    decoded_key = NULL;
    page->items[page->count].etag =
        etag != NULL ? s3_memory_strdup((const char *) etag) : NULL;
    if (page->items[page->count].key == NULL ||
        (etag != NULL && page->items[page->count].etag == NULL)) {
        free(page->items[page->count].key);
        free(page->items[page->count].etag);
        result = S3_RESULT_ERROR;
        goto done;
    }
    page->items[page->count].size = bytes;
    page->items[page->count].modified = modified;
    ++page->count;
    result = S3_RESULT_OK;
done:
    free(decoded_key);
    xmlFree(key);
    xmlFree(size);
    xmlFree(date);
    xmlFree(etag);
    return result;
}

static enum s3_result parse_page(const char *body, size_t size,
                                 struct list_page *page,
                                 struct s3_error *error) {
    xmlDoc *doc = NULL;
    xmlNode *root;
    xmlNode *truncated_node;
    xmlNode *encoding_node;
    xmlChar *truncated = NULL;
    xmlChar *encoding = NULL;
    enum s3_result result = S3_RESULT_OK;
    doc = s3_xml_read(body, size, S3_XML_BODY_LIMIT, "s3-list.xml");
    root = doc != NULL ? xmlDocGetRootElement(doc) : NULL;
    if (!s3_xml_name(root, "ListBucketResult")) goto invalid;
    encoding_node = s3_xml_child(root, "EncodingType");
    encoding = encoding_node != NULL ? xmlNodeGetContent(encoding_node) : NULL;
    if (encoding == NULL || strcmp((const char *) encoding, "url") != 0)
        goto invalid;
    truncated_node = s3_xml_child(root, "IsTruncated");
    truncated =
        truncated_node != NULL ? xmlNodeGetContent(truncated_node) : NULL;
    if (truncated == NULL || (strcmp((const char *) truncated, "true") != 0 &&
                              strcmp((const char *) truncated, "false") != 0))
        goto invalid;
    page->truncated = strcmp((const char *) truncated, "true") == 0;
    for (xmlNode *node = root->children; node != NULL; node = node->next) {
        if (!s3_xml_name(node, "Contents")) continue;
        if (page->count == 1000) goto invalid;
        result = append_xml_object(page, node);
        if (result != S3_RESULT_OK) goto failed;
    }
    if (page->truncated) {
        xmlNode *token_node = s3_xml_child(root, "NextContinuationToken");
        xmlChar *token =
            token_node != NULL ? xmlNodeGetContent(token_node) : NULL;
        if (token == NULL || token[0] == '\0') {
            xmlFree(token);
            goto invalid;
        }
        page->token = s3_memory_strdup((const char *) token);
        xmlFree(token);
        if (page->token == NULL) {
            result = S3_RESULT_ERROR;
            goto failed;
        }
    }
    xmlFree(truncated);
    xmlFree(encoding);
    xmlFreeDoc(doc);
    return S3_RESULT_OK;
invalid:
    result = S3_RESULT_PROTOCOL_ERROR;
failed:
    xmlFree(truncated);
    xmlFree(encoding);
    if (doc != NULL) xmlFreeDoc(doc);
    free_page(page);
    return s3_error_set(error, result,
                        result == S3_RESULT_ERROR
                            ? "out of memory"
                            : "invalid ListObjectsV2 XML");
}

static enum s3_result fetch_page(struct s3_client *client,
                                 struct s3_error *error, const char *bucket,
                                 const char *prefix, const char *token,
                                 struct list_page *page) {
    char *encoded_prefix = NULL, *encoded_token = NULL, *query = NULL;
    char *body = NULL;
    size_t size = 0;
    enum s3_result result;
    size_t query_size;
    if (prefix != NULL && prefix[0] != '\0')
        encoded_prefix = s3_uri_encode_alloc(prefix, false);
    if (token != NULL) encoded_token = s3_uri_encode_alloc(token, false);
    if ((prefix != NULL && prefix[0] != '\0' && encoded_prefix == NULL) ||
        (token != NULL && encoded_token == NULL)) {
        result = s3_error_set(error, S3_RESULT_ERROR, "out of memory");
        goto done;
    }
    query_size = 96 + (encoded_prefix != NULL ? strlen(encoded_prefix) : 0) +
                 (encoded_token != NULL ? strlen(encoded_token) : 0);
    query = malloc(query_size);
    if (query == NULL) {
        result = s3_error_set(error, S3_RESULT_ERROR, "out of memory");
        goto done;
    }
    (void) snprintf(query, query_size,
                    "list-type=2&max-keys=1000&encoding-type=url%s%s%s%s",
                    encoded_prefix != NULL ? "&prefix=" : "",
                    encoded_prefix != NULL ? encoded_prefix : "",
                    encoded_token != NULL ? "&continuation-token=" : "",
                    encoded_token != NULL ? encoded_token : "");
    result = s3_request_bucket(client, error, bucket, query, "GET", NULL, &body,
                               &size);
    if (result == S3_RESULT_OK) result = parse_page(body, size, page, error);
done:
    free(encoded_prefix);
    free(encoded_token);
    free(query);
    free(body);
    return result;
}

enum s3_result s3_object_list(struct s3_client *client, struct s3_error *error,
                              const char *bucket, const char *prefix,
                              s3_object_callback callback, void *data,
                              size_t *count) {
    char *token = NULL;
    size_t total = 0;
    enum s3_result result;
    s3_error_clear(error);
    if (count != NULL) *count = 0;
    if (client == NULL || error == NULL || callback == NULL)
        return s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                            "invalid ListObjectsV2 arguments");
    do {
        struct list_page page = {0};
        result = fetch_page(client, error, bucket, prefix, token, &page);
        if (result != S3_RESULT_OK) {
            free_page(&page);
            break;
        }
        for (size_t i = 0; i < page.count; ++i) {
            const struct s3_object object = {
                .bucket = bucket,
                .key = page.items[i].key,
                .size = page.items[i].size,
                .last_modified = page.items[i].modified,
                .etag = page.items[i].etag,
            };
            if (!callback(data, &object)) {
                error->callback_errno = errno != 0 ? errno : EIO;
                result = s3_error_set(error, S3_RESULT_CALLBACK_ERROR,
                                      "object callback failed");
                break;
            }
        }
        if (result == S3_RESULT_OK && SIZE_MAX - total < page.count)
            result = s3_error_set(error, S3_RESULT_ERROR, "too many objects");
        if (result == S3_RESULT_OK) total += page.count;
        if (result == S3_RESULT_OK && page.truncated) {
            if (page.token == NULL ||
                (token != NULL && strcmp(token, page.token) == 0))
                result =
                    s3_error_set(error, S3_RESULT_PROTOCOL_ERROR,
                                 "repeated ListObjectsV2 continuation token");
            else {
                free(token);
                token = page.token;
                page.token = NULL;
            }
        }
        else if (result == S3_RESULT_OK) {
            free(token);
            token = NULL;
        }
        free_page(&page);
        if (result != S3_RESULT_OK) break;
    } while (token != NULL);
    free(token);
    if (result == S3_RESULT_OK && count != NULL) *count = total;
    return result;
}
