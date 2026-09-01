/* SPDX-License-Identifier: MIT-0 */
#include "s3_internal.h"

#include <errno.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { S3_BUCKET_LIST_BODY_LIMIT = 16 * 1024 * 1024 };

struct owned_bucket {
    char *name;
};

struct bucket_array {
    struct owned_bucket *items;
    size_t count;
    size_t capacity;
};

struct bucket_list_context {
    struct s3_response response;
    char *body;
    size_t body_size;
    size_t body_capacity;
    enum s3_result body_error;
};

static xmlNode *child_element(xmlNode *parent, const char *name) {
    for (xmlNode *node = parent->children; node != NULL; node = node->next)
        if (node->type == XML_ELEMENT_NODE &&
            xmlStrcmp(node->name, (const xmlChar *) name) == 0)
            return node;
    return NULL;
}

static void bucket_array_free(struct bucket_array *array) {
    for (size_t i = 0; i < array->count; ++i) free(array->items[i].name);
    free(array->items);
}

static enum s3_result bucket_array_append(struct bucket_array *array,
                                          xmlDoc *doc, xmlNode *node) {
    xmlNode *name_node = child_element(node, "Name");
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
    array->items[array->count].name = s3_strdup((const char *) name);
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

enum s3_result s3_parse_bucket_list(const char *body, size_t size,
                                    s3_bucket_callback callback, void *data,
                                    struct s3_error *error) {
    struct bucket_array buckets = {0};
    xmlDoc *doc;
    xmlNode *root;
    xmlNode *container;
    enum s3_result result = S3_RESULT_OK;
    if (body == NULL || size == 0 || size > S3_BUCKET_LIST_BODY_LIMIT ||
        size > (size_t) INT_MAX || callback == NULL)
        return s3_error_set(error, S3_RESULT_PROTOCOL_ERROR,
                            "invalid ListBuckets response");
    doc = xmlReadMemory(body, (int) size, "s3-buckets.xml", NULL,
                        XML_PARSE_NONET | XML_PARSE_NOERROR |
                            XML_PARSE_NOWARNING | XML_PARSE_NOBLANKS);
    if (doc == NULL || xmlGetIntSubset(doc) != NULL) {
        if (doc != NULL) xmlFreeDoc(doc);
        return s3_error_set(error, S3_RESULT_PROTOCOL_ERROR,
                            "invalid ListBuckets XML");
    }
    root = xmlDocGetRootElement(doc);
    container =
        root != NULL &&
                xmlStrcmp(root->name,
                          (const xmlChar *) "ListAllMyBucketsResult") == 0
            ? child_element(root, "Buckets")
            : NULL;
    if (container == NULL) result = S3_RESULT_PROTOCOL_ERROR;
    for (xmlNode *node = container != NULL ? container->children : NULL;
         result == S3_RESULT_OK && node != NULL; node = node->next) {
        enum s3_result append_result;
        if (node->type != XML_ELEMENT_NODE) continue;
        if (xmlStrcmp(node->name, (const xmlChar *) "Bucket") != 0) continue;
        append_result = bucket_array_append(&buckets, doc, node);
        if (append_result != S3_RESULT_OK) result = append_result;
    }
    xmlFreeDoc(doc);
    if (result != S3_RESULT_OK) {
        bucket_array_free(&buckets);
        return s3_error_set(error, result,
                            result == S3_RESULT_ERROR
                                ? "out of memory"
                                : "invalid ListBuckets XML");
    }
    if (buckets.count > 1)
        qsort(buckets.items, buckets.count, sizeof(*buckets.items),
              compare_buckets);
    for (size_t i = 0; i < buckets.count; ++i) {
        const struct s3_bucket bucket = {
            .name = buckets.items[i].name,
        };
        if (!callback(data, &bucket)) {
            if (error != NULL) error->callback_errno = errno != 0 ? errno : EIO;
            result = s3_error_set(error, S3_RESULT_CALLBACK_ERROR,
                                  "bucket callback failed");
            break;
        }
    }
    bucket_array_free(&buckets);
    return result;
}

static size_t collect_bucket_body(char *buffer, size_t size, size_t count,
                                  void *data) {
    struct bucket_list_context *context = data;
    size_t bytes;
    size_t needed;
    size_t capacity;
    char *body;
    if (size != 0 && count > SIZE_MAX / size) return 0;
    bytes = size * count;
    if (context->response.status < 200 || context->response.status >= 300) {
        size_t room = S3_ERROR_BODY_LIMIT - context->response.error_body_size;
        size_t copy = bytes < room ? bytes : room;
        memcpy(context->response.error_body + context->response.error_body_size,
               buffer, copy);
        context->response.error_body_size += copy;
        context->response.error_body[context->response.error_body_size] = '\0';
        return bytes;
    }
    if (bytes > S3_BUCKET_LIST_BODY_LIMIT - context->body_size) {
        context->body_error = S3_RESULT_PROTOCOL_ERROR;
        return 0;
    }
    needed = context->body_size + bytes + 1;
    if (needed > context->body_capacity) {
        capacity = context->body_capacity == 0 ? 4096 : context->body_capacity;
        while (capacity < needed) {
            if (capacity > (S3_BUCKET_LIST_BODY_LIMIT + 1U) / 2U) {
                capacity = S3_BUCKET_LIST_BODY_LIMIT + 1U;
                break;
            }
            capacity *= 2;
        }
        body = realloc(context->body, capacity);
        if (body == NULL) {
            context->body_error = S3_RESULT_ERROR;
            return 0;
        }
        context->body = body;
        context->body_capacity = capacity;
    }
    memcpy(context->body + context->body_size, buffer, bytes);
    context->body_size += bytes;
    context->body[context->body_size] = '\0';
    return bytes;
}

enum s3_result s3_bucket_list(struct s3_client *client, struct s3_error *error,
                              s3_bucket_callback callback, void *data) {
    struct bucket_list_context context = {0};
    char *url = NULL;
    enum s3_result result;
    if (client == NULL || error == NULL || callback == NULL)
        return s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                            "invalid ListBuckets arguments");
    s3_error_clear(error);
    result = s3_build_service_url(client, &url, error);
    if (result != S3_RESULT_OK) return result;
    for (unsigned attempt = 1; attempt <= client->max_attempts; ++attempt) {
        struct curl_slist *headers = NULL;
        CURLcode code;
        char curl_error[CURL_ERROR_SIZE] = {0};
        bool retryable;
        if (attempt > 1) s3_response_cleanup(&context.response);
        s3_response_reset(&context.response);
        free(context.body);
        context.body = NULL;
        context.body_size = 0;
        context.body_capacity = 0;
        context.body_error = S3_RESULT_OK;
        result = s3_prepare_request(client, url, &headers, error);
        if (result != S3_RESULT_OK) {
            curl_slist_free_all(headers);
            free(context.body);
            free(url);
            return result;
        }
        (void) curl_easy_setopt(client->curl, CURLOPT_HTTPGET, 1L);
        (void) curl_easy_setopt(client->curl, CURLOPT_HEADERFUNCTION,
                                s3_header_callback);
        (void) curl_easy_setopt(client->curl, CURLOPT_HEADERDATA,
                                &context.response);
        (void) curl_easy_setopt(client->curl, CURLOPT_WRITEFUNCTION,
                                collect_bucket_body);
        (void) curl_easy_setopt(client->curl, CURLOPT_WRITEDATA, &context);
        (void) curl_easy_setopt(client->curl, CURLOPT_ERRORBUFFER, curl_error);
        code = curl_easy_perform(client->curl);
        (void) curl_easy_getinfo(client->curl, CURLINFO_RESPONSE_CODE,
                                 &context.response.status);
        curl_slist_free_all(headers);
        s3_error_clear(error);
        error->attempts = attempt;
        error->http_status = context.response.status;
        s3_parse_error_xml(context.response.error_body,
                           context.response.error_body_size, error);
        if (context.body_error != S3_RESULT_OK) {
            result = s3_error_set(error, context.body_error,
                                  context.body_error == S3_RESULT_ERROR
                                      ? "out of memory"
                                      : "ListBuckets response is too large");
            break;
        }
        if (code == CURLE_OK && context.response.status >= 200 &&
            context.response.status < 300) {
            result = s3_parse_bucket_list(context.body, context.body_size,
                                          callback, data, error);
            break;
        }
        retryable =
            s3_is_retryable(code, context.response.status, error->s3_code);
        if (!retryable) {
            result =
                s3_result_from_response(code, &context.response, false, error);
            if (code != CURLE_OK && curl_error[0] != '\0')
                (void) snprintf(error->message, sizeof(error->message), "%s",
                                curl_error);
            break;
        }
        if (attempt == client->max_attempts) {
            result = s3_error_set(error, S3_RESULT_RETRY_EXHAUSTED,
                                  "S3 ListBuckets retry limit exhausted");
            break;
        }
        s3_retry_delay(attempt - 1);
    }
    free(context.body);
    free(url);
    s3_response_cleanup(&context.response);
    return result;
}

