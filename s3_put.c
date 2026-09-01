/* SPDX-License-Identifier: MIT-0 */
#include "s3_internal.h"

#include <errno.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { DEFAULT_PART_SIZE = 16 * 1024 * 1024, PUT_RESPONSE_LIMIT = 1024 * 1024 };

struct memory_reader {
    const unsigned char *data;
    size_t size;
    size_t offset;
};

struct put_response {
    struct s3_response response;
    char *body;
    size_t size;
    size_t capacity;
    bool too_large;
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

static size_t collect_response(char *buffer, size_t size, size_t count,
                               void *data) {
    struct put_response *output = data;
    size_t bytes;
    char *body;
    if (size != 0 && count > SIZE_MAX / size) return 0;
    bytes = size * count;
    if (output->response.status < 200 || output->response.status >= 300) {
        size_t room = S3_ERROR_BODY_LIMIT - output->response.error_body_size;
        size_t copy = bytes < room ? bytes : room;
        memcpy(output->response.error_body + output->response.error_body_size,
               buffer, copy);
        output->response.error_body_size += copy;
        output->response.error_body[output->response.error_body_size] = '\0';
        return bytes;
    }
    if (bytes > PUT_RESPONSE_LIMIT - output->size) {
        output->too_large = true;
        return 0;
    }
    if (output->size + bytes + 1 > output->capacity) {
        size_t capacity = output->capacity == 0 ? 1024 : output->capacity;
        while (capacity < output->size + bytes + 1) capacity *= 2;
        body = realloc(output->body, capacity);
        if (body == NULL) return 0;
        output->body = body;
        output->capacity = capacity;
    }
    memcpy(output->body + output->size, buffer, bytes);
    output->size += bytes;
    output->body[output->size] = '\0';
    return bytes;
}

static enum s3_result
add_properties_headers(struct curl_slist **headers,
                       const struct s3_object_properties *properties,
                       struct s3_error *error) {
    if (properties == NULL) return S3_RESULT_OK;
    if ((properties->content_type[0] != '\0' &&
         !s3_add_header(headers, "Content-Type", properties->content_type)) ||
        (properties->content_encoding[0] != '\0' &&
         !s3_add_header(headers, "Content-Encoding",
                        properties->content_encoding)) ||
        (properties->cache_control[0] != '\0' &&
         !s3_add_header(headers, "Cache-Control", properties->cache_control)))
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
        if (!s3_add_header(headers, header_name, value)) {
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
               bool retry, struct put_response *output) {
    enum s3_result result = S3_RESULT_ERROR;
    unsigned attempts = retry ? client->max_attempts : 1;
    for (unsigned attempt = 1; attempt <= attempts; ++attempt) {
        struct curl_slist *headers = NULL;
        struct memory_reader reader = {.data = body, .size = body_size};
        CURLcode code;
        char curl_error[CURL_ERROR_SIZE] = {0};
        char length[32];
        if (attempt > 1) s3_response_cleanup(&output->response);
        s3_response_reset(&output->response);
        free(output->body);
        output->body = NULL;
        output->size = output->capacity = 0;
        output->too_large = false;
        result = add_properties_headers(&headers, properties, error);
        if (result != S3_RESULT_OK) {
            curl_slist_free_all(headers);
            return result;
        }
        (void) snprintf(length, sizeof(length), "%zu", body_size);
        if (!s3_add_header(&headers, "Content-Length", length)) {
            curl_slist_free_all(headers);
            return s3_error_set(error, S3_RESULT_ERROR, "out of memory");
        }
        result = s3_prepare_request(client, url, &headers, error);
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
                                s3_header_callback);
        (void) curl_easy_setopt(client->curl, CURLOPT_HEADERDATA,
                                &output->response);
        (void) curl_easy_setopt(client->curl, CURLOPT_WRITEFUNCTION,
                                collect_response);
        (void) curl_easy_setopt(client->curl, CURLOPT_WRITEDATA, output);
        (void) curl_easy_setopt(client->curl, CURLOPT_ERRORBUFFER, curl_error);
        code = curl_easy_perform(client->curl);
        (void) curl_easy_getinfo(client->curl, CURLINFO_RESPONSE_CODE,
                                 &output->response.status);
        curl_slist_free_all(headers);
        s3_error_clear(error);
        error->attempts = attempt;
        error->http_status = output->response.status;
        s3_parse_error_xml(output->response.error_body,
                           output->response.error_body_size, error);
        if (output->too_large) {
            result = s3_error_set(error, S3_RESULT_PROTOCOL_ERROR,
                                  "S3 upload response is too large");
            break;
        }
        if (code == CURLE_OK && output->response.status >= 200 &&
            output->response.status < 300)
            return S3_RESULT_OK;
        if (!s3_is_retryable(code, output->response.status, error->s3_code)) {
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
        s3_retry_delay(attempt - 1);
    }
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

static char *xml_value(const char *body, size_t size, const char *root_name,
                       const char *element) {
    xmlDoc *doc;
    xmlNode *root, *node = NULL;
    xmlChar *value = NULL;
    char *copy = NULL;
    if (body == NULL || size == 0 || size > (size_t) INT_MAX ||
        strstr(body, "<!DOCTYPE") != NULL)
        return NULL;
    doc = xmlReadMemory(body, (int) size, "s3-upload.xml", NULL,
                        XML_PARSE_NONET | XML_PARSE_NOERROR |
                            XML_PARSE_NOWARNING | XML_PARSE_NOBLANKS);
    if (doc == NULL) return NULL;
    root = xmlDocGetRootElement(doc);
    if (root != NULL &&
        xmlStrcmp(root->name, (const xmlChar *) root_name) == 0) {
        for (node = root->children; node != NULL; node = node->next)
            if (node->type == XML_ELEMENT_NODE &&
                xmlStrcmp(node->name, (const xmlChar *) element) == 0)
                break;
    }
    if (node != NULL) value = xmlNodeGetContent(node);
    if (value != NULL && value[0] != '\0')
        copy = s3_strdup((const char *) value);
    xmlFree(value);
    xmlFreeDoc(doc);
    return copy;
}

static bool xml_has_root(const char *body, size_t size, const char *name) {
    xmlDoc *doc;
    xmlNode *root;
    bool matches;
    if (body == NULL || size == 0 || size > (size_t) INT_MAX ||
        strstr(body, "<!DOCTYPE") != NULL)
        return false;
    doc = xmlReadMemory(body, (int) size, "s3-complete.xml", NULL,
                        XML_PARSE_NONET | XML_PARSE_NOERROR |
                            XML_PARSE_NOWARNING | XML_PARSE_NOBLANKS);
    if (doc == NULL) return false;
    root = xmlDocGetRootElement(doc);
    matches =
        root != NULL && xmlStrcmp(root->name, (const xmlChar *) name) == 0;
    xmlFreeDoc(doc);
    return matches;
}

static void put_response_free(struct put_response *response) {
    free(response->body);
    s3_response_cleanup(&response->response);
    memset(response, 0, sizeof(*response));
}

static void abort_upload(struct s3_client *client, const char *bucket,
                         const char *key, const char *encoded_upload_id) {
    char *query = NULL, *url = NULL;
    struct put_response response = {0};
    struct s3_error ignored;
    size_t size = strlen(encoded_upload_id) + 10;
    query = malloc(size);
    if (query != NULL) {
        (void) snprintf(query, size, "uploadId=%s", encoded_upload_id);
        if (s3_build_object_url(client, bucket, key, query, &url, &ignored) ==
            S3_RESULT_OK)
            (void) memory_request(client, &ignored, url, "DELETE",
                                  (const unsigned char *) "", 0, NULL, false,
                                  &response);
    }
    free(query);
    free(url);
    put_response_free(&response);
}

enum s3_result s3_object_put(struct s3_client *client, struct s3_error *error,
                             const char *bucket, const char *key, uint64_t size,
                             const struct s3_object_properties *properties,
                             s3_read_callback read_callback, void *data) {
    unsigned char *buffer = NULL;
    size_t part_size = DEFAULT_PART_SIZE;
    enum s3_result result;
    char *url = NULL;
    struct put_response response = {0};
    if (client == NULL || error == NULL || read_callback == NULL)
        return s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                            "invalid PutObject arguments");
    while (size / part_size + (size % part_size != 0) > 10000) {
        if (part_size > SIZE_MAX - 1024 * 1024)
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
        result = s3_build_url(client, bucket, key, &url, error);
        if (result == S3_RESULT_OK)
            result = memory_request(client, error, url, "PUT", buffer,
                                    part_size, properties, true, &response);
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
        etags = calloc(part_count, sizeof(*etags));
        if (etags == NULL) {
            result = s3_error_set(error, S3_RESULT_ERROR, "out of memory");
            goto multipart_done;
        }
        result =
            s3_build_object_url(client, bucket, key, "uploads", &url, error);
        if (result != S3_RESULT_OK) goto multipart_done;
        result = memory_request(client, error, url, "POST",
                                (const unsigned char *) "", 0, properties, true,
                                &response);
        if (result != S3_RESULT_OK) goto multipart_done;
        upload_id = xml_value(response.body, response.size,
                              "InitiateMultipartUploadResult", "UploadId");
        if (upload_id == NULL) {
            result = s3_error_set(error, S3_RESULT_PROTOCOL_ERROR,
                                  "invalid InitiateMultipartUpload XML");
            goto multipart_done;
        }
        encoded_upload_id =
            s3_uri_encode((const unsigned char *) upload_id, false);
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
                s3_build_object_url(client, bucket, key, query, &url, error);
            if (result != S3_RESULT_OK) goto multipart_done;
            put_response_free(&response);
            result = memory_request(client, error, url, "PUT", buffer, amount,
                                    NULL, true, &response);
            if (result != S3_RESULT_OK) goto multipart_done;
            if (response.response.properties.etag[0] == '\0') {
                result = s3_error_set(error, S3_RESULT_PROTOCOL_ERROR,
                                      "UploadPart response lacks ETag");
                goto multipart_done;
            }
            etags[part] = s3_strdup(response.response.properties.etag);
            if (etags[part] == NULL) {
                result = s3_error_set(error, S3_RESULT_ERROR, "out of memory");
                goto multipart_done;
            }
            remaining -= amount;
        }
        result = expect_eof(read_callback, data, error);
        if (result != S3_RESULT_OK) goto multipart_done;
        complete_size = 64;
        for (size_t i = 0; i < part_count; ++i)
            complete_size += strlen(etags[i]) + 80;
        complete = malloc(complete_size);
        if (complete == NULL) {
            result = s3_error_set(error, S3_RESULT_ERROR, "out of memory");
            goto multipart_done;
        }
        complete_size = (size_t) snprintf(complete, complete_size,
                                          "<CompleteMultipartUpload>");
        for (size_t i = 0; i < part_count; ++i)
            complete_size += (size_t) snprintf(
                complete + complete_size, 64 + strlen(etags[i]),
                "<Part><PartNumber>%zu</PartNumber><ETag>%s</ETag></Part>",
                i + 1, etags[i]);
        memcpy(complete + complete_size, "</CompleteMultipartUpload>", 27);
        complete_size += 26;
        complete[complete_size] = '\0';
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
                s3_build_object_url(client, bucket, key, query, &url, error);
            free(query);
        }
        if (result != S3_RESULT_OK) goto multipart_done;
        put_response_free(&response);
        result = memory_request(client, error, url, "POST",
                                (const unsigned char *) complete, complete_size,
                                NULL, false, &response);
        if (result == S3_RESULT_OK &&
            !xml_has_root(response.body, response.size,
                          "CompleteMultipartUploadResult")) {
            s3_error_clear(error);
            error->http_status = response.response.status;
            s3_parse_error_xml(response.body, response.size, error);
            if (error->message[0] != '\0') {
                error->result = S3_RESULT_PROTOCOL_ERROR;
                result = S3_RESULT_PROTOCOL_ERROR;
            }
            else {
                result = s3_error_set(error, S3_RESULT_PROTOCOL_ERROR,
                                      "invalid CompleteMultipartUpload XML");
            }
        }
    multipart_done:
        if (result != S3_RESULT_OK && encoded_upload_id != NULL) {
            struct s3_error saved = *error;
            abort_upload(client, bucket, key, encoded_upload_id);
            *error = saved;
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
