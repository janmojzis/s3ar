/* SPDX-License-Identifier: MIT-0 */
#include "log.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

static volatile sig_atomic_t current_verbosity;
static const char *log_name;

struct log_part log_bytes(long long bytes) {
    return (struct log_part) {.kind = log_PART_BYTES, .value.number = bytes};
}

static void log_print_bytes(FILE *stream, long long bytes) {
    static const char *const units[] = {"B",   "KiB", "MiB", "GiB",
                                        "TiB", "PiB", "EiB"};
    long double value = bytes;
    size_t unit = 0;
    while ((value >= 1024 || value <= -1024) &&
           unit + 1 < sizeof(units) / sizeof(units[0])) {
        value /= 1024;
        ++unit;
    }
    long long hundredths =
        (long long) (value * 100 + (value < 0 ? -0.5L : 0.5L));
    if (hundredths % 100 == 0)
        (void) fprintf(stream, "%.0Lf %s", value, units[unit]);
    else
        (void) fprintf(stream, "%.2Lf %s", value, units[unit]);
}

struct log_part log_custom(log_format_fn format, const void *data) {
    return (struct log_part) {.kind = log_PART_CUSTOM,
                              .value.custom = {.format = format, .data = data}};
}

void log_set_name(const char *name) { log_name = name; }

const char *log_get_name(void) { return log_name; }

void log_inc_level(int signal_number) {
    (void) signal_number;
    sig_atomic_t verbosity = current_verbosity;
    if (verbosity < 3) current_verbosity = verbosity + 1;
}

void log_dec_level(int signal_number) {
    (void) signal_number;
    sig_atomic_t verbosity = current_verbosity;
    if (verbosity > 0) current_verbosity = verbosity - 1;
}

void log_usage(FILE *stream, const char *text) { (void) fputs(text, stream); }

bool log_enabled(enum log_level level) {
    switch (level) {
        case log_OUTPUT:
        case log_SUCCESS:
        case log_FATAL:
        case log_WARNING:
            return true;
        case log_INFO:
        case log_ERROR:
            return current_verbosity >= 1;
        case log_DEBUG:
            return current_verbosity >= 2;
        case log_TRACE:
            return current_verbosity >= 3;
    }
    return false;
}

static void log_print(enum log_level level, const struct log_part *parts,
                      size_t count) {
    int saved_errno = errno;
    FILE *stream = level == log_OUTPUT ? stdout : stderr;
    static const char *const level_names[] = {
        [log_SUCCESS] = "success", [log_FATAL] = "fatal", [log_ERROR] = "error",
        [log_WARNING] = "warning", [log_INFO] = "info",   [log_DEBUG] = "debug",
        [log_TRACE] = "trace",
    };
    if (level != log_OUTPUT) {
        if (log_name != NULL) (void) fprintf(stream, "%s: ", log_name);
        (void) fprintf(stream, "%s: ", level_names[level]);
    }
    for (size_t i = 0; i < count; ++i) {
        if (parts[i].kind == log_PART_TEXT)
            (void) fputs(parts[i].value.text != NULL ? parts[i].value.text
                                                     : "(null)",
                         stream);
        else if (parts[i].kind == log_PART_CUSTOM &&
                 parts[i].value.custom.format != NULL)
            parts[i].value.custom.format(stream, parts[i].value.custom.data);
        else if (parts[i].kind == log_PART_ERRNO)
            (void) fputs(strerror(parts[i].value.error_number), stream);
        else if (parts[i].kind == log_PART_NUM)
            (void) fprintf(stream, "%lld", parts[i].value.number);
        else if (parts[i].kind == log_PART_BYTES)
            log_print_bytes(stream, parts[i].value.number);
    }
    (void) fputc('\n', stream);
    errno = saved_errno;
}

void log_emit(enum log_level level, const struct log_part *parts,
              size_t count) {
    if (log_enabled(level)) log_print(level, parts, count);
}
