/* SPDX-License-Identifier: MIT-0 */
#include "s3_upload.h"
#include "s3_xml.h"

#include <libxml/xmlwriter.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct s3_object_properties xml_properties = {
    .content_type = "application/xml",
};

static bool xml_has_root(const char *body, size_t size, const char *name);

struct memory_reader {
    const unsigned char *data;
    size_t size;
    size_t offset;
};

static size_t read_memory(char *buffer, size_t size, size_t count, void *data) {
    struct memory_reader *reader = data;
    size_t capacity, available, amount;
    if (size != 0 && count > SIZE_MAX / size) return CURL_READFUNC_ABORT;
    capacity = size * count;
    available = reader->size - reader->offset;
    amount = capacity < available ? capacity : available;
    if (amount != 0) memcpy(buffer, reader->data + reader->offset, amount);
    reader->offset += amount;
    return amount;
}

static enum s3_result
add_properties_headers(struct curl_slist **headers,
                       const struct s3_object_properties *properties,
                       struct s3_error *error) {
    if (properties == NULL) return S3_RESULT_OK;
    if ((properties->content_type != NULL &&
         properties->content_type[0] != '\0' &&
         !s3_headers_add(headers, "Content-Type", properties->content_type)) ||
        (properties->content_encoding != NULL &&
         properties->content_encoding[0] != '\0' &&
         !s3_headers_add(headers, "Content-Encoding",
                         properties->content_encoding)) ||
        (properties->cache_control != NULL &&
         properties->cache_control[0] != '\0' &&
         !s3_headers_add(headers, "Cache-Control", properties->cache_control)))
        return s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                            "invalid object properties");
    if ((properties->content_disposition != NULL &&
         !s3_headers_add(headers, "Content-Disposition",
                         properties->content_disposition)) ||
        (properties->content_language != NULL &&
         !s3_headers_add(headers, "Content-Language",
                         properties->content_language)) ||
        (properties->expires != NULL &&
         !s3_headers_add(headers, "Expires", properties->expires)))
        return s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                            "invalid object properties");
    if (properties->metadata_count > S3_METADATA_LIMIT ||
        (properties->metadata_count != 0 && properties->metadata == NULL))
        return s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                            "invalid object metadata");
    for (size_t i = 0; i < properties->metadata_count; ++i) {
        const char *name = properties->metadata[i].name;
        const char *value = properties->metadata[i].value;
        char *header_name;
        size_t length;
        if (name == NULL || name[0] == '\0' || value == NULL ||
            strpbrk(name, "\r\n:") != NULL || strpbrk(value, "\r\n") != NULL)
            return s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                                "invalid object metadata");
        if (strlen(name) > SIZE_MAX - 12)
            return s3_error_set(error, S3_RESULT_ERROR, "out of memory");
        length = strlen(name) + 12;
        header_name = malloc(length);
        if (header_name == NULL)
            return s3_error_set(error, S3_RESULT_ERROR, "out of memory");
        (void) snprintf(header_name, length, "x-amz-meta-%s", name);
        if (!s3_headers_add(headers, header_name, value)) {
            free(header_name);
            return s3_error_set(error, S3_RESULT_ERROR,
                                "cannot add object metadata");
        }
        free(header_name);
    }
    return S3_RESULT_OK;
}

