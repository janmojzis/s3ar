/* SPDX-License-Identifier: MIT-0 */
#include "s3_internal.h"

#include <stdio.h>

const char *s3_result_name(enum s3_result result) {
    static const char *const names[] = {"ok",
                                        "not found",
                                        "precondition failed",
                                        "access denied",
                                        "callback error",
                                        "retry exhausted",
                                        "protocol error",
                                        "configuration error",
                                        "error",
                                        "not modified"};
    if ((unsigned) result >= sizeof(names) / sizeof(names[0])) return "error";
    return names[result];
}

enum s3_result s3_result_from_response(CURLcode code,
                                       const struct s3_response *response,
                                       bool callback_failed,
                                       struct s3_error *error) {
    if (callback_failed)
        return s3_error_set(error, S3_RESULT_CALLBACK_ERROR,
                            "output callback failed");
    if (code != CURLE_OK)
        return s3_error_set(error, S3_RESULT_ERROR, curl_easy_strerror(code));
    if (response->status >= 200 && response->status < 300) return S3_RESULT_OK;
    if (response->status == 404) {
        if (error != NULL && error->message[0] != '\0') {
            error->result = S3_RESULT_NOT_FOUND;
            return error->result;
        }
        return s3_error_set(error, S3_RESULT_NOT_FOUND, "object not found");
    }
    if (response->status == 403) {
        if (error != NULL && error->message[0] != '\0') {
            error->result = S3_RESULT_ACCESS_DENIED;
            return error->result;
        }
        return s3_error_set(error, S3_RESULT_ACCESS_DENIED, "access denied");
    }
    if (response->status == 412)
        return s3_error_set(error, S3_RESULT_PRECONDITION_FAILED,
                            "object changed during download");
    if (response->status == 301 && response->bucket_region[0] != '\0') {
        if (error == NULL) return S3_RESULT_ERROR;
        error->result = S3_RESULT_ERROR;
        (void) snprintf(error->bucket_region, sizeof(error->bucket_region),
                        "%s", response->bucket_region);
        (void) snprintf(error->message, sizeof(error->message),
                        "bucket is in region %s", response->bucket_region);
        return error->result;
    }
    if (error != NULL && error->message[0] != '\0') {
        error->result = S3_RESULT_ERROR;
        return error->result;
    }
    return s3_error_set(error, S3_RESULT_ERROR, "S3 request failed");
}
