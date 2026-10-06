/* SPDX-License-Identifier: MIT-0 */
#include "s3_upload.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { DEFAULT_PART_SIZE = S3_MULTIPART_PART_SIZE };

static enum s3_result read_checked(unsigned char *buffer, size_t capacity,
                                   s3_read_callback callback, void *data,
                                   size_t *amount,
                                   enum s3_read_result *read_result,
                                   struct s3_error *error) {
    *amount = 0;
    *read_result = callback(data, buffer, capacity, amount);
    if (*amount > capacity || (*read_result == S3_READ_DATA && *amount == 0))
        return s3_error_set(error, S3_RESULT_CALLBACK_ERROR,
                            "input callback returned invalid data");
    if (*read_result == S3_READ_ERROR) {
        error->callback_errno = errno != 0 ? errno : EIO;
        return s3_error_set(error, S3_RESULT_CALLBACK_ERROR,
                            "input callback failed");
    }
    return S3_RESULT_OK;
}

static enum s3_result fill_buffer(unsigned char *buffer, size_t wanted,
                                  s3_read_callback callback, void *data,
                                  struct s3_error *error) {
    size_t offset = 0;
    while (offset < wanted) {
        size_t amount;
        enum s3_read_result read_result;
        enum s3_result result =
            read_checked(buffer + offset, wanted - offset, callback, data,
                         &amount, &read_result, error);
        if (result != S3_RESULT_OK) return result;
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
        size_t amount;
        enum s3_read_result read_result;
        enum s3_result result =
            read_checked(buffer + offset, capacity - offset, callback, data,
                         &amount, &read_result, error);
        if (result != S3_RESULT_OK) return result;
        offset += amount;
        if (read_result == S3_READ_EOF) {
            *eof = true;
            break;
        }
    }
    *size = offset;
    return S3_RESULT_OK;
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
    uint64_t required_part_size = size / 10000 + (size % 10000 != 0);
    if (required_part_size > part_size) {
        const uint64_t unit = 1024 * 1024;
        uint64_t units =
            required_part_size / unit + (required_part_size % unit != 0);
        if (units > S3_MULTIPART_MAX_PART_SIZE / unit ||
            units > SIZE_MAX / unit)
            return s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                                "object is too large");
        part_size = (size_t) (units * unit);
    }
    if (size < part_size) part_size = (size_t) size;
    buffer = malloc(part_size != 0 ? part_size : 1);
    if (buffer == NULL)
        return s3_error_set(error, S3_RESULT_ERROR, "out of memory");
    result = fill_buffer(buffer, part_size, read_callback, data, error);
    if (result != S3_RESULT_OK) goto done;
    if (size <= DEFAULT_PART_SIZE) {
        result = expect_eof(read_callback, data, error);
        if (result != S3_RESULT_OK) goto done;
        result = s3_url_build(client, bucket, key, &url, error);
        if (result == S3_RESULT_OK)
            result =
                s3_upload_request(client, error, url, "PUT", buffer, part_size,
                                  properties, S3_UPLOAD_RETRY, NULL, &response);
        goto done;
    }
    {
        char **etags = NULL;
        char *encoded_upload_id = NULL;
        size_t part_count =
            (size_t) (size / part_size + (size % part_size != 0));
        uint64_t remaining = size;
        bool completion_uncertain = false;
        etags = calloc(part_count, sizeof(*etags));
        if (etags == NULL) {
            result = s3_error_set(error, S3_RESULT_ERROR, "out of memory");
            goto multipart_done;
        }
        result = s3_upload_initiate(client, error, bucket, key, properties,
                                    NULL, &encoded_upload_id);
        if (result != S3_RESULT_OK) goto multipart_done;
        for (size_t part = 0; part < part_count; ++part) {
            size_t amount =
                remaining < part_size ? (size_t) remaining : part_size;
            if (part != 0) {
                result =
                    fill_buffer(buffer, amount, read_callback, data, error);
                if (result != S3_RESULT_OK) goto multipart_done;
            }
            result =
                s3_upload_part(client, error, bucket, key, encoded_upload_id,
                               part + 1, buffer, amount, NULL, &etags[part]);
            if (result != S3_RESULT_OK) goto multipart_done;
            remaining -= amount;
        }
        result = expect_eof(read_callback, data, error);
        if (result != S3_RESULT_OK) goto multipart_done;
        result =
            s3_upload_complete(client, error, bucket, key, encoded_upload_id,
                               etags, part_count, &completion_uncertain);
    multipart_done:
        s3_upload_cleanup_failed(client, error, bucket, key, encoded_upload_id,
                                 result, completion_uncertain);
        for (size_t i = 0; i < part_count; ++i)
            free(etags != NULL ? etags[i] : NULL);
        free(etags);
        free(encoded_upload_id);
    }