static enum s3_result bucket_request(struct s3_client *client,
                                     struct s3_error *error, const char *bucket,
                                     const char *query, const char *method,
                                     const char *request_body,
                                     char **response_body,
                                     size_t *response_size) {
    struct bucket_list_context context = {0};
    char *url = NULL;
    enum s3_result result;
    if (response_body != NULL) *response_body = NULL;
    if (response_size != NULL) *response_size = 0;
    result = s3_build_bucket_url(client, bucket, query, &url, error);
    if (result != S3_RESULT_OK) return result;
    for (unsigned attempt = 1; attempt <= client->max_attempts; ++attempt) {
        struct curl_slist *headers = NULL;
        CURLcode code;
        char curl_error[CURL_ERROR_SIZE] = {0};
        if (attempt > 1) s3_response_cleanup(&context.response);
        s3_response_reset(&context.response);
        free(context.body);
        context.body = NULL;
        context.body_size = context.body_capacity = 0;
        context.body_error = S3_RESULT_OK;
        result = s3_prepare_request(client, url, &headers, error);
        if (result != S3_RESULT_OK) break;
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
            (void) s3_add_header(&headers, "Content-Length", "0");
            (void) curl_easy_setopt(client->curl, CURLOPT_HTTPHEADER, headers);
        }
        (void) curl_easy_setopt(client->curl, CURLOPT_HEADERFUNCTION,
                                s3_header_callback);
        (void) curl_easy_setopt(client->curl, CURLOPT_HEADERDATA,
                                &context.response);
        (void) curl_easy_setopt(client->curl, CURLOPT_WRITEFUNCTION,
                                collect_bucket_body);
        (void) curl_easy_setopt(client->curl, CURLOPT_WRITEDATA, &context);
        (void) curl_easy_setopt(client->curl, CURLOPT_ERRORBUFFER, curl_error);
        code = curl_easy_perform(client->curl);
        (void) curl_easy_getinfo(client->curl, CURLINFO_RESPONSE_CODE,
                                 &context.response.status);
        curl_slist_free_all(headers);
        s3_error_clear(error);
        error->attempts = attempt;
        error->http_status = context.response.status;
        s3_parse_error_xml(context.response.error_body,
                           context.response.error_body_size, error);
        if (context.body_error != S3_RESULT_OK) {
            result = s3_error_set(error, context.body_error,
                                  "S3 XML response is too large");
            break;
        }
        if (code == CURLE_OK && context.response.status >= 200 &&
            context.response.status < 300) {
            result = S3_RESULT_OK;
            break;
        }
        if (!s3_is_retryable(code, context.response.status, error->s3_code)) {
            result =
                s3_result_from_response(code, &context.response, false, error);
            if (code != CURLE_OK && curl_error[0] != '\0')
                (void) snprintf(error->message, sizeof(error->message), "%s",
                                curl_error);
            break;
        }
        if (attempt == client->max_attempts) {
            result = s3_error_set(error, S3_RESULT_RETRY_EXHAUSTED,
                                  "S3 bucket request retry limit exhausted");
            break;
        }
        s3_retry_delay(attempt - 1);
    }
    free(url);
    s3_response_cleanup(&context.response);
    if (result == S3_RESULT_OK && response_body != NULL) {
        *response_body = context.body;
        if (response_size != NULL) *response_size = context.body_size;
        context.body = NULL;
    }
    free(context.body);
    return result;
}