enum s3_result s3_upload_request_ex(
    struct s3_client *client, struct s3_error *error, const char *url,
    const char *method, const unsigned char *body, size_t body_size,
    const struct s3_object_properties *properties,
    enum s3_upload_retry_mode retry_mode, bool *completion_uncertain,
    struct s3_memory_response *output,
    const struct s3_upload_copy_request *copy, const char *tagging) {
    enum s3_result result = S3_RESULT_ERROR;
    unsigned attempts = retry_mode == S3_UPLOAD_ONCE ? 1 : client->max_attempts;
    if (completion_uncertain != NULL) *completion_uncertain = false;
    for (unsigned attempt = 1; attempt <= attempts; ++attempt) {
        struct curl_slist *headers = NULL;
        struct memory_reader reader = {.data = body, .size = body_size};
        CURLcode code;
        bool completion_succeeded = false;
        char curl_error[CURL_ERROR_SIZE] = {0};
        char length[32];
        s3_response_memory_reset(output, S3_UPLOAD_RESPONSE_LIMIT);
        result = add_properties_headers(&headers, properties, error);
        if (result != S3_RESULT_OK) {
            curl_slist_free_all(headers);
            return result;
        }
        if (tagging != NULL && tagging[0] != '\0' &&
            !s3_headers_add(&headers, "x-amz-tagging", tagging)) {
            curl_slist_free_all(headers);
            return s3_error_set(error, S3_RESULT_ERROR,
                                "cannot prepare object tags");
        }
        if (copy != NULL &&
            (!s3_headers_add(&headers, "x-amz-copy-source", copy->source) ||
             (copy->etag != NULL &&
              !s3_headers_add(&headers, "x-amz-copy-source-if-match",
                              copy->etag)) ||
             (copy->range != NULL &&
              !s3_headers_add(&headers, "x-amz-copy-source-range",
                              copy->range)))) {
            curl_slist_free_all(headers);
            return s3_error_set(error, S3_RESULT_ERROR,
                                "cannot prepare copy headers");
        }
        (void) snprintf(length, sizeof(length), "%zu", body_size);
        if (!s3_headers_add(&headers, "Content-Length", length)) {
            curl_slist_free_all(headers);
            return s3_error_set(error, S3_RESULT_ERROR, "out of memory");
        }
        result = s3_request_prepare(client, url, &headers, error);
        if (result != S3_RESULT_OK) {
            curl_slist_free_all(headers);
            return result;
        }
        if (strcmp(method, "PUT") == 0) {
            (void) curl_easy_setopt(client->curl, CURLOPT_UPLOAD, 1L);
            (void) curl_easy_setopt(client->curl, CURLOPT_READFUNCTION,
                                    read_memory);
            (void) curl_easy_setopt(client->curl, CURLOPT_READDATA, &reader);
            (void) curl_easy_setopt(client->curl, CURLOPT_INFILESIZE_LARGE,
                                    (curl_off_t) body_size);
        }
        else {
            (void) curl_easy_setopt(client->curl, CURLOPT_CUSTOMREQUEST,
                                    method);
            (void) curl_easy_setopt(client->curl, CURLOPT_POSTFIELDS, body);
            (void) curl_easy_setopt(client->curl, CURLOPT_POSTFIELDSIZE_LARGE,
                                    (curl_off_t) body_size);
        }
        code = s3_request_perform(client, attempt, attempts, &output->response,
                                  s3_response_memory_collect, output,
                                  curl_error, error);
        curl_slist_free_all(headers);
        if ((retry_mode == S3_UPLOAD_COMPLETE || copy != NULL) &&
            code == CURLE_OK && output->response.status >= 200 &&
            output->response.status < 300 &&
            output->body_error == S3_RESULT_OK) {
            completion_succeeded =
                xml_has_root(output->body, output->size,
                             copy != NULL ? copy->result_root
                                          : "CompleteMultipartUploadResult");
            if (!completion_succeeded)
                s3_error_parse_xml(output->body, output->size, error);
        }
        s3_trace_perform_end(client, attempt, attempts, code, &output->response,
                             error);
        result = s3_response_check_headers(
            &output->response,
            "invalid or oversized S3 upload response headers", error);
        if (result != S3_RESULT_OK) {
            if (completion_uncertain != NULL) *completion_uncertain = true;
            break;
        }
        if (output->body_error != S3_RESULT_OK) {
            if (completion_uncertain != NULL) *completion_uncertain = true;
            result = s3_error_set(error, output->body_error,
                                  output->body_error == S3_RESULT_ERROR
                                      ? "out of memory"
                                      : "S3 upload response is too large");
            break;
        }
        if (code == CURLE_OK && output->response.status >= 200 &&
            output->response.status < 300) {
            if ((retry_mode != S3_UPLOAD_COMPLETE && copy == NULL) ||
                completion_succeeded)
                return S3_RESULT_OK;
            /* S3 may return an Error XML document with HTTP 200. */
            if (error->s3_code[0] == '\0') {
                if (completion_uncertain != NULL) *completion_uncertain = true;
                result = s3_error_set(
                    error, S3_RESULT_PROTOCOL_ERROR,
                    copy != NULL ? "invalid S3 copy response XML"
                                 : "invalid CompleteMultipartUpload XML");
                break;
            }
            if (!s3_retry_allowed(code, output->response.status,
                                  error->s3_code)) {
                if (error->message[0] != '\0') {
                    error->result = S3_RESULT_PROTOCOL_ERROR;
                    result = S3_RESULT_PROTOCOL_ERROR;
                }
                else
                    result = s3_error_set(
                        error, S3_RESULT_PROTOCOL_ERROR,
                        copy != NULL ? "S3 copy failed"
                                     : "S3 multipart completion failed");
                break;
            }
        }
        else if (completion_uncertain != NULL && code != CURLE_OK) {
            /* A lost response may follow a successful completion. */
            *completion_uncertain = true;
        }
        if (!s3_retry_allowed(code, output->response.status, error->s3_code)) {
            result =
                s3_request_result(code, &output->response, curl_error, error);
            break;
        }
        if (attempt == attempts) {
            result = s3_error_set(error, S3_RESULT_RETRY_EXHAUSTED,
                                  "S3 upload retry limit exhausted");
            break;
        }
        s3_retry_delay(client, attempt, attempts, code, &output->response,
                       error);
    }
    return result;
}

