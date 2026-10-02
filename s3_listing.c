/* SPDX-License-Identifier: MIT-0 */
#include "s3_internal.h"
#include "s3_xml.h"

#include <stdlib.h>
#include <string.h>

void s3_listing_page_free(struct s3_listing_page *page) {
    if (page == NULL) return;
    for (size_t i = 0; i < page->count; ++i) {
        free(page->items[i].key);
        free(page->items[i].id);
    }
    free(page->items);
    free(page->next_key);
    free(page->next_id);
    memset(page, 0, sizeof(*page));
}

static char *copy_key(const char *text, bool encoded) {
    if (text == NULL) return NULL;
    char *key = NULL;
    if (encoded)
        (void) s3_uri_decode_alloc(text, &key);
    else
        key = strdup(text);
    if (key != NULL && !s3_url_key_valid(key)) {
        free(key);
        return NULL;
    }
    return key;
}

static bool append_target(struct s3_listing_page *page, size_t *capacity,
                          char *key, char *id, bool delete_marker) {
    struct s3_listing_item *items;
    size_t next_capacity;
    if (page->count == *capacity) {
        next_capacity = *capacity == 0 ? 16 : *capacity * 2;
        if (next_capacity < *capacity ||
            next_capacity > SIZE_MAX / sizeof(*page->items))
            return false;
        items = realloc(page->items, next_capacity * sizeof(*items));
        if (items == NULL) return false;
        page->items = items;
        *capacity = next_capacity;
    }
    page->items[page->count++] = (struct s3_listing_item) {
        .key = key, .id = id, .delete_marker = delete_marker};
    return true;
}

static enum s3_result parse_page(const char *body, size_t size, bool uploads,
                                 struct s3_listing_page *page,
                                 struct s3_error *error) {
    xmlDoc *doc = s3_xml_read(body, size, S3_XML_BODY_LIMIT, "s3-delete.xml");
    xmlNode *root = doc != NULL ? xmlDocGetRootElement(doc) : NULL;
    char *truncated = NULL, *encoding = NULL;
    enum s3_result result = S3_RESULT_PROTOCOL_ERROR;
    size_t capacity = 0;
    if (!s3_xml_name(root, uploads ? "ListMultipartUploadsResult"
                                   : "ListVersionsResult"))
        goto done;
    truncated = s3_xml_text(root, "IsTruncated");
    encoding = s3_xml_text(root, "EncodingType");
    if (truncated == NULL ||
        (encoding != NULL && strcmp(encoding, "url") != 0) ||
        (strcmp(truncated, "true") != 0 && strcmp(truncated, "false") != 0))
        goto done;
    page->truncated = strcmp(truncated, "true") == 0;
    for (xmlNode *node = root->children; node != NULL; node = node->next) {
        char *encoded, *key, *id;
        if (!s3_xml_name(node, uploads ? "Upload" : "Version") &&
            (uploads || !s3_xml_name(node, "DeleteMarker")))
            continue;
        encoded = s3_xml_text(node, "Key");
        id = s3_xml_text(node, uploads ? "UploadId" : "VersionId");
        key = copy_key(encoded, encoding != NULL);
        free(encoded);
        if (key == NULL || id == NULL || id[0] == '\0' ||
            page->count >= S3_DELETE_BATCH_LIMIT ||
            !append_target(page, &capacity, key, id,
                           !uploads && s3_xml_name(node, "DeleteMarker"))) {
            free(key);
            free(id);
            goto done;
        }
    }
    if (page->truncated) {
        char *encoded = s3_xml_text(root, "NextKeyMarker");
        page->next_key = copy_key(encoded, encoding != NULL);
        free(encoded);
        page->next_id = s3_xml_text(root, uploads ? "NextUploadIdMarker"
                                                  : "NextVersionIdMarker");
        if (page->next_key == NULL || page->next_key[0] == '\0') goto done;
    }
    result = S3_RESULT_OK;
done:
    free(truncated);
    free(encoding);
    if (doc != NULL) xmlFreeDoc(doc);
    if (result != S3_RESULT_OK) {
        s3_listing_page_free(page);
        return s3_error_set(error, S3_RESULT_PROTOCOL_ERROR,
                            uploads ? "invalid ListMultipartUploads XML"
                                    : "invalid ListObjectVersions XML");
    }
    return result;
}

static char *build_query(bool uploads, const char *prefix, const char *key,
                         const char *id) {
    const struct s3_query_param params[] = {
        {"prefix", prefix},
        {"key-marker", key},
        {uploads ? "upload-id-marker" : "version-id-marker", id},
        {NULL, NULL},
    };
    return s3_query_build(uploads ? "uploads&max-uploads=1000&encoding-type=url"
                                  : "versions&max-keys=1000&encoding-type=url",
                          params);
}

static enum s3_result fetch_page(struct s3_client *client,
                                 struct s3_error *error, const char *bucket,
                                 bool uploads, const char *prefix,
                                 const char *key_marker, const char *id_marker,
                                 struct s3_listing_page *page) {
    char *query;
    char *body = NULL;
    size_t size = 0;
    enum s3_result result;
    s3_error_clear(error);
    if (page != NULL) memset(page, 0, sizeof(*page));
    if (client == NULL || error == NULL || bucket == NULL || page == NULL ||
        (prefix != NULL && !s3_url_key_valid(prefix)) ||
        (key_marker != NULL && !s3_url_key_valid(key_marker))) {
        return s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                            "invalid listing arguments");
    }
    query = build_query(uploads, prefix, key_marker, id_marker);
    if (query == NULL)
        return s3_error_set(error, S3_RESULT_ERROR, "out of memory");
    result = s3_request_bucket(client, error, bucket, query, "GET", NULL, &body,
                               &size);
    free(query);
    if (result == S3_RESULT_OK)
        result = parse_page(body, size, uploads, page, error);
    free(body);
    return result;
}

enum s3_result s3_listing_versions_page(struct s3_client *client,
                                        struct s3_error *error,
                                        const char *bucket, const char *prefix,
                                        const char *key_marker,
                                        const char *version_marker,
                                        struct s3_listing_page *page) {
    return fetch_page(client, error, bucket, false, prefix, key_marker,
                      version_marker, page);
}

enum s3_result s3_listing_uploads_page(struct s3_client *client,
                                       struct s3_error *error,
                                       const char *bucket, const char *prefix,
                                       const char *key_marker,
                                       const char *upload_marker,
                                       struct s3_listing_page *page) {
    return fetch_page(client, error, bucket, true, prefix, key_marker,
                      upload_marker, page);
}
