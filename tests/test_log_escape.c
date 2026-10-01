/* SPDX-License-Identifier: MIT-0 */
#include "log.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void test_all_bytes(void) {
    char input[256], expected[1024];
    size_t used = 0, size = 0;
    for (unsigned i = 0; i < 256; ++i) {
        input[i] = (char) i;
        if (i == '\\') {
            expected[used++] = '\\';
            expected[used++] = '\\';
        }
        else if (i >= 32 && i <= 126)
            expected[used++] = (char) i;
        else {
            int n = snprintf(expected + used, sizeof(expected) - used,
                             "\\x%02X", i);
            assert(n == 4);
            used += 4;
        }
    }
    char *output = NULL;
    FILE *stream = open_memstream(&output, &size);
    assert(stream != NULL);
    log_write_data(stream, input, sizeof(input));
    assert(fclose(stream) == 0);
    assert(size == used && memcmp(output, expected, used) == 0);
    free(output);
}

static void format_binary(FILE *stream, const void *data) {
    (void) data;
    (void) fputc('[', stream);
    log_write_data(stream, "\0\n\xff", 3);
    (void) fputc(']', stream);
}

static void test_record(void) {
    FILE *capture = tmpfile();
    assert(capture != NULL);
    assert(fflush(stderr) == 0);
    int saved = dup(STDERR_FILENO);
    assert(saved >= 0);
    assert(dup2(fileno(capture), STDERR_FILENO) == STDERR_FILENO);
    log_set_name("tool\033");
    errno = ENOENT;
    log_w4("a\n\t\\%20", log_custom(format_binary, NULL), log_num(-7),
           (const char *) NULL);
    assert(errno == ENOENT);
    assert(fflush(stderr) == 0);
    assert(dup2(saved, STDERR_FILENO) == STDERR_FILENO);
    assert(close(saved) == 0);
    rewind(capture);
    char output[256];
    size_t size = fread(output, 1, sizeof(output), capture);
    const char expected[] =
        "tool\\x1B: warning: a\\x0A\\x09\\\\%20[\\x00\\x0A\\xFF]-7(null)\n";
    assert(size == sizeof(expected) - 1);
    assert(memcmp(output, expected, size) == 0);
    assert(fclose(capture) == 0);
    log_set_name(NULL);
}

static void test_text_and_help(void) {
    char *output = NULL;
    size_t size = 0;
    FILE *stream = open_memstream(&output, &size);
    assert(stream != NULL);
    log_write_text(stream, "caf\xc3\xa9\\%2F");
    log_write_text(stream, NULL);
    log_usage(stream, "\nhelp\n");
    assert(fclose(stream) == 0);
    const char expected[] = "caf\\xC3\\xA9\\\\%2F(null)\nhelp\n";
    assert(size == sizeof(expected) - 1);
    assert(memcmp(output, expected, size) == 0);
    free(output);
}

int main(void) {
    test_all_bytes();
    test_record();
    test_text_and_help();
    return 0;
}
