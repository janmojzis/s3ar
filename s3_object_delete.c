/* SPDX-License-Identifier: MIT-0 */
#include "s3_internal.h"
#include "s3_xml.h"

#include <libxml/chvalid.h>
#include <libxml/xmlwriter.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct batch_entry {
    const struct s3_object_version_ref *target;
    size_t index;
    bool seen;
    bool deleted;
    char *code;
    char *message;
};

static int compare_batch_entries(const void *left, const void *right) {
    const struct s3_object_version_ref *a =
        ((const struct batch_entry *) left)->target;
    const struct s3_object_version_ref *b =
        ((const struct batch_entry *) right)->target;
    int key_order = strcmp(a->key, b->key);
    return key_order != 0 ? key_order : strcmp(a->version_id, b->version_id);
}

static bool xml_value_valid(const char *value) {
    static const int minimum[] = {0, 0, 0x80, 0x800, 0x10000};
    const unsigned char *p = (const unsigned char *) value;
    const unsigned char *end = p + strlen(value);
    while (p < end) {
        int length = end - p < 4 ? (int) (end - p) : 4;
        int character = xmlGetUTF8Char(p, &length);
        if (character < 0 || length <= 0 || length > 4 ||
            character < minimum[length] || !xmlIsCharQ(character))
            return false;
        p += length;
    }
    return true;
}

static bool batch_target_xml_valid(const struct s3_object_version_ref *target) {
    return xml_value_valid(target->key) && xml_value_valid(target->version_id);
}

static bool write_xml_value(xmlTextWriter *writer, const char *value) {
    const char *start = value;
    if (!xml_value_valid(value)) return false;
    for (const char *p = value; *p != '\0'; ++p) {
        if (*p != '\r') continue;
        if (p != start) {
            size_t length = (size_t) (p - start);
            char *chunk = strndup(start, length);
            bool written = chunk != NULL && xmlTextWriterWriteString(
                                                writer, BAD_CAST chunk) >= 0;
            free(chunk);
            if (!written) return false;
        }
        if (xmlTextWriterWriteRaw(writer, BAD_CAST "&#13;") < 0) return false;
        start = p + 1;
    }
    return xmlTextWriterWriteString(writer, BAD_CAST start) >= 0;
}

static char *build_delete_xml(const struct batch_entry *entries, size_t count) {
    xmlBuffer *buffer = xmlBufferCreate();
    xmlTextWriter *writer =
        buffer != NULL ? xmlNewTextWriterMemory(buffer, 0) : NULL;
    char *body = NULL;
    if (writer == NULL ||
        xmlTextWriterStartElement(writer, BAD_CAST "Delete") < 0 ||
        xmlTextWriterWriteAttribute(
            writer, BAD_CAST "xmlns",
            BAD_CAST "http://s3.amazonaws.com/doc/2006-03-01/") < 0)
        goto done;
    for (size_t i = 0; i < count; ++i) {
        if (xmlTextWriterStartElement(writer, BAD_CAST "Object") < 0 ||
            xmlTextWriterStartElement(writer, BAD_CAST "Key") < 0 ||
            !write_xml_value(writer, entries[i].target->key) ||
            xmlTextWriterEndElement(writer) < 0 ||
            xmlTextWriterStartElement(writer, BAD_CAST "VersionId") < 0 ||
            !write_xml_value(writer, entries[i].target->version_id) ||
            xmlTextWriterEndElement(writer) < 0 ||
            xmlTextWriterEndElement(writer) < 0)
            goto done;
    }
    if (xmlTextWriterEndElement(writer) < 0 || xmlTextWriterFlush(writer) < 0)
        goto done;
    body = malloc(xmlBufferLength(buffer) + 1);
    if (body != NULL) {
        memcpy(body, xmlBufferContent(buffer), xmlBufferLength(buffer));
        body[xmlBufferLength(buffer)] = '\0';
    }
done:
    if (writer != NULL) xmlFreeTextWriter(writer);
    if (buffer != NULL) xmlBufferFree(buffer);
    return body;
}

static struct batch_entry *find_batch_entry(struct batch_entry *entries,
                                            size_t count, const char *key,
                                            const char *id) {
    if (id == NULL) {
        struct batch_entry *match = NULL;
        for (size_t i = 0; i < count; ++i) {
            if (strcmp(entries[i].target->key, key) != 0) continue;
            if (match != NULL) return NULL;
            match = &entries[i];
        }
        return match;
    }
    struct s3_object_version_ref target = {.key = (char *) key,
                                           .version_id = id};
    struct batch_entry needle = {.target = &target};
    return bsearch(&needle, entries, count, sizeof(*entries),
                   compare_batch_entries);
}

static char *xml_text(xmlNode *parent, const char *name) {
    xmlNode *node = s3_xml_child(parent, name);
    xmlChar *value = node != NULL ? xmlNodeGetContent(node) : NULL;
    char *copy = value != NULL ? strdup((const char *) value) : NULL;
    xmlFree(value);
    return copy;
}

