/* SPDX-License-Identifier: MIT-0 */
#include "s3_internal.h"
#include "s3_xml.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct owned_bucket {
    char *name;
};

struct bucket_array {
    struct owned_bucket *items;
    size_t count;
    size_t capacity;
};

static void bucket_array_free(struct bucket_array *array) {
    for (size_t i = 0; i < array->count; ++i) free(array->items[i].name);
    free(array->items);
}

static enum s3_result bucket_array_append(struct bucket_array *array,
                                          xmlDoc *doc, xmlNode *node) {
    xmlNode *name_node = s3_xml_child(node, "Name");
    xmlChar *name = NULL;
    struct owned_bucket *items;
    size_t capacity;
    enum s3_result result = S3_RESULT_PROTOCOL_ERROR;
    if (name_node == NULL) return result;
    name = xmlNodeListGetString(doc, name_node->children, 1);
    if (name == NULL || name[0] == '\0') goto done;
    if (array->count == array->capacity) {
        capacity = array->capacity == 0 ? 8 : array->capacity * 2;
        if (capacity < array->capacity ||
            capacity > SIZE_MAX / sizeof(*array->items)) {
            result = S3_RESULT_ERROR;
            goto done;
        }
        items = realloc(array->items, capacity * sizeof(*items));
        if (items == NULL) {
            result = S3_RESULT_ERROR;
            goto done;
        }
        array->items = items;
        array->capacity = capacity;
    }
    array->items[array->count].name = s3_memory_strdup((const char *) name);
    if (array->items[array->count].name == NULL) {
        result = S3_RESULT_ERROR;
        goto done;
    }
    ++array->count;
    result = S3_RESULT_OK;

done:
    xmlFree(name);
    return result;
}

static int compare_buckets(const void *left, const void *right) {
    const struct owned_bucket *a = left;
    const struct owned_bucket *b = right;
    return strcmp(a->name, b->name);
}

static enum s3_result parse_bucket_page(const char *body, size_t size,
                                        struct bucket_array *buckets,
                                        char **continuation_token,
                                        struct s3_error *error) {
    xmlDoc *doc;
    xmlNode *root;
    xmlNode *container;
    enum s3_result result = S3_RESULT_OK;
    *continuation_token = NULL;
    doc = s3_xml_read(body, size, S3_XML_BODY_LIMIT, "s3-buckets.xml");
    if (doc == NULL) {
        return s3_error_set(error, S3_RESULT_PROTOCOL_ERROR,
                            "invalid ListBuckets XML");
    }
    root = xmlDocGetRootElement(doc);
    container = s3_xml_name(root, "ListAllMyBucketsResult")
                    ? s3_xml_child(root, "Buckets")
                    : NULL;
    if (container == NULL) result = S3_RESULT_PROTOCOL_ERROR;
    for (xmlNode *node = container != NULL ? container->children : NULL;
         result == S3_RESULT_OK && node != NULL; node = node->next) {
        enum s3_result append_result;
        if (node->type != XML_ELEMENT_NODE) continue;
        if (!s3_xml_name(node, "Bucket")) continue;
        append_result = bucket_array_append(buckets, doc, node);
        if (append_result != S3_RESULT_OK) result = append_result;
    }
    if (result == S3_RESULT_OK) {
        xmlNode *token_node = s3_xml_child(root, "ContinuationToken");
        if (token_node != NULL) {
            xmlChar *token = xmlNodeListGetString(doc, token_node->children, 1);
            if (token == NULL || token[0] == '\0')
                result = S3_RESULT_PROTOCOL_ERROR;
            else {
                *continuation_token = s3_memory_strdup((const char *) token);
                if (*continuation_token == NULL) result = S3_RESULT_ERROR;
            }
            xmlFree(token);
        }
    }
    xmlFreeDoc(doc);
    if (result != S3_RESULT_OK) {
        free(*continuation_token);
        *continuation_token = NULL;
        return s3_error_set(error, result,
                            result == S3_RESULT_ERROR
                                ? "out of memory"
                                : "invalid ListBuckets XML");
    }
    return S3_RESULT_OK;
}

static enum s3_result fetch_bucket_page(struct s3_client *client,
                                        struct s3_error *error, const char *url,
                                        struct bucket_array *buckets,
                                        char **continuation_token) {
    char *body = NULL;
    size_t size = 0;
    enum s3_result result =
        s3_request_url(client, error, url, "GET", NULL, &body, &size);
    if (result == S3_RESULT_OK)
        result =
            parse_bucket_page(body, size, buckets, continuation_token, error);
    free(body);
    return result;
}

static enum s3_result build_bucket_list_query(const char *continuation_token,
                                              char **query,
                                              struct s3_error *error) {
    static const char prefix[] = "max-buckets=10000&continuation-token=";
    char *encoded;
    size_t size;
    if (continuation_token == NULL) {
        *query = s3_memory_strdup("max-buckets=10000");
        if (*query == NULL)
            return s3_error_set(error, S3_RESULT_ERROR, "out of memory");
        return S3_RESULT_OK;
    }
    encoded = s3_uri_encode_alloc(continuation_token, false);
    if (encoded == NULL)
        return s3_error_set(error, S3_RESULT_ERROR, "out of memory");
    if (strlen(encoded) > SIZE_MAX - sizeof(prefix)) {
        free(encoded);
        return s3_error_set(error, S3_RESULT_ERROR, "out of memory");
    }
    size = sizeof(prefix) + strlen(encoded);
    *query = malloc(size);
    if (*query == NULL) {
        free(encoded);
        return s3_error_set(error, S3_RESULT_ERROR, "out of memory");
    }
    (void) snprintf(*query, size, "%s%s", prefix, encoded);
    free(encoded);
    return S3_RESULT_OK;
}

enum s3_result s3_bucket_list(struct s3_client *client, struct s3_error *error,
                              s3_bucket_callback callback, void *data) {
    struct bucket_array buckets = {0};
    char *continuation_token = NULL;
    enum s3_result result = S3_RESULT_OK;
    s3_error_clear(error);
    if (client == NULL || error == NULL || callback == NULL)
        return s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                            "invalid ListBuckets arguments");
    do {
        char *query = NULL;
        char *url = NULL;
        char *next_token = NULL;
        result = build_bucket_list_query(continuation_token, &query, error);
        if (result == S3_RESULT_OK)
            result = s3_url_build_service(client, query, &url, error);
        free(query);
        if (result == S3_RESULT_OK)
            result =
                fetch_bucket_page(client, error, url, &buckets, &next_token);
        free(url);
        if (result == S3_RESULT_OK && next_token != NULL &&
            continuation_token != NULL &&
            strcmp(next_token, continuation_token) == 0)
            result = s3_error_set(error, S3_RESULT_PROTOCOL_ERROR,
                                  "repeated ListBuckets continuation token");
        free(continuation_token);
        continuation_token = next_token;
    } while (result == S3_RESULT_OK && continuation_token != NULL);
    free(continuation_token);
    if (result == S3_RESULT_OK && buckets.count > 1)
        qsort(buckets.items, buckets.count, sizeof(*buckets.items),
              compare_buckets);
    for (size_t i = 0; result == S3_RESULT_OK && i < buckets.count; ++i) {
        const struct s3_bucket bucket = {
            .name = buckets.items[i].name,
        };
        if (!callback(data, &bucket)) {
            error->callback_errno = errno != 0 ? errno : EIO;
            result = s3_error_set(error, S3_RESULT_CALLBACK_ERROR,
                                  "bucket callback failed");
        }
    }
    bucket_array_free(&buckets);
    return result;
}
