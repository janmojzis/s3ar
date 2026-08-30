/* SPDX-License-Identifier: MIT-0 */

#include "die.h"
#include "log.h"

#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include <stdio.h>
#include <wchar.h>
#include <wctype.h>

static const unsigned char escapes[128] = {
    ['\a'] = 'a', ['\b'] = 'b', ['\f'] = 'f', ['\n'] = 'n',
    ['\r'] = 'r', ['\t'] = 't', ['\v'] = 'v', ['\\'] = '\\',
};

int log_quote_name(FILE *stream, const char *name) {
    int saved_errno = errno;
    mbstate_t state = {0};
    const unsigned char *input = (const unsigned char *) name;
    size_t remaining = strlen(name);
    while (remaining > 0) {
        wchar_t character;
        size_t length = mbrtowc(&character, (const char *) input, remaining,
                                &state);
        bool valid = length != (size_t) -1 && length != (size_t) -2;
        if (!valid) {
            memset(&state, 0, sizeof(state));
            errno = saved_errno;
            length = 1;
        }
        if (length == 0) { break; }
        if (valid && length == 1 && *input < sizeof escapes &&
            escapes[*input] != 0) {
            if (fprintf(stream, "\\%c", escapes[*input]) < 0) { return -1; }
        }
        else if (valid && iswprint(character)) {
            if (fwrite(input, 1, length, stream) != length) { return -1; }
        }
        else {
            for (size_t i = 0; i < length; ++i) {
                if (fprintf(stream, "\\%03o", input[i]) < 0) { return -1; }
            }
        }
        input += length;
        remaining -= length;
    }
    errno = saved_errno;
    return 0;
}

void log_s3_name(FILE *stream, const char *bucket, const char *key) {
    int result = log_quote_name(stream, bucket);
    if (result >= 0 && key != NULL) {
        result = fputc('/', stream) == EOF ? -1 : log_quote_name(stream, key);
    }
    if (result >= 0) { result = fputc('\n', stream) == EOF ? -1 : 0; }
    if (result < 0) {
        die_fatal(stream == stdout ? "s3ar: unable to write standard output"
                                   : "s3ar: unable to write verbose output",
                  bucket, key);
    }
}

void log_s3_bucket(const struct s3_bucket *bucket, bool verbose) {
    int result = log_quote_name(stdout, bucket->name);
    if (result >= 0 && verbose) {
        result = fprintf(stdout, "\tacl=%s",
                         bucket->acl != NULL ? bucket->acl : "unavailable");
    }
    if (result >= 0) { result = fputc('\n', stdout) == EOF ? -1 : 0; }
    if (result < 0) {
        die_fatal("s3ar: unable to write standard output", bucket->name,
                  NULL);
    }
}

void log_s3_object(const struct s3_object *object, bool verbose) {
    if (!verbose) {
        log_s3_name(stdout, object->bucket, object->key);
        return;
    }
    int result = log_quote_name(stdout, object->bucket);
    if (result >= 0) { result = fputc('/', stdout) == EOF ? -1 : 0; }
    if (result >= 0) { result = log_quote_name(stdout, object->key); }
    if (result >= 0) {
        result = fprintf(stdout, "\t%" PRIu64 "\t", object->size);
    }
    for (size_t i = 0; result >= 0 && i < object->metadata_count; ++i) {
        result = fprintf(stdout, "%s%s=%s", i > 0 ? "," : "",
                         object->metadata[i].name,
                         object->metadata[i].value);
    }
    if (result >= 0 && object->metadata_count == 0) {
        result = fputc('-', stdout);
    }
    if (result >= 0) { result = fputc('\n', stdout); }
    if (result < 0) {
        die_fatal("s3ar: unable to write standard output", object->bucket,
                  object->key);
    }
}
