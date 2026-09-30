/* SPDX-License-Identifier: MIT-0 */
#include "s3_internal.h"
#include "log.h"

#include <randombytes.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

bool s3_retry_allowed(CURLcode code, long status, const char *s3_code) {
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
    if (status == 307 || status == 408 || status == 429 || status == 500 ||
        status == 502 || status == 503 || status == 504)
        return true;
    return s3_code != NULL && (strcmp(s3_code, "InternalError") == 0 ||
                               strcmp(s3_code, "RequestTimeout") == 0 ||
                               strcmp(s3_code, "SlowDown") == 0 ||
                               strcmp(s3_code, "ServiceUnavailable") == 0);
}

void s3_retry_delay(struct s3_client *client, unsigned attempt,
                    unsigned max_attempts, CURLcode code,
                    const struct s3_response *response,
                    const struct s3_error *error) {
    struct timespec delay;
    uint32_t random_bits;
    unsigned shift = attempt - 1 < 6 ? attempt - 1 : 6;
    uint64_t cap_ms = UINT64_C(250) << shift;
    randombytes(&random_bits, sizeof(random_bits));
    uint64_t milliseconds = ((uint64_t) random_bits * (cap_ms + 1)) >> 32;
    if (response != NULL && response->have_retry_after &&
        milliseconds < response->retry_after_ms)
        milliseconds = response->retry_after_ms;
    if (log_enabled(log_TRACE)) {
        char line[256];
        (void) snprintf(
            line, sizeof(line),
            "retry id=%llu next=%u/%u reason=%s%d "
            "delay_ms=%llu retry_after_ms=%llu",
            client->trace_id, attempt + 1, max_attempts,
            code != CURLE_OK ? "curl:" : "http:",
            code != CURLE_OK ? (int) code
                             : (int) (response != NULL ? response->status
                                                       : error->http_status),
            (unsigned long long) milliseconds,
            (unsigned long long) (response != NULL && response->have_retry_after
                                      ? response->retry_after_ms
                                      : 0));
        log_t1(line);
    }
    while (milliseconds != 0) {
        uint64_t slice = milliseconds < 50 ? milliseconds : 50;
        if (client->cancel_callback != NULL &&
            client->cancel_callback(client->cancel_data))
            return;
        delay.tv_sec = 0;
        delay.tv_nsec = (long) slice * 1000000L;
        while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
            if (client->cancel_callback == NULL) return;
            if (client->cancel_callback != NULL &&
                client->cancel_callback(client->cancel_data))
                return;
        }
        milliseconds -= slice;
    }
}