enum s3_result s3_bucket_head(struct s3_client *client, struct s3_error *error,
                              const char *bucket) {
    if (client == NULL || error == NULL)
        return s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                            "invalid HeadBucket arguments");
    s3_error_clear(error);
    return bucket_request(client, error, bucket, NULL, "HEAD", NULL, NULL,
                          NULL);
}

enum s3_result s3_bucket_create(struct s3_client *client,
                                struct s3_error *error, const char *bucket) {
    char body[512];
    const char *content = NULL;
    enum s3_result result;
    if (client == NULL || error == NULL)
        return s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                            "invalid CreateBucket arguments");
    if (strcmp(client->region, "us-east-1") != 0) {
        int length = snprintf(body, sizeof(body),
                              "<CreateBucketConfiguration "
                              "xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/"
                              "\"><LocationConstraint>%s</LocationConstraint></"
                              "CreateBucketConfiguration>",
                              client->region);
        if (length < 0 || (size_t) length >= sizeof(body))
            return s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                                "S3 region is too long");
        content = body;
    }
    result =
        bucket_request(client, error, bucket, NULL, "PUT", content, NULL, NULL);
    if (result != S3_RESULT_OK &&
        strcmp(error->s3_code, "BucketAlreadyOwnedByYou") == 0) {
        s3_error_clear(error);
        return S3_RESULT_OK;
    }
    return result;
}

