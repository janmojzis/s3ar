/* SPDX-License-Identifier: MIT-0 */
#include "s3_internal.h"
#include "s3_xml.h"

#include <errno.h>
#include <libxml/xmlwriter.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    DEFAULT_PART_SIZE = S3_MULTIPART_PART_SIZE,
    MIN_PART_SIZE = 5 * 1024 * 1024,
    PUT_RESPONSE_LIMIT = 1024 * 1024
};

static const struct s3_object_properties xml_properties = {
    .content_type = "application/xml",
};

enum request_retry_mode { REQUEST_ONCE, REQUEST_RETRY, REQUEST_COMPLETE };

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
    if (properties->metadata_count > 128 ||
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

static enum s3_result
memory_request(struct s3_client *client, struct s3_error *error,
               const char *url, const char *method, const unsigned char *body,
               size_t body_size, const struct s3_object_properties *properties,
               enum request_retry_mode retry_mode, bool *completion_uncertain,
               struct s3_memory_response *output) {
    enum s3_result result = S3_RESULT_ERROR;
    unsigned attempts = retry_mode == REQUEST_ONCE ? 1 : client->max_attempts;
    if (completion_uncertain != NULL) *completion_uncertain = false;
    for (unsigned attempt = 1; attempt <= attempts; ++attempt) {
        struct curl_slist *headers = NULL;
        struct memory_reader reader = {.data = body, .size = body_size};
        CURLcode code;
        bool completion_succeeded = false;
        char curl_error[CURL_ERROR_SIZE] = {0};
        char length[32];
        s3_response_memory_reset(output, PUT_RESPONSE_LIMIT);
        result = add_properties_headers(&headers, properties, error);
        if (result != S3_RESULT_OK) {
            curl_slist_free_all(headers);
            return result;
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
        (void) curl_easy_setopt(client->curl, CURLOPT_HEADERFUNCTION,
                                s3_headers_callback);
        (void) curl_easy_setopt(client->curl, CURLOPT_HEADERDATA,
                                &output->response);
        (void) curl_easy_setopt(client->curl, CURLOPT_WRITEFUNCTION,
                                s3_response_memory_collect);
        (void) curl_easy_setopt(client->curl, CURLOPT_WRITEDATA, output);
        (void) curl_easy_setopt(client->curl, CURLOPT_ERRORBUFFER, curl_error);
        s3_trace_perform_start(client, attempt, attempts);
        code = curl_easy_perform(client->curl);
        (void) curl_easy_getinfo(client->curl, CURLINFO_RESPONSE_CODE,
                                 &output->response.status);
        curl_slist_free_all(headers);
        s3_error_clear(error);
        error->attempts = attempt;
        error->http_status = output->response.status;
        s3_error_parse_xml(output->response.error_body,
                           output->response.error_body_size, error);
        if (retry_mode == REQUEST_COMPLETE && code == CURLE_OK &&
            output->response.status >= 200 && output->response.status < 300 &&
            output->body_error == S3_RESULT_OK) {
            completion_succeeded = xml_has_root(
                output->body, output->size, "CompleteMultipartUploadResult");
            if (!completion_succeeded)
                s3_error_parse_xml(output->body, output->size, error);
        }
        s3_trace_perform_end(client, attempt, attempts, code, &output->response,
                             error);
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
            if (retry_mode != REQUEST_COMPLETE || completion_succeeded)
                return S3_RESULT_OK;
            /* S3 may return an Error XML document with HTTP 200. */
            if (error->s3_code[0] == '\0') {
                if (completion_uncertain != NULL) *completion_uncertain = true;
                result = s3_error_set(error, S3_RESULT_PROTOCOL_ERROR,
                                      "invalid CompleteMultipartUpload XML");
                break;
            }
            if (!s3_retry_allowed(code, output->response.status,
                                  error->s3_code)) {
                if (error->message[0] != '\0') {
                    error->result = S3_RESULT_PROTOCOL_ERROR;
                    result = S3_RESULT_PROTOCOL_ERROR;
                }
                else
                    result = s3_error_set(error, S3_RESULT_PROTOCOL_ERROR,
                                          "S3 multipart completion failed");
                break;
            }
        }
        else if (completion_uncertain != NULL && code != CURLE_OK) {
            /* A lost response may follow a successful completion. */
            *completion_uncertain = true;
        }
        if (!s3_retry_allowed(code, output->response.status, error->s3_code)) {
            result =
                s3_result_from_response(code, &output->response, false, error);
            if (code != CURLE_OK && curl_error[0] != '\0')
                (void) snprintf(error->message, sizeof(error->message), "%s",
                                curl_error);
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

static enum s3_result complete_upload(struct s3_client *client,
                                      struct s3_error *error, const char *url,
                                      const char *body, size_t body_size,
                                      bool *uncertain,
                                      struct s3_memory_response *response) {
    enum s3_result result = memory_request(
        client, error, url, "POST", (const unsigned char *) body, body_size,
        &xml_properties, REQUEST_COMPLETE, uncertain, response);
    if (result != S3_RESULT_OK && *uncertain)
        return s3_error_set(error, S3_RESULT_ERROR,
                            "multipart completion outcome uncertain; object "
                            "may have been created");
    return result;
}

static enum s3_result fill_buffer(unsigned char *buffer, size_t wanted,
                                  s3_read_callback callback, void *data,
                                  struct s3_error *error) {
    size_t offset = 0;
    while (offset < wanted) {
        size_t amount = 0;
        enum s3_read_result read_result =
            callback(data, buffer + offset, wanted - offset, &amount);
        if (amount > wanted - offset ||
            (read_result == S3_READ_DATA && amount == 0))
            return s3_error_set(error, S3_RESULT_CALLBACK_ERROR,
                                "input callback returned invalid data");
        if (read_result == S3_READ_ERROR) {
            error->callback_errno = errno != 0 ? errno : EIO;
            return s3_error_set(error, S3_RESULT_CALLBACK_ERROR,
                                "input callback failed");
        }
        if (read_result == S3_READ_EOF)
            return s3_error_set(error, S3_RESULT_CALLBACK_ERROR,
                                "input ended before declared object size");
        offset += amount;
    }
    return S3_RESULT_OK;
}

static enum s3_result expect_eof(s3_read_callback callback, void *data,
                                 struct s3_error *error) {
    unsigned char byte;
    size_t amount = 0;
    enum s3_read_result result = callback(data, &byte, 1, &amount);
    if (result == S3_READ_ERROR) {
        error->callback_errno = errno != 0 ? errno : EIO;
        return s3_error_set(error, S3_RESULT_CALLBACK_ERROR,
                            "input callback failed");
    }
    if (result != S3_READ_EOF || amount != 0)
        return s3_error_set(error, S3_RESULT_CALLBACK_ERROR,
                            "input exceeds declared object size");
    return S3_RESULT_OK;
}

static enum s3_result read_part(unsigned char *buffer, size_t capacity,
                                s3_read_callback callback, void *data,
                                size_t *size, bool *eof,
                                struct s3_error *error) {
    size_t offset = 0;
    *eof = false;
    while (offset < capacity) {
        size_t amount = 0;
        enum s3_read_result read_result =
            callback(data, buffer + offset, capacity - offset, &amount);
        if (amount > capacity - offset ||
            (read_result == S3_READ_DATA && amount == 0))
            return s3_error_set(error, S3_RESULT_CALLBACK_ERROR,
                                "input callback returned invalid data");
        if (read_result == S3_READ_ERROR) {
            error->callback_errno = errno != 0 ? errno : EIO;
            return s3_error_set(error, S3_RESULT_CALLBACK_ERROR,
                                "input callback failed");
        }
        offset += amount;
        if (read_result == S3_READ_EOF) {
            *eof = true;
            break;
        }
    }
    *size = offset;
    return S3_RESULT_OK;
}

static char *xml_value(const char *body, size_t size, const char *root_name,
                       const char *element) {
    xmlDoc *doc;
    xmlNode *root, *node = NULL;
    xmlChar *value = NULL;
    char *copy = NULL;
    doc = s3_xml_read(body, size, PUT_RESPONSE_LIMIT, "s3-upload.xml");
    if (doc == NULL) return NULL;
    root = xmlDocGetRootElement(doc);
    if (s3_xml_name(root, root_name)) {
        for (node = root->children; node != NULL; node = node->next)
            if (s3_xml_name(node, element)) break;
    }
    if (node != NULL) value = xmlNodeGetContent(node);
    if (value != NULL && value[0] != '\0')
        copy = s3_memory_strdup((const char *) value);
    xmlFree(value);
    xmlFreeDoc(doc);
    return copy;
}

static bool xml_has_root(const char *body, size_t size, const char *name) {
    xmlDoc *doc;
    xmlNode *root;
    bool matches;
    doc = s3_xml_read(body, size, PUT_RESPONSE_LIMIT, "s3-complete.xml");
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

static void put_response_free(struct s3_memory_response *response) {
    s3_response_memory_cleanup(response);
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
        result = memory_request(client, error, url, "DELETE",
                                (const unsigned char *) "", 0, NULL,
                                REQUEST_RETRY, NULL, &response);
        client->cancel_callback = cancel_callback;
        client->cancel_data = cancel_data;
    }
    if (result == S3_RESULT_NOT_FOUND) result = S3_RESULT_OK;
    free(query);
    free(url);
    put_response_free(&response);
    return result;
}

static void record_abort_failure(struct s3_error *error,
                                 enum s3_result abort_result,
                                 const struct s3_error *abort_error) {
    char original[sizeof(error->message)];
    (void) snprintf(original, sizeof(original), "%s", error->message);
    (void) snprintf(error->message, sizeof(error->message),
                    "multipart abort failed: %.32s%s%.80s; original error: "
                    "%.80s",
                    s3_result_name(abort_result),
                    abort_error->message[0] != '\0' ? ": " : "",
                    abort_error->message, original);
}

enum s3_result s3_object_put(struct s3_client *client, struct s3_error *error,
                             const char *bucket, const char *key, uint64_t size,
                             const struct s3_object_properties *properties,
                             s3_read_callback read_callback, void *data) {
    unsigned char *buffer = NULL;
    size_t part_size = DEFAULT_PART_SIZE;
    enum s3_result result;
    char *url = NULL;
    struct s3_memory_response response = {0};
    s3_error_clear(error);
    if (client == NULL || error == NULL || read_callback == NULL)
        return s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                            "invalid PutObject arguments");
    while (size / part_size + (size % part_size != 0) > 10000) {
        if ((uint64_t) part_size > S3_MULTIPART_MAX_PART_SIZE - 1024 * 1024 ||
            part_size > SIZE_MAX - 1024 * 1024)
            return s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                                "object is too large");
        part_size += 1024 * 1024;
    }
    if (size < part_size) part_size = (size_t) size;
    buffer = malloc(part_size != 0 ? part_size : 1);
    if (buffer == NULL)
        return s3_error_set(error, S3_RESULT_ERROR, "out of memory");
    result = fill_buffer(buffer, part_size, read_callback, data, error);
    if (result != S3_RESULT_OK) goto done;
    if (size < DEFAULT_PART_SIZE) {
        result = expect_eof(read_callback, data, error);
        if (result != S3_RESULT_OK) goto done;
        result = s3_url_build(client, bucket, key, &url, error);
        if (result == S3_RESULT_OK)
            result =
                memory_request(client, error, url, "PUT", buffer, part_size,
                               properties, REQUEST_RETRY, NULL, &response);
        goto done;
    }
    {
        char **etags = NULL;
        char *upload_id = NULL, *encoded_upload_id = NULL;
        size_t part_count =
            (size_t) (size / part_size + (size % part_size != 0));
        uint64_t remaining = size;
        char *complete = NULL;
        size_t complete_size = 0;
        bool completion_uncertain = false;
        etags = calloc(part_count, sizeof(*etags));
        if (etags == NULL) {
            result = s3_error_set(error, S3_RESULT_ERROR, "out of memory");
            goto multipart_done;
        }
        result =
            s3_url_build_object(client, bucket, key, "uploads", &url, error);
        if (result != S3_RESULT_OK) goto multipart_done;
        result = memory_request(client, error, url, "POST",
                                (const unsigned char *) "", 0, properties,
                                REQUEST_ONCE, NULL, &response);
        if (result != S3_RESULT_OK) goto multipart_done;
        upload_id = xml_value(response.body, response.size,
                              "InitiateMultipartUploadResult", "UploadId");
        if (upload_id == NULL) {
            result = s3_error_set(error, S3_RESULT_PROTOCOL_ERROR,
                                  "invalid InitiateMultipartUpload XML");
            goto multipart_done;
        }
        encoded_upload_id = s3_uri_encode_alloc(upload_id, false);
        if (encoded_upload_id == NULL) {
            result = s3_error_set(error, S3_RESULT_ERROR, "out of memory");
            goto multipart_done;
        }
        for (size_t part = 0; part < part_count; ++part) {
            size_t amount =
                remaining < part_size ? (size_t) remaining : part_size;
            char query[1024];
            if (part != 0) {
                result =
                    fill_buffer(buffer, amount, read_callback, data, error);
                if (result != S3_RESULT_OK) goto multipart_done;
            }
            free(url);
            url = NULL;
            if (snprintf(query, sizeof(query), "partNumber=%zu&uploadId=%s",
                         part + 1, encoded_upload_id) >= (int) sizeof(query)) {
                result = s3_error_set(error, S3_RESULT_ERROR,
                                      "multipart upload ID is too long");
                goto multipart_done;
            }
            result =
                s3_url_build_object(client, bucket, key, query, &url, error);
            if (result != S3_RESULT_OK) goto multipart_done;
            put_response_free(&response);
            result = memory_request(client, error, url, "PUT", buffer, amount,
                                    NULL, REQUEST_RETRY, NULL, &response);
            if (result != S3_RESULT_OK) goto multipart_done;
            if (response.response.properties.etag[0] == '\0') {
                result = s3_error_set(error, S3_RESULT_PROTOCOL_ERROR,
                                      "UploadPart response lacks ETag");
                goto multipart_done;
            }
            etags[part] = s3_memory_strdup(response.response.properties.etag);
            if (etags[part] == NULL) {
                result = s3_error_set(error, S3_RESULT_ERROR, "out of memory");
                goto multipart_done;
            }
            remaining -= amount;
        }
        result = expect_eof(read_callback, data, error);
        if (result != S3_RESULT_OK) goto multipart_done;
        result = build_complete_xml(etags, part_count, &complete,
                                    &complete_size, error);
        if (result != S3_RESULT_OK) goto multipart_done;
        free(url);
        url = NULL;
        {
            size_t query_size = strlen(encoded_upload_id) + 10;
            char *query = malloc(query_size);
            if (query == NULL) {
                result = s3_error_set(error, S3_RESULT_ERROR, "out of memory");
                goto multipart_done;
            }
            (void) snprintf(query, query_size, "uploadId=%s",
                            encoded_upload_id);
            result =
                s3_url_build_object(client, bucket, key, query, &url, error);
            free(query);
        }
        if (result != S3_RESULT_OK) goto multipart_done;
        put_response_free(&response);
        result = complete_upload(client, error, url, complete, complete_size,
                                 &completion_uncertain, &response);
    multipart_done:
        if (result != S3_RESULT_OK && encoded_upload_id != NULL &&
            !completion_uncertain) {
            struct s3_error saved = *error;
            struct s3_error abort_error = {0};
            enum s3_result abort_result = abort_upload(
                client, &abort_error, bucket, key, encoded_upload_id);
            *error = saved;
            if (abort_result != S3_RESULT_OK)
                record_abort_failure(error, abort_result, &abort_error);
        }
        for (size_t i = 0; i < part_count; ++i)
            free(etags != NULL ? etags[i] : NULL);
        free(etags);
        free(upload_id);
        free(encoded_upload_id);
        free(complete);
    }
done:
    free(buffer);
    free(url);
    put_response_free(&response);
    return result;
}

enum s3_result
s3_object_put_stream(struct s3_client *client, struct s3_error *error,
                     const char *bucket, const char *key, size_t part_size,
                     const struct s3_object_properties *properties,
                     s3_read_callback read_callback, void *data) {
    unsigned char *buffer = NULL;
    char **etags = NULL;
    size_t etag_count = 0;
    char *url = NULL, *upload_id = NULL, *encoded_upload_id = NULL;
    char *complete = NULL;
    struct s3_memory_response response = {0};
    enum s3_result result;
    bool completion_uncertain = false;

    s3_error_clear(error);
    if (client == NULL || error == NULL || part_size < MIN_PART_SIZE ||
        (uint64_t) part_size > S3_MULTIPART_MAX_PART_SIZE ||
        read_callback == NULL)
        return s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                            "invalid PutObject arguments");
    buffer = malloc(part_size);
    etags = calloc(10000, sizeof(*etags));
    if (buffer == NULL || etags == NULL) {
        result = s3_error_set(error, S3_RESULT_ERROR, "out of memory");
        goto done;
    }
    result = s3_url_build_object(client, bucket, key, "uploads", &url, error);
    if (result != S3_RESULT_OK) goto done;
    result =
        memory_request(client, error, url, "POST", (const unsigned char *) "",
                       0, properties, REQUEST_ONCE, NULL, &response);
    if (result != S3_RESULT_OK) goto done;
    upload_id = xml_value(response.body, response.size,
                          "InitiateMultipartUploadResult", "UploadId");
    if (upload_id == NULL) {
        result = s3_error_set(error, S3_RESULT_PROTOCOL_ERROR,
                              "invalid InitiateMultipartUpload XML");
        goto done;
    }
    encoded_upload_id = s3_uri_encode_alloc(upload_id, false);
    if (encoded_upload_id == NULL) {
        result = s3_error_set(error, S3_RESULT_ERROR, "out of memory");
        goto done;
    }

    for (;;) {
        size_t amount = 0;
        bool eof = false;
        char query[1024];
        result = read_part(buffer, part_size, read_callback, data, &amount,
                           &eof, error);
        if (result != S3_RESULT_OK) goto done;
        if (amount == 0 && eof && etag_count != 0) break;
        if (etag_count == 10000) {
            result = s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                                  "stream exceeds multipart upload limit");
            goto done;
        }
        if (snprintf(query, sizeof(query), "partNumber=%zu&uploadId=%s",
                     etag_count + 1,
                     encoded_upload_id) >= (int) sizeof(query)) {
            result = s3_error_set(error, S3_RESULT_ERROR,
                                  "multipart upload ID is too long");
            goto done;
        }
        free(url);
        url = NULL;
        result = s3_url_build_object(client, bucket, key, query, &url, error);
        if (result != S3_RESULT_OK) goto done;
        put_response_free(&response);
        result = memory_request(client, error, url, "PUT", buffer, amount, NULL,
                                REQUEST_RETRY, NULL, &response);
        if (result != S3_RESULT_OK) goto done;
        if (response.response.properties.etag[0] == '\0') {
            result = s3_error_set(error, S3_RESULT_PROTOCOL_ERROR,
                                  "UploadPart response lacks ETag");
            goto done;
        }
        etags[etag_count] = s3_memory_strdup(response.response.properties.etag);
        if (etags[etag_count] == NULL) {
            result = s3_error_set(error, S3_RESULT_ERROR, "out of memory");
            goto done;
        }
        ++etag_count;
        if (eof) break;
    }

    {
        size_t complete_size = 0;
        result = build_complete_xml(etags, etag_count, &complete,
                                    &complete_size, error);
        if (result != S3_RESULT_OK) goto done;
        free(url);
        url = NULL;
        {
            size_t query_size = strlen(encoded_upload_id) + 10;
            char *query = malloc(query_size);
            if (query == NULL) {
                result = s3_error_set(error, S3_RESULT_ERROR, "out of memory");
                goto done;
            }
            (void) snprintf(query, query_size, "uploadId=%s",
                            encoded_upload_id);
            result =
                s3_url_build_object(client, bucket, key, query, &url, error);
            free(query);
        }
        if (result != S3_RESULT_OK) goto done;
        put_response_free(&response);
        result = complete_upload(client, error, url, complete, complete_size,
                                 &completion_uncertain, &response);
    }

done:
    if (result != S3_RESULT_OK && encoded_upload_id != NULL &&
        !completion_uncertain) {
        struct s3_error saved = *error;
        struct s3_error abort_error = {0};
        enum s3_result abort_result =
            abort_upload(client, &abort_error, bucket, key, encoded_upload_id);
        *error = saved;
        if (abort_result != S3_RESULT_OK)
            record_abort_failure(error, abort_result, &abort_error);
    }
    for (size_t i = 0; i < etag_count; ++i) free(etags[i]);
    free(etags);
    free(buffer);
    free(url);
    free(upload_id);
    free(encoded_upload_id);
    free(complete);
    put_response_free(&response);
    return result;
}