enum s3_result s3_upload_request(struct s3_client *client,
                                 struct s3_error *error, const char *url,
                                 const char *method, const unsigned char *body,
                                 size_t body_size,
                                 const struct s3_object_properties *properties,
                                 enum s3_upload_retry_mode retry_mode,
                                 bool *completion_uncertain,
                                 struct s3_memory_response *output) {
    return s3_upload_request_ex(client, error, url, method, body, body_size,
                                properties, retry_mode, completion_uncertain,
                                output, NULL, NULL);
}

static char *xml_value(const char *body, size_t size, const char *root_name,
                       const char *element) {
    xmlDoc *doc =
        s3_xml_read(body, size, S3_UPLOAD_RESPONSE_LIMIT, "s3-upload.xml");
    xmlNode *root = doc != NULL ? xmlDocGetRootElement(doc) : NULL;
    char *value =
        s3_xml_name(root, root_name) ? s3_xml_text(root, element) : NULL;
    if (value != NULL && value[0] == '\0') {
        free(value);
        value = NULL;
    }
    if (doc != NULL) xmlFreeDoc(doc);
    return value;
}

static bool xml_has_root(const char *body, size_t size, const char *name) {
    xmlDoc *doc;
    xmlNode *root;
    bool matches;
    doc = s3_xml_read(body, size, S3_UPLOAD_RESPONSE_LIMIT, "s3-complete.xml");
    if (doc == NULL) return false;
    root = xmlDocGetRootElement(doc);
    matches = s3_xml_name(root, name);
    xmlFreeDoc(doc);
    return matches;
}

