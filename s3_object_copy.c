/* SPDX-License-Identifier: MIT-0 */
#include "s3_upload.h"
#include "s3_xml.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static enum s3_result encode_copy_tags(const char *body, size_t size,
                                       char **tagging, struct s3_error *error) {
    xmlDoc *doc =
        s3_xml_read(body, size, S3_UPLOAD_RESPONSE_LIMIT, "s3-tags.xml");
    xmlNode *root = doc != NULL ? xmlDocGetRootElement(doc) : NULL;
    xmlNode *set = s3_xml_child(root, "TagSet");
    xmlBuffer *buffer = NULL;
    enum s3_result result = S3_RESULT_PROTOCOL_ERROR;
    unsigned count = 0;
    *tagging = NULL;
    if (!s3_xml_name(root, "Tagging") || set == NULL) goto done;
    buffer = xmlBufferCreate();
    if (buffer == NULL) {
        result = S3_RESULT_ERROR;
        goto done;
    }
    for (xmlNode *tag = set->children; tag != NULL; tag = tag->next) {
        xmlNode *key_node, *value_node;
        xmlChar *key, *value;
        char *encoded_key, *encoded_value;
        bool appended;
        if (tag->type != XML_ELEMENT_NODE) continue;
        if (!s3_xml_name(tag, "Tag") || ++count > 10) goto done;
        key_node = s3_xml_child(tag, "Key");
        value_node = s3_xml_child(tag, "Value");
        if (key_node == NULL || value_node == NULL) goto done;
        key = xmlNodeGetContent(key_node);
        value = xmlNodeGetContent(value_node);
        if (key == NULL || value == NULL || key[0] == '\0') {
            xmlFree(key);
            xmlFree(value);
            goto done;
        }
        encoded_key = s3_uri_encode_alloc((const char *) key, false);
        encoded_value = s3_uri_encode_alloc((const char *) value, false);
        xmlFree(key);
        xmlFree(value);
        appended = encoded_key != NULL && encoded_value != NULL &&
                   (count == 1 || xmlBufferCat(buffer, BAD_CAST "&") == 0) &&
                   xmlBufferCat(buffer, BAD_CAST encoded_key) == 0 &&
                   xmlBufferCat(buffer, BAD_CAST "=") == 0 &&
                   xmlBufferCat(buffer, BAD_CAST encoded_value) == 0;
        free(encoded_key);
        free(encoded_value);
        if (!appended) {
            result = S3_RESULT_ERROR;
            goto done;
        }
    }
    *tagging = s3_memory_strdup((const char *) xmlBufferContent(buffer));
    result = *tagging != NULL ? S3_RESULT_OK : S3_RESULT_ERROR;
done:
    if (buffer != NULL) xmlBufferFree(buffer);
    if (doc != NULL) xmlFreeDoc(doc);
    if (result != S3_RESULT_OK)
        return s3_error_set(error, result,
                            result == S3_RESULT_ERROR
                                ? "cannot prepare object tags"
                                : "invalid GetObjectTagging XML");
    return result;
}

