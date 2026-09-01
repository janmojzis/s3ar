/* SPDX-License-Identifier: MIT-0 */
#include "s3_internal.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

bool s3_is_retryable(CURLcode code, long status, const char *s3_code) {
    switch (code) {
    case CURLE_COULDNT_RESOLVE_PROXY:
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT:
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_SEND_ERROR:
    case CURLE_RECV_ERROR:
    case CURLE_PARTIAL_FILE:
    case CURLE_HTTP2:
        return true;
    default:
        break;
    }
    if (code != CURLE_OK) return false;
    if (status == 408 || status == 429 || status == 500 || status == 502 ||
        status == 503 || status == 504)
        return true;
    return s3_code != NULL &&
           (strcmp(s3_code, "InternalError") == 0 ||
            strcmp(s3_code, "RequestTimeout") == 0 ||
            strcmp(s3_code, "SlowDown") == 0 ||
            strcmp(s3_code, "ServiceUnavailable") == 0);
}

void s3_retry_delay(unsigned attempt) {
    struct timespec delay;
    unsigned cap_ms = 100U << (attempt < 5 ? attempt : 5);
    unsigned milliseconds = (unsigned)rand() % (cap_ms + 1U);
    delay.tv_sec = (time_t)(milliseconds / 1000U);
    delay.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
    while (nanosleep(&delay, &delay) != 0) continue;
}

enum s3_result s3_result_from_response(CURLcode code,
                                       const struct s3_response *response,
                                       bool callback_failed,
                                       struct s3_error *error) {
    if (callback_failed) return s3_error_set(error, S3_RESULT_CALLBACK_ERROR,
                                              "output callback failed");
    if (code != CURLE_OK) return s3_error_set(error, S3_RESULT_ERROR,
                                               curl_easy_strerror(code));
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
    if (error != NULL && error->message[0] != '\0') {
        error->result = S3_RESULT_ERROR;
        return error->result;
    }
    return s3_error_set(error, S3_RESULT_ERROR, "S3 request failed");
}