static enum s3_result build_complete_xml(char *const *etags, size_t count,
                                         char **body, size_t *body_size,
                                         struct s3_error *error) {
    xmlBuffer *buffer = NULL;
    xmlTextWriter *writer = NULL;
    enum s3_result result = S3_RESULT_ERROR;
    *body = NULL;
    *body_size = 0;
    buffer = xmlBufferCreate();
    if (buffer == NULL) goto done;
    writer = xmlNewTextWriterMemory(buffer, 0);
    if (writer == NULL || xmlTextWriterStartElement(
                              writer, BAD_CAST "CompleteMultipartUpload") < 0)
        goto done;
    for (size_t i = 0; i < count; ++i) {
        if (xmlTextWriterStartElement(writer, BAD_CAST "Part") < 0 ||
            xmlTextWriterWriteFormatElement(writer, BAD_CAST "PartNumber",
                                            "%zu", i + 1) < 0 ||
            xmlTextWriterWriteElement(writer, BAD_CAST "ETag",
                                      BAD_CAST etags[i]) < 0 ||
            xmlTextWriterEndElement(writer) < 0)
            goto done;
    }
    if (xmlTextWriterEndElement(writer) < 0 || xmlTextWriterFlush(writer) < 0)
        goto done;
    *body_size = xmlBufferLength(buffer);
    *body = malloc(*body_size + 1);
    if (*body == NULL) goto done;
    memcpy(*body, xmlBufferContent(buffer), *body_size);
    (*body)[*body_size] = '\0';
    result = S3_RESULT_OK;

done:
    if (writer != NULL) xmlFreeTextWriter(writer);
    if (buffer != NULL) xmlBufferFree(buffer);
    if (result != S3_RESULT_OK) {
        free(*body);
        *body = NULL;
        *body_size = 0;
        return s3_error_set(error, S3_RESULT_ERROR,
                            "cannot build multipart XML");
    }
    return result;
}

enum s3_result s3_upload_initiate(struct s3_client *client,
                                  struct s3_error *error, const char *bucket,
                                  const char *key,
                                  const struct s3_object_properties *properties,
                                  const char *tagging,
                                  char **encoded_upload_id) {
    char *url = NULL, *upload_id = NULL;
    struct s3_memory_response response = {0};
    enum s3_result result =
        s3_url_build_object(client, bucket, key, "uploads", &url, error);
    *encoded_upload_id = NULL;
    if (result != S3_RESULT_OK) goto done;
    result = s3_upload_request_ex(
        client, error, url, "POST", (const unsigned char *) "", 0, properties,
        S3_UPLOAD_ONCE, NULL, &response, NULL, tagging);
    if (result != S3_RESULT_OK) goto done;
    upload_id = xml_value(response.body, response.size,
                          "InitiateMultipartUploadResult", "UploadId");
    if (upload_id == NULL) {
        result = s3_error_set(error, S3_RESULT_PROTOCOL_ERROR,
                              "invalid InitiateMultipartUpload XML");
        goto done;
    }
    *encoded_upload_id = s3_uri_encode_alloc(upload_id, false);
    if (*encoded_upload_id == NULL)
        result = s3_error_set(error, S3_RESULT_ERROR, "out of memory");
done:
    free(upload_id);
    free(url);
    s3_response_memory_cleanup(&response);
    return result;
}

enum s3_result s3_upload_part(struct s3_client *client, struct s3_error *error,
                              const char *bucket, const char *key,
                              const char *encoded_upload_id, size_t part_number,
                              const unsigned char *body, size_t body_size,
                              const struct s3_upload_copy_request *copy,
                              char **etag) {
    char query[1024], *url = NULL;
    struct s3_memory_response response = {0};
    enum s3_result result;
    *etag = NULL;
    if (snprintf(query, sizeof(query), "partNumber=%zu&uploadId=%s",
                 part_number, encoded_upload_id) >= (int) sizeof(query))
        return s3_error_set(error, S3_RESULT_ERROR,
                            "multipart upload ID is too long");
    result = s3_url_build_object(client, bucket, key, query, &url, error);
    if (result != S3_RESULT_OK) goto done;
    result =
        s3_upload_request_ex(client, error, url, "PUT", body, body_size, NULL,
                             S3_UPLOAD_RETRY, NULL, &response, copy, NULL);
    if (result != S3_RESULT_OK) goto done;
    if (copy != NULL) {
        *etag =
            xml_value(response.body, response.size, "CopyPartResult", "ETag");
        if (*etag == NULL)
            result = s3_error_set(error, S3_RESULT_PROTOCOL_ERROR,
                                  "CopyPartResult lacks ETag");
    }
    else if (response.response.properties.etag[0] == '\0')
        result = s3_error_set(error, S3_RESULT_PROTOCOL_ERROR,
                              "UploadPart response lacks ETag");
    else {
        *etag = s3_memory_strdup(response.response.properties.etag);
        if (*etag == NULL)
            result = s3_error_set(error, S3_RESULT_ERROR, "out of memory");
    }
done:
    free(url);
    s3_response_memory_cleanup(&response);
    return result;
}

