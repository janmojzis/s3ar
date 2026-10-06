/* SPDX-License-Identifier: MIT-0 */
#include "s3_log.h"
#include "s3_internal.h"

#include <string.h>

void s3_log_format_uri(FILE *stream, const void *data) {
    const struct s3_log_uri_data *uri = data;
    static const char hex[] = "0123456789ABCDEF";
    if (uri->scheme != NULL) {
        log_write_text(stream, uri->scheme);
        (void) fputs("://", stream);
    }
    const char *names[] = {uri->bucket, uri->key};
    for (size_t part = 0; part < 2 && names[part] != NULL; ++part) {
        if (part != 0) (void) fputc('/', stream);
        for (const unsigned char *p = (const unsigned char *) names[part]; *p;
             ++p) {
            unsigned char c = *p;
            if (s3_uri_encode_isliteral(c, true))
                (void) fputc(c, stream);
            else {
                (void) fputc('%', stream);
                (void) fputc(hex[c >> 4], stream);
                (void) fputc(hex[c & 15], stream);
            }
        }
    }
}

static void s3_log_format_error(FILE *stream, const void *data) {
    const struct s3_error *error = data;
    if (error == NULL) {
        (void) fputs("(null)", stream);
        return;
    }
    const char *message = error->message[0] != '\0'
                              ? error->message
                              : s3_result_name(error->result);
    log_write_text(stream, message);
    if (error->s3_code[0] != '\0') {
        (void) fputs(" (S3 ", stream);
        log_write_text(stream, error->s3_code);
        (void) fputc(')', stream);
    }
    if (error->http_status != 0)
        (void) fprintf(stream, " [HTTP %ld]", error->http_status);
    if (error->request_id[0] != '\0') {
        (void) fputs(" [request ", stream);
        log_write_text(stream, error->request_id);
        (void) fputc(']', stream);
    }
    if (error->attempts > 1)
        (void) fprintf(stream, " [after %u attempts]", error->attempts);
    if (error->callback_errno != 0) {
        (void) fputs(": ", stream);
        log_write_text(stream, strerror(error->callback_errno));
    }
}

struct log_part s3_log_error(const struct s3_error *error) {
    return log_custom(s3_log_format_error, error);
}