static enum s3_result
parse_delete_result(struct batch_entry *entries, size_t count, const char *body,
                    size_t size, struct s3_object_delete_result *results,
                    struct s3_error *error) {
    xmlDoc *doc = s3_xml_read(body, size, S3_XML_BODY_LIMIT, "s3-delete.xml");
    xmlNode *root = doc != NULL ? xmlDocGetRootElement(doc) : NULL;
    enum s3_result result = S3_RESULT_PROTOCOL_ERROR;
    if (!s3_xml_name(root, "DeleteResult")) goto done;
    for (xmlNode *node = root->children; node != NULL; node = node->next) {
        bool deleted = s3_xml_name(node, "Deleted");
        char *key, *id;
        struct batch_entry *entry;
        if (!deleted && !s3_xml_name(node, "Error")) continue;
        key = xml_text(node, "Key");
        id = xml_text(node, "VersionId");
        entry = key != NULL ? find_batch_entry(entries, count, key, id) : NULL;
        free(key);
        free(id);
        if (entry == NULL || entry->seen) goto done;
        entry->seen = true;
        entry->deleted = deleted;
        if (!deleted) {
            entry->code = xml_text(node, "Code");
            entry->message = xml_text(node, "Message");
            if (entry->code == NULL || entry->code[0] == '\0' ||
                entry->message == NULL)
                goto done;
        }
    }
    for (size_t i = 0; i < count; ++i)
        if (!entries[i].seen) goto done;
    for (size_t i = 0; i < count; ++i) {
        results[entries[i].index].deleted = entries[i].deleted;
        results[entries[i].index].code = entries[i].code;
        results[entries[i].index].message = entries[i].message;
        entries[i].code = NULL;
        entries[i].message = NULL;
    }
    result = S3_RESULT_OK;
done:
    if (doc != NULL) xmlFreeDoc(doc);
    if (result != S3_RESULT_OK)
        return s3_error_set(error, S3_RESULT_PROTOCOL_ERROR,
                            "invalid DeleteObjects XML");
    return result;
}

void s3_object_delete_results_free(struct s3_object_delete_result *results,
                                   size_t count) {
    if (results == NULL) return;
    for (size_t i = 0; i < count; ++i) {
        free(results[i].code);
        free(results[i].message);
    }
    memset(results, 0, count * sizeof(*results));
}

enum s3_result s3_object_delete_batch(struct s3_client *client,
                                      struct s3_error *error,
                                      const char *bucket,
                                      const struct s3_object_version_ref *items,
                                      size_t count,
                                      struct s3_object_delete_result *results) {
    struct batch_entry entries[S3_DELETE_BATCH_LIMIT] = {0};
    char *request = NULL, *response = NULL;
    size_t response_size = 0;
    size_t xml_count = 0;
    enum s3_result result;
    s3_error_clear(error);
    if (results == NULL || count == 0 || count > S3_DELETE_BATCH_LIMIT)
        return s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                            "invalid DeleteObjects arguments");
    memset(results, 0, count * sizeof(*results));
    if (client == NULL || error == NULL || bucket == NULL || items == NULL)
        return s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                            "invalid DeleteObjects arguments");
    for (size_t i = 0; i < count; ++i) {
        if (!s3_url_key_valid(items[i].key) || items[i].version_id == NULL ||
            items[i].version_id[0] == '\0')
            return s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                                "invalid object version");
        entries[i].target = &items[i];
        entries[i].index = i;
    }
    qsort(entries, count, sizeof(*entries), compare_batch_entries);
    for (size_t i = 1; i < count; ++i)
        if (compare_batch_entries(&entries[i - 1], &entries[i]) == 0)
            return s3_error_set(error, S3_RESULT_PROTOCOL_ERROR,
                                "duplicate version in listing");
    /* XML 1.0 cannot represent every valid S3 key. Keep those targets out of
     * the batch and delete them through the URL-encoded single-object API. */
    for (size_t i = 0; i < count; ++i)
        if (batch_target_xml_valid(entries[i].target))
            entries[xml_count++] = entries[i];
    result = S3_RESULT_OK;
    if (xml_count != 0) {
        request = build_delete_xml(entries, xml_count);
        if (request == NULL) {
            result = s3_error_set(error, S3_RESULT_ERROR,
                                  "cannot build DeleteObjects XML");
            goto done;
        }
        result = s3_request_bucket(client, error, bucket, "delete", "POST",
                                   request, &response, &response_size);
        if (result == S3_RESULT_OK)
            result = parse_delete_result(entries, xml_count, response,
                                         response_size, results, error);
        if (result != S3_RESULT_OK) goto done;
    }
    for (size_t i = 0; i < count; ++i) {
        struct s3_error single_error = {0};
        if (batch_target_xml_valid(&items[i])) continue;
        result = s3_object_delete_version(client, &single_error, bucket,
                                          items[i].key, items[i].version_id);
        if (result != S3_RESULT_OK) {
            *error = single_error;
            goto done;
        }
        /* Preserve the batch response context for any per-item errors. */
        if (xml_count == 0) *error = single_error;
        results[i].deleted = true;
    }
done:
    for (size_t i = 0; i < xml_count; ++i) {
        free(entries[i].code);
        free(entries[i].message);
    }
    if (result != S3_RESULT_OK) s3_object_delete_results_free(results, count);
    free(request);
    free(response);
    return result;
}

enum s3_result s3_object_delete_version(struct s3_client *client,
                                        struct s3_error *error,
                                        const char *bucket, const char *key,
                                        const char *version_id) {
    const struct s3_query_param params[] = {{"versionId", version_id},
                                            {NULL, NULL}};
    char *query = NULL, *url = NULL;
    enum s3_result result;
    s3_error_clear(error);
    if (client == NULL || error == NULL || bucket == NULL ||
        !s3_url_key_valid(key) || version_id == NULL || version_id[0] == '\0')
        return s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                            "invalid object version");
    query = s3_query_build("", params);
    if (query == NULL)
        return s3_error_set(error, S3_RESULT_ERROR, "out of memory");
    result = s3_url_build_object(client, bucket, key, query, &url, error);
    free(query);
    if (result == S3_RESULT_OK)
        result = s3_request_url(client, error, url, "DELETE", NULL, NULL, NULL);
    free(url);
    return result;
}