enum s3_result s3_upload_complete(struct s3_client *client,
                                  struct s3_error *error, const char *bucket,
                                  const char *key,
                                  const char *encoded_upload_id,
                                  char *const *etags, size_t count,
                                  bool *uncertain) {
    char *body = NULL, *query = NULL, *url = NULL;
    size_t body_size = 0;
    struct s3_memory_response response = {0};
    enum s3_result result =
        build_complete_xml(etags, count, &body, &body_size, error);
    if (result != S3_RESULT_OK) goto done;
    size_t query_size = strlen(encoded_upload_id) + sizeof("uploadId=");
    query = malloc(query_size);
    if (query == NULL) {
        result = s3_error_set(error, S3_RESULT_ERROR, "out of memory");
        goto done;
    }
    (void) snprintf(query, query_size, "uploadId=%s", encoded_upload_id);
    result = s3_url_build_object(client, bucket, key, query, &url, error);
    if (result != S3_RESULT_OK) goto done;
    result = s3_upload_request(
        client, error, url, "POST", (const unsigned char *) body, body_size,
        &xml_properties, S3_UPLOAD_COMPLETE, uncertain, &response);
    if (result != S3_RESULT_OK && *uncertain)
        result = s3_error_set(error, S3_RESULT_ERROR,
                              "multipart completion outcome uncertain; object "
                              "may have been created");
done:
    free(body);
    free(query);
    free(url);
    s3_response_memory_cleanup(&response);
    return result;
}

static enum s3_result abort_upload(struct s3_client *client,
                                   struct s3_error *error, const char *bucket,
                                   const char *key,
                                   const char *encoded_upload_id) {
    char *query = NULL, *url = NULL;
    struct s3_memory_response response = {0};
    enum s3_result result;
    size_t size = strlen(encoded_upload_id) + 10;
    query = malloc(size);
    if (query == NULL) {
        return s3_error_set(error, S3_RESULT_ERROR, "out of memory");
    }
    (void) snprintf(query, size, "uploadId=%s", encoded_upload_id);
    result = s3_url_build_object(client, bucket, key, query, &url, error);
    if (result == S3_RESULT_OK) {
        /* Cleanup must still run after the caller cancels either PUT API. */
        s3_cancel_callback cancel_callback = client->cancel_callback;
        void *cancel_data = client->cancel_data;
        client->cancel_callback = NULL;
        result = s3_upload_request(client, error, url, "DELETE",
                                   (const unsigned char *) "", 0, NULL,
                                   S3_UPLOAD_RETRY, NULL, &response);
        client->cancel_callback = cancel_callback;
        client->cancel_data = cancel_data;
    }
    if (result == S3_RESULT_NOT_FOUND) result = S3_RESULT_OK;
    free(query);
    free(url);
    s3_response_memory_cleanup(&response);
    return result;
}

void s3_upload_cleanup_failed(struct s3_client *client, struct s3_error *error,
                              const char *bucket, const char *key,
                              const char *encoded_upload_id,
                              enum s3_result result,
                              bool completion_uncertain) {
    if (result == S3_RESULT_OK || encoded_upload_id == NULL ||
        completion_uncertain)
        return;
    struct s3_error abort_error = {0};
    enum s3_result abort_result =
        abort_upload(client, &abort_error, bucket, key, encoded_upload_id);
    if (abort_result == S3_RESULT_OK) return;
    char original[sizeof(error->message)];
    (void) snprintf(original, sizeof(original), "%s", error->message);
    (void) snprintf(error->message, sizeof(error->message),
                    "multipart abort failed: %.32s%s%.80s; original error: "
                    "%.80s",
                    s3_result_name(abort_result),
                    abort_error.message[0] != '\0' ? ": " : "",
                    abort_error.message, original);
}