enum s3_result s3_object_copy(struct s3_client *client, struct s3_error *error,
                              const char *source_bucket, const char *source_key,
                              const char *destination_bucket,
                              const char *destination_key, size_t part_size) {
    struct s3_object_properties properties = {0};
    struct s3_memory_response response = {0};
    struct s3_upload_copy_request copy = {0};
    char *encoded_bucket = NULL, *encoded_key = NULL;
    char *source = NULL, *url = NULL;
    char *tagging = NULL, *tag_body = NULL;
    char *encoded_upload_id = NULL;
    char **etags = NULL;
    size_t part_count = 0;
    bool completion_uncertain = false;
    enum s3_result result;

    s3_error_clear(error);
    if (client == NULL || error == NULL ||
        part_size < S3_UPLOAD_MIN_PART_SIZE ||
        (uint64_t) part_size > S3_MULTIPART_MAX_PART_SIZE ||
        source_bucket == NULL || source_key == NULL ||
        destination_bucket == NULL || destination_key == NULL)
        return s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                            "invalid CopyObject arguments");
    result =
        s3_url_validate_object_name(client, source_bucket, source_key, error);
    if (result != S3_RESULT_OK) return result;
    result = s3_url_validate_object_name(client, destination_bucket,
                                         destination_key, error);
    if (result != S3_RESULT_OK) return result;
    if (strcmp(source_bucket, destination_bucket) == 0 &&
        strcmp(source_key, destination_key) == 0)
        return s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                            "source and destination are identical");

    result =
        s3_object_head(client, error, &properties, source_bucket, source_key);
    if (result != S3_RESULT_OK) goto done;
    if (properties.etag[0] == '\0') {
        result = s3_error_set(error, S3_RESULT_PROTOCOL_ERROR,
                              "source HEAD response lacks ETag");
        goto done;
    }
    encoded_bucket = s3_uri_encode_alloc(source_bucket, false);
    encoded_key = s3_uri_encode_alloc(source_key, true);
    if (encoded_bucket == NULL || encoded_key == NULL) {
        result = s3_error_set(error, S3_RESULT_ERROR, "out of memory");
        goto done;
    }
    source = malloc(strlen(encoded_bucket) + strlen(encoded_key) + 3);
    if (source == NULL) {
        result = s3_error_set(error, S3_RESULT_ERROR, "out of memory");
        goto done;
    }
    (void) sprintf(source, "/%s/%s", encoded_bucket, encoded_key);
    copy.source = source;
    copy.etag = properties.etag;

    /* part_size is bounded by the 5 GiB CopyObject limit above. COPY is the
     * default metadata/tagging directive, so the server preserves both. */
    if (properties.size <= part_size) {
        copy.result_root = "CopyObjectResult";
        result = s3_url_build_object(client, destination_bucket,
                                     destination_key, NULL, &url, error);
        if (result == S3_RESULT_OK)
            result = s3_upload_request_ex(
                client, error, url, "PUT", (const unsigned char *) "", 0, NULL,
                S3_UPLOAD_RETRY, NULL, &response, &copy, NULL);
        goto done;
    }

    uint64_t count =
        properties.size / part_size + (properties.size % part_size != 0);
    if (count > 10000) {
        result = s3_error_set(
            error, S3_RESULT_CONFIGURATION_ERROR,
            "object exceeds 10000 parts; increase --multipart-size");
        goto done;
    }
    part_count = (size_t) count;

    result = s3_url_build_object(client, source_bucket, source_key, "tagging",
                                 &url, error);
    if (result != S3_RESULT_OK) goto done;
    {
        size_t tag_size = 0;
        result = s3_request_url(client, error, url, "GET", NULL, &tag_body,
                                &tag_size);
        if (result != S3_RESULT_OK) goto done;
        result = encode_copy_tags(tag_body, tag_size, &tagging, error);
        if (result != S3_RESULT_OK) goto done;
    }
    free(url);
    url = NULL;

    etags = calloc(part_count, sizeof(*etags));
    if (etags == NULL) {
        result = s3_error_set(error, S3_RESULT_ERROR, "out of memory");
        goto done;
    }
    result =
        s3_upload_initiate(client, error, destination_bucket, destination_key,
                           &properties, tagging, &encoded_upload_id);
    if (result != S3_RESULT_OK) goto done;
    copy.result_root = "CopyPartResult";
    for (size_t part = 0; part < part_count; ++part) {
        uint64_t first = (uint64_t) part * part_size;
        uint64_t last = properties.size - first < part_size
                            ? properties.size - 1
                            : first + part_size - 1;
        char range[80];
        (void) snprintf(range, sizeof(range), "bytes=%" PRIu64 "-%" PRIu64,
                        first, last);
        copy.range = range;
        result =
            s3_upload_part(client, error, destination_bucket, destination_key,
                           encoded_upload_id, part + 1,
                           (const unsigned char *) "", 0, &copy, &etags[part]);
        if (result != S3_RESULT_OK) goto done;
    }
    result = s3_upload_complete(client, error, destination_bucket,
                                destination_key, encoded_upload_id, etags,
                                part_count, &completion_uncertain);

done:
    if (result == S3_RESULT_PRECONDITION_FAILED)
        (void) snprintf(error->message, sizeof(error->message),
                        "source object changed during copy");
    s3_upload_cleanup_failed(client, error, destination_bucket, destination_key,
                             encoded_upload_id, result, completion_uncertain);
    for (size_t i = 0; i < part_count; ++i)
        free(etags != NULL ? etags[i] : NULL);
    free(etags);
    free(encoded_bucket);
    free(encoded_key);
    free(tagging);
    free(tag_body);
    free(source);
    free(encoded_upload_id);
    free(url);
    s3_response_memory_cleanup(&response);
    s3_object_properties_free(&properties);
    return result;
}
