/* SPDX-License-Identifier: MIT-0 */
#include "s3_internal.h"

#include <libxml/xmlwriter.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static enum s3_result build_create_bucket_xml(const char *region, char **body,
                                              struct s3_error *error) {
    static const char namespace[] = "http://s3.amazonaws.com/doc/2006-03-01/";
    xmlBuffer *buffer = NULL;
    xmlTextWriter *writer = NULL;
    enum s3_result result = S3_RESULT_ERROR;
    *body = NULL;
    buffer = xmlBufferCreate();
    if (buffer == NULL) goto done;
    writer = xmlNewTextWriterMemory(buffer, 0);
    if (writer == NULL ||
        xmlTextWriterStartElement(writer,
                                  BAD_CAST "CreateBucketConfiguration") < 0 ||
        xmlTextWriterWriteAttribute(writer, BAD_CAST "xmlns",
                                    BAD_CAST namespace) < 0 ||
        xmlTextWriterWriteElement(writer, BAD_CAST "LocationConstraint",
                                  BAD_CAST region) < 0 ||
        xmlTextWriterEndElement(writer) < 0 || xmlTextWriterFlush(writer) < 0)
        goto done;
    *body = malloc(xmlBufferLength(buffer) + 1);
    if (*body == NULL) goto done;
    memcpy(*body, xmlBufferContent(buffer), xmlBufferLength(buffer));
    (*body)[xmlBufferLength(buffer)] = '\0';
    result = S3_RESULT_OK;

done:
    if (writer != NULL) xmlFreeTextWriter(writer);
    if (buffer != NULL) xmlBufferFree(buffer);
    if (result != S3_RESULT_OK) {
        free(*body);
        *body = NULL;
        return s3_error_set(error, S3_RESULT_ERROR,
                            "cannot build CreateBucket XML");
    }
    return result;
}

enum s3_result s3_bucket_head(struct s3_client *client, struct s3_error *error,
                              const char *bucket) {
    s3_error_clear(error);
    if (client == NULL || error == NULL)
        return s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                            "invalid HeadBucket arguments");
    return s3_request_bucket(client, error, bucket, NULL, "HEAD", NULL, NULL,
                             NULL);
}

enum s3_result s3_bucket_delete(struct s3_client *client,
                                struct s3_error *error, const char *bucket) {
    s3_error_clear(error);
    if (client == NULL || error == NULL || bucket == NULL)
        return s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                            "invalid bucket deletion arguments");
    return s3_request_bucket(client, error, bucket, NULL, "DELETE", NULL, NULL,
                             NULL);
}

enum s3_result s3_bucket_create(struct s3_client *client,
                                struct s3_error *error, const char *bucket) {
    char *content = NULL;
    enum s3_result result;
    s3_error_clear(error);
    if (client == NULL || error == NULL)
        return s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                            "invalid CreateBucket arguments");
    if (strcmp(client->region, "us-east-1") != 0) {
        result = build_create_bucket_xml(client->region, &content, error);
        if (result != S3_RESULT_OK) return result;
    }
    result = s3_request_bucket(client, error, bucket, NULL, "PUT", content,
                               NULL, NULL);
    free(content);
    if (result != S3_RESULT_OK &&
        strcmp(error->s3_code, "BucketAlreadyOwnedByYou") == 0) {
        s3_error_clear(error);
        return S3_RESULT_OK;
    }
    return result;
}

enum s3_result s3_bucket_ensure(struct s3_client *client,
                                struct s3_error *error, const char *bucket) {
    s3_error_clear(error);
    enum s3_result result = s3_bucket_head(client, error, bucket);
    if (result == S3_RESULT_OK) return result;
    if (result != S3_RESULT_NOT_FOUND) return result;
    return s3_bucket_create(client, error, bucket);
}