enum s3_result s3_bucket_ensure(struct s3_client *client,
                                struct s3_error *error, const char *bucket) {
    enum s3_result result = s3_bucket_head(client, error, bucket);
    if (result == S3_RESULT_OK) return result;
    if (result != S3_RESULT_NOT_FOUND) return result;
    return s3_bucket_create(client, error, bucket);
}

static bool acl_permission(const char *permission, bool *read, bool *write) {
    if (strcmp(permission, "READ") == 0)
        *read = true;
    else if (strcmp(permission, "WRITE") == 0)
        *write = true;
    else if (strcmp(permission, "FULL_CONTROL") == 0)
        *read = *write = true;
    else
        return false;
    return true;
}

static enum s3_result parse_acl(const char *body, size_t size, char *summary,
                                size_t capacity, struct s3_error *error) {
    xmlDoc *doc = NULL;
    xmlNode *root;
    xmlChar *owner_id = NULL;
    bool public_read = false, public_write = false;
    bool auth_read = false, auth_write = false, custom = false;
    if (body == NULL || size == 0 || size > (size_t) INT_MAX ||
        strstr(body, "<!DOCTYPE") != NULL)
        goto invalid;
    doc = xmlReadMemory(body, (int) size, "s3-acl.xml", NULL,
                        XML_PARSE_NONET | XML_PARSE_NOERROR |
                            XML_PARSE_NOWARNING | XML_PARSE_NOBLANKS);
    root = doc != NULL ? xmlDocGetRootElement(doc) : NULL;
    if (root == NULL ||
        xmlStrcmp(root->name, (const xmlChar *) "AccessControlPolicy") != 0)
        goto invalid;
    {
        xmlNode *owner = child_element(root, "Owner");
        xmlNode *id = owner != NULL ? child_element(owner, "ID") : NULL;
        if (id != NULL) owner_id = xmlNodeGetContent(id);
    }
    for (xmlNode *node = root; node != NULL;) {
        if (node->type == XML_ELEMENT_NODE &&
            xmlStrcmp(node->name, (const xmlChar *) "Grant") == 0) {
            xmlNode *grantee = child_element(node, "Grantee");
            xmlNode *permission_node = child_element(node, "Permission");
            xmlNode *uri_node =
                grantee != NULL ? child_element(grantee, "URI") : NULL;
            xmlNode *id_node =
                grantee != NULL ? child_element(grantee, "ID") : NULL;
            xmlChar *permission = permission_node != NULL
                                      ? xmlNodeGetContent(permission_node)
                                      : NULL;
            xmlChar *uri =
                uri_node != NULL ? xmlNodeGetContent(uri_node) : NULL;
            xmlChar *id = id_node != NULL ? xmlNodeGetContent(id_node) : NULL;
            if (permission == NULL)
                custom = true;
            else if (uri != NULL &&
                     strstr((const char *) uri, "/AllUsers") != NULL) {
                if (!acl_permission((const char *) permission, &public_read,
                                    &public_write))
                    custom = true;
            }
            else if (uri != NULL && strstr((const char *) uri,
                                           "/AuthenticatedUsers") != NULL) {
                if (!acl_permission((const char *) permission, &auth_read,
                                    &auth_write))
                    custom = true;
            }
            else if (id == NULL || owner_id == NULL ||
                     xmlStrcmp(id, owner_id) != 0 ||
                     strcmp((const char *) permission, "FULL_CONTROL") != 0)
                custom = true;
            xmlFree(permission);
            xmlFree(uri);
            xmlFree(id);
        }
        if (node->children != NULL)
            node = node->children;
        else {
            while (node != root && node->next == NULL) node = node->parent;
            node = node == root ? NULL : node->next;
        }
    }
    {
        const char *values[5];
        size_t count = 0, used = 0;
        if (public_read) values[count++] = "public-read";
        if (public_write) values[count++] = "public-write";
        if (auth_read) values[count++] = "authenticated-read";
        if (auth_write) values[count++] = "authenticated-write";
        if (custom) values[count++] = "custom";
        if (count == 0) values[count++] = "private";
        for (size_t i = 0; i < count; ++i) {
            int n = snprintf(summary + used, capacity - used, "%s%s",
                             i == 0 ? "" : ",", values[i]);
            if (n < 0 || (size_t) n >= capacity - used) goto invalid;
            used += (size_t) n;
        }
    }
    xmlFree(owner_id);
    xmlFreeDoc(doc);
    return S3_RESULT_OK;
invalid:
    xmlFree(owner_id);
    if (doc != NULL) xmlFreeDoc(doc);
    return s3_error_set(error, S3_RESULT_PROTOCOL_ERROR,
                        "invalid GetBucketAcl XML");
}

enum s3_result s3_bucket_acl(struct s3_client *client, struct s3_error *error,
                             const char *bucket, s3_bucket_callback callback,
                             void *data) {
    char *body = NULL;
    size_t size = 0;
    char summary[128];
    enum s3_result result;
    if (client == NULL || error == NULL || callback == NULL)
        return s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                            "invalid GetBucketAcl arguments");
    result =
        bucket_request(client, error, bucket, "acl", "GET", NULL, &body, &size);
    if (result != S3_RESULT_OK) return result;
    result = parse_acl(body, size, summary, sizeof(summary), error);
    free(body);
    if (result == S3_RESULT_OK) {
        const struct s3_bucket value = {.name = bucket, .acl = summary};
        if (!callback(data, &value)) {
            error->callback_errno = errno != 0 ? errno : EIO;
            return s3_error_set(error, S3_RESULT_CALLBACK_ERROR,
                                "bucket callback failed");
        }
    }
    return result;
}