done:
    free(buffer);
    free(url);
    s3_response_memory_cleanup(&response);
    return result;
}

enum s3_result
s3_object_put_stream(struct s3_client *client, struct s3_error *error,
                     const char *bucket, const char *key, size_t part_size,
                     const struct s3_object_properties *properties,
                     s3_read_callback read_callback, void *data) {
    unsigned char *buffer = NULL;
    unsigned char next_byte;
    char **etags = NULL;
    char *url = NULL;
    struct s3_memory_response response = {0};
    size_t etag_count = 0;
    size_t amount = 0, next_size = 0;
    bool eof = false, next_eof = false;
    char *encoded_upload_id = NULL;
    enum s3_result result;
    bool completion_uncertain = false;

    s3_error_clear(error);
    if (client == NULL || error == NULL ||
        part_size < S3_UPLOAD_MIN_PART_SIZE ||
        (uint64_t) part_size > S3_MULTIPART_MAX_PART_SIZE ||
        read_callback == NULL)
        return s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                            "invalid PutObject arguments");
    buffer = malloc(part_size);
    if (buffer == NULL) {
        result = s3_error_set(error, S3_RESULT_ERROR, "out of memory");
        goto done;
    }
    result =
        read_part(buffer, part_size, read_callback, data, &amount, &eof, error);
    if (result != S3_RESULT_OK) goto done;
    /* One byte of lookahead distinguishes an exact-sized object from a stream
     * that needs multipart, without allocating a second part buffer. */
    if (!eof) {
        result = read_part(&next_byte, 1, read_callback, data, &next_size,
                           &next_eof, error);
        if (result != S3_RESULT_OK) goto done;
    }
    if (eof || next_size == 0) {
        result = s3_url_build(client, bucket, key, &url, error);
        if (result == S3_RESULT_OK)
            result =
                s3_upload_request(client, error, url, "PUT", buffer, amount,
                                  properties, S3_UPLOAD_RETRY, NULL, &response);
        goto done;
    }
    etags = calloc(10000, sizeof(*etags));
    if (etags == NULL) {
        result = s3_error_set(error, S3_RESULT_ERROR, "out of memory");
        goto done;
    }
    result = s3_upload_initiate(client, error, bucket, key, properties, NULL,
                                &encoded_upload_id);
    if (result != S3_RESULT_OK) goto done;
    for (;;) {
        if (etag_count != 0) {
            size_t prefix = etag_count == 1 ? next_size : 0;
            if (prefix != 0) buffer[0] = next_byte;
            eof = prefix != 0 && next_eof;
            amount = 0;
            if (!eof) {
                result = read_part(buffer + prefix, part_size - prefix,
                                   read_callback, data, &amount, &eof, error);
                if (result != S3_RESULT_OK) goto done;
            }
            amount += prefix;
        }
        if (amount == 0 && eof && etag_count != 0) break;
        if (etag_count == 10000) {
            result = s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                                  "stream exceeds multipart upload limit");
            goto done;
        }
        result = s3_upload_part(client, error, bucket, key, encoded_upload_id,
                                etag_count + 1, buffer, amount, NULL,
                                &etags[etag_count]);
        if (result != S3_RESULT_OK) goto done;
        ++etag_count;
        if (eof) break;
    }

    result = s3_upload_complete(client, error, bucket, key, encoded_upload_id,
                                etags, etag_count, &completion_uncertain);

done:
    s3_upload_cleanup_failed(client, error, bucket, key, encoded_upload_id,
                             result, completion_uncertain);
    for (size_t i = 0; i < etag_count; ++i) free(etags[i]);
    free(etags);
    free(buffer);
    free(url);
    s3_response_memory_cleanup(&response);
    free(encoded_upload_id);
    return result;
}
