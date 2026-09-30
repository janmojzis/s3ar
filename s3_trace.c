/* SPDX-License-Identifier: MIT-0 */
#include "s3_internal.h"
#include "log.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* Keep a trace line bounded even when a server or object name is unusually
 * long. */
enum { TRACE_LINE_LIMIT = 1024, TRACE_TARGET_LIMIT = 512 };

static void append_char(char *out, size_t capacity, size_t *used, char value) {
    if (*used + 1 < capacity) out[(*used)++] = value;
    out[*used] = '\0';
}

static void append_literal(char *out, size_t capacity, size_t *used,
                           const char *value) {
    for (; *value != '\0'; ++value) append_char(out, capacity, used, *value);
}

static void append_safe(char *out, size_t capacity, size_t *used,
                        const char *value, size_t length) {
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < length && *used + 3 < capacity; ++i) {
        unsigned char c = (unsigned char) value[i];
        if (c >= 0x21 && c <= 0x7e && c != '%')
            append_char(out, capacity, used, (char) c);
        else {
            append_char(out, capacity, used, '%');
            append_char(out, capacity, used, hex[c >> 4]);
            append_char(out, capacity, used, hex[c & 15]);
        }
    }
}

static bool numeric_parameter(const char *name, size_t length) {
    static const char *const names[] = {"partNumber", "list-type", "max-keys",
                                        "max-uploads", "max-buckets"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if (strlen(names[i]) == length && strncmp(name, names[i], length) == 0)
            return true;
    return false;
}

static void append_target(char *out, size_t capacity, size_t *used,
                          const char *target, size_t length) {
    const char *end = target + length;
    const char *query;
    /* Proxies may use an absolute request target. Omit authority and userinfo.
     */
    if (length >= 7 && strncmp(target, "http://", 7) == 0)
        target += 7;
    else if (length >= 8 && strncmp(target, "https://", 8) == 0)
        target += 8;
    if (target != end && target[0] != '/') {
        const char *slash = memchr(target, '/', (size_t) (end - target));
        target = slash != NULL ? slash : end;
    }
    query = memchr(target, '?', (size_t) (end - target));
    append_safe(out, capacity, used, target,
                (size_t) ((query != NULL ? query : end) - target));
    if (query == NULL) return;
    append_char(out, capacity, used, '?');
    for (const char *p = query + 1; p < end && *used + 20 < capacity;) {
        const char *next = memchr(p, '&', (size_t) (end - p));
        const char *last = next != NULL ? next : end;
        const char *equals = memchr(p, '=', (size_t) (last - p));
        size_t name_length = (size_t) ((equals != NULL ? equals : last) - p);
        if (p != query + 1) append_char(out, capacity, used, '&');
        append_safe(out, capacity, used, p, name_length);
        if (equals != NULL) {
            append_char(out, capacity, used, '=');
            if (numeric_parameter(p, name_length)) {
                bool digits = equals + 1 < last;
                for (const char *v = equals + 1; v < last; ++v)
                    if (!isdigit((unsigned char) *v)) digits = false;
                if (digits)
                    append_safe(out, capacity, used, equals + 1,
                                (size_t) (last - equals - 1));
                else
                    append_safe(out, capacity, used, "[redacted]", 10);
            }
            else
                append_safe(out, capacity, used, "[redacted]", 10);
        }
        p = next != NULL ? next + 1 : end;
    }
}

static int trace_debug(CURL *curl, curl_infotype type, char *data, size_t size,
                       void *context) {
    struct s3_client *client = context;
    const char *end = data + size;
    const char *line_end, *space, *target_end;
    char line[TRACE_LINE_LIMIT];
    size_t used;
    (void) curl;
    if (!client->trace_active || !log_enabled(log_TRACE)) return 0;
    if (type == CURLINFO_HEADER_IN) {
        if (size >= 5 && strncasecmp(data, "HTTP/", 5) == 0)
            client->trace_request_id[0] = '\0';
        else if (size >= 17 &&
                 strncasecmp(data, "x-amz-request-id:", 17) == 0) {
            const char *first = data + 17;
            while (first < end && (*first == ' ' || *first == '\t')) ++first;
            const char *last = end;
            while (last > first && (last[-1] == '\r' || last[-1] == '\n' ||
                                    last[-1] == ' ' || last[-1] == '\t'))
                --last;
            size_t length = (size_t) (last - first);
            if (length >= sizeof(client->trace_request_id))
                length = sizeof(client->trace_request_id) - 1;
            memcpy(client->trace_request_id, first, length);
            client->trace_request_id[length] = '\0';
        }
        return 0;
    }
    if (type != CURLINFO_HEADER_OUT) return 0;
    line_end = memchr(data, '\n', size);
    if (line_end == NULL) line_end = end;
    space = memchr(data, ' ', (size_t) (line_end - data));
    if (space == NULL) return 0;
    target_end = memchr(space + 1, ' ', (size_t) (line_end - space - 1));
    if (target_end == NULL) return 0;
    ++client->trace_sent;
    used = (size_t) snprintf(line, sizeof(line),
                             "http id=%llu attempt=%u/%u wire=%u ",
                             client->trace_id, client->trace_attempt,
                             client->trace_max_attempts, client->trace_sent);
    append_safe(line, sizeof(line), &used, data, (size_t) (space - data));
    append_char(line, sizeof(line), &used, ' ');
    append_target(line,
                  used + TRACE_TARGET_LIMIT < sizeof(line)
                      ? used + TRACE_TARGET_LIMIT
                      : sizeof(line),
                  &used, space + 1, (size_t) (target_end - space - 1));
    /* Only known non-secret request headers are useful at this level. */
    for (const char *p = line_end + (line_end < end); p < end;) {
        const char *last = memchr(p, '\n', (size_t) (end - p));
        if (last == NULL) last = end;
        const char *value_end = last;
        if (value_end > p && value_end[-1] == '\r') --value_end;
        if ((size_t) (value_end - p) > 7 && strncasecmp(p, "Range: ", 7) == 0) {
            append_literal(line, sizeof(line), &used, " range=");
            append_safe(line, sizeof(line), &used, p + 7,
                        (size_t) (value_end - p - 7));
        }
        else if ((size_t) (value_end - p) > 16 &&
                 strncasecmp(p, "Content-Length: ", 16) == 0) {
            append_literal(line, sizeof(line), &used, " content_length=");
            append_safe(line, sizeof(line), &used, p + 16,
                        (size_t) (value_end - p - 16));
        }
        p = last < end ? last + 1 : end;
    }
    log_t1(line);
    return 0;
}

void s3_trace_perform_start(struct s3_client *client, unsigned attempt,
                            unsigned max_attempts) {
    client->trace_active = log_enabled(log_TRACE);
    if (!client->trace_active) return;
    client->trace_sent = 0;
    client->trace_attempt = attempt;
    client->trace_max_attempts = max_attempts;
    client->trace_request_id[0] = '\0';
    if (attempt == 1 || client->trace_id == 0) ++client->trace_id;
    (void) clock_gettime(CLOCK_MONOTONIC, &client->trace_started);
    if (curl_easy_setopt(client->curl, CURLOPT_DEBUGFUNCTION, trace_debug) !=
            CURLE_OK ||
        curl_easy_setopt(client->curl, CURLOPT_DEBUGDATA, client) != CURLE_OK ||
        curl_easy_setopt(client->curl, CURLOPT_VERBOSE, 1L) != CURLE_OK)
        client->trace_active = false;
}

void s3_trace_perform_end(struct s3_client *client, unsigned attempt,
                          unsigned max_attempts, CURLcode code,
                          const struct s3_response *response,
                          const struct s3_error *error) {
    struct timespec now;
    curl_off_t uploaded = 0, downloaded = 0;
    long long elapsed_ms;
    char line[TRACE_LINE_LIMIT];
    size_t used;
    if (!client->trace_active) return;
    if (!log_enabled(log_TRACE)) {
        client->trace_active = false;
        return;
    }
    (void) clock_gettime(CLOCK_MONOTONIC, &now);
    elapsed_ms =
        (long long) (now.tv_sec - client->trace_started.tv_sec) * 1000 +
        (now.tv_nsec - client->trace_started.tv_nsec) / 1000000;
    (void) curl_easy_getinfo(client->curl, CURLINFO_SIZE_UPLOAD_T, &uploaded);
    (void) curl_easy_getinfo(client->curl, CURLINFO_SIZE_DOWNLOAD_T,
                             &downloaded);
    used = (size_t) snprintf(
        line, sizeof(line),
        "http result id=%llu attempt=%u/%u wire=%u status=%ld curl=%d "
        "duration_ms=%lld sent=%lld received=%lld",
        client->trace_id, attempt, max_attempts, client->trace_sent,
        response->status, (int) code, elapsed_ms, (long long) uploaded,
        (long long) downloaded);
    if (error != NULL && error->s3_code[0] != '\0') {
        append_literal(line, sizeof(line), &used, " s3=");
        append_safe(line, sizeof(line), &used, error->s3_code,
                    strnlen(error->s3_code, sizeof(error->s3_code)));
    }
    const char *request_id = client->trace_request_id[0] != '\0'
                                 ? client->trace_request_id
                             : error != NULL ? error->request_id
                                             : NULL;
    if (request_id != NULL && request_id[0] != '\0') {
        append_literal(line, sizeof(line), &used, " request_id=");
        append_safe(line, sizeof(line), &used, request_id,
                    strnlen(request_id, sizeof(client->trace_request_id)));
    }
    log_t1(line);
    client->trace_active = false;
}
