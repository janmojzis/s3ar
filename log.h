#ifndef LOG_H____
#define LOG_H____

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>

enum log_level {
    log_OUTPUT,
    log_SUCCESS,
    log_FATAL,
    log_ERROR,
    log_WARNING,
    log_INFO,
    log_DEBUG,
    log_TRACE,
};

enum log_part_kind {
    log_PART_TEXT,
    log_PART_CUSTOM,
    log_PART_ERRNO,
    log_PART_NUM,
    log_PART_BYTES,
};

struct log_part {
    enum log_part_kind kind;
    union {
        const char *text;
        struct {
            void (*format)(FILE *stream, const void *data);
            const void *data;
        } custom;
        int error_number;
        long long number;
    } value;
};

static inline struct log_part log_text(const char *text) {
    return (struct log_part) {.kind = log_PART_TEXT, .value.text = text};
}

typedef void (*log_format_fn)(FILE *stream, const void *data);

struct log_part log_custom(log_format_fn format, const void *data);

static inline struct log_part log_errno(void) {
    return (struct log_part) {.kind = log_PART_ERRNO,
                              .value.error_number = errno};
}

static inline struct log_part log_num(long long number) {
    return (struct log_part) {.kind = log_PART_NUM, .value.number = number};
}

/* Binary units (B, KiB, MiB, ...); fractional values use two decimal places. */
struct log_part log_bytes(long long bytes);

static inline struct log_part log_part_identity(struct log_part part) {
    return part;
}

#define LOG_PART(value)                                                        \
    _Generic((value), struct log_part: log_part_identity, default: log_text)(  \
        value)

void log_set_name(const char *name);
const char *log_get_name(void);
void log_inc_level(int signal_number);
void log_dec_level(int signal_number);
void log_usage(FILE *stream, const char *text);
bool log_enabled(enum log_level level);
void log_emit(enum log_level level, const struct log_part *parts, size_t count);

#define LOG_RECORD(level, ...)                                                 \
    do {                                                                       \
        if (log_enabled(level)) {                                              \
            const struct log_part parts[] = {__VA_ARGS__};                     \
            log_emit(level, parts, sizeof(parts) / sizeof(parts[0]));          \
        }                                                                      \
    } while (0)

#define log_o1(a) LOG_RECORD(log_OUTPUT, LOG_PART(a))
#define log_o2(a, b) LOG_RECORD(log_OUTPUT, LOG_PART(a), LOG_PART(b))
#define log_o3(a, b, c)                                                        \
    LOG_RECORD(log_OUTPUT, LOG_PART(a), LOG_PART(b), LOG_PART(c))
#define log_o4(a, b, c, d)                                                     \
    LOG_RECORD(log_OUTPUT, LOG_PART(a), LOG_PART(b), LOG_PART(c), LOG_PART(d))
#define log_o5(a, b, c, d, e)                                                  \
    LOG_RECORD(log_OUTPUT, LOG_PART(a), LOG_PART(b), LOG_PART(c), LOG_PART(d), \
               LOG_PART(e))
#define log_o6(a, b, c, d, e, f)                                               \
    LOG_RECORD(log_OUTPUT, LOG_PART(a), LOG_PART(b), LOG_PART(c), LOG_PART(d), \
               LOG_PART(e), LOG_PART(f))
#define log_o7(a, b, c, d, e, f, g)                                            \
    LOG_RECORD(log_OUTPUT, LOG_PART(a), LOG_PART(b), LOG_PART(c), LOG_PART(d), \
               LOG_PART(e), LOG_PART(f), LOG_PART(g))
#define log_o8(a, b, c, d, e, f, g, h)                                         \
    LOG_RECORD(log_OUTPUT, LOG_PART(a), LOG_PART(b), LOG_PART(c), LOG_PART(d), \
               LOG_PART(e), LOG_PART(f), LOG_PART(g), LOG_PART(h))

#define log_f1(a) LOG_RECORD(log_FATAL, LOG_PART(a))
#define log_f2(a, b) LOG_RECORD(log_FATAL, LOG_PART(a), LOG_PART(b))
#define log_f3(a, b, c)                                                        \
    LOG_RECORD(log_FATAL, LOG_PART(a), LOG_PART(b), LOG_PART(c))
#define log_f4(a, b, c, d)                                                     \
    LOG_RECORD(log_FATAL, LOG_PART(a), LOG_PART(b), LOG_PART(c), LOG_PART(d))
#define log_f5(a, b, c, d, e)                                                  \
    LOG_RECORD(log_FATAL, LOG_PART(a), LOG_PART(b), LOG_PART(c), LOG_PART(d),  \
               LOG_PART(e))
#define log_f6(a, b, c, d, e, f)                                               \
    LOG_RECORD(log_FATAL, LOG_PART(a), LOG_PART(b), LOG_PART(c), LOG_PART(d),  \
               LOG_PART(e), LOG_PART(f))

#define log_s1(a) LOG_RECORD(log_SUCCESS, LOG_PART(a))
#define log_s2(a, b) LOG_RECORD(log_SUCCESS, LOG_PART(a), LOG_PART(b))
#define log_s3(a, b, c)                                                        \
    LOG_RECORD(log_SUCCESS, LOG_PART(a), LOG_PART(b), LOG_PART(c))
#define log_s4(a, b, c, d)                                                     \
    LOG_RECORD(log_SUCCESS, LOG_PART(a), LOG_PART(b), LOG_PART(c), LOG_PART(d))
#define log_s5(a, b, c, d, e)                                                  \
    LOG_RECORD(log_SUCCESS, LOG_PART(a), LOG_PART(b), LOG_PART(c),             \
               LOG_PART(d), LOG_PART(e))
#define log_s6(a, b, c, d, e, f)                                               \
    LOG_RECORD(log_SUCCESS, LOG_PART(a), LOG_PART(b), LOG_PART(c),             \
               LOG_PART(d), LOG_PART(e), LOG_PART(f))

#define log_e1(a) LOG_RECORD(log_ERROR, LOG_PART(a))
#define log_e2(a, b) LOG_RECORD(log_ERROR, LOG_PART(a), LOG_PART(b))
#define log_e3(a, b, c)                                                        \
    LOG_RECORD(log_ERROR, LOG_PART(a), LOG_PART(b), LOG_PART(c))
#define log_e4(a, b, c, d)                                                     \
    LOG_RECORD(log_ERROR, LOG_PART(a), LOG_PART(b), LOG_PART(c), LOG_PART(d))

#define log_w1(a) LOG_RECORD(log_WARNING, LOG_PART(a))
#define log_w2(a, b) LOG_RECORD(log_WARNING, LOG_PART(a), LOG_PART(b))
#define log_w3(a, b, c)                                                        \
    LOG_RECORD(log_WARNING, LOG_PART(a), LOG_PART(b), LOG_PART(c))
#define log_w4(a, b, c, d)                                                     \
    LOG_RECORD(log_WARNING, LOG_PART(a), LOG_PART(b), LOG_PART(c), LOG_PART(d))

#define log_i1(a) LOG_RECORD(log_INFO, LOG_PART(a))
#define log_i2(a, b) LOG_RECORD(log_INFO, LOG_PART(a), LOG_PART(b))
#define log_i3(a, b, c)                                                        \
    LOG_RECORD(log_INFO, LOG_PART(a), LOG_PART(b), LOG_PART(c))
#define log_i6(a, b, c, d, e, f)                                               \
    LOG_RECORD(log_INFO, LOG_PART(a), LOG_PART(b), LOG_PART(c), LOG_PART(d),   \
               LOG_PART(e), LOG_PART(f))
#define log_i7(a, b, c, d, e, f, g)                                            \
    LOG_RECORD(log_INFO, LOG_PART(a), LOG_PART(b), LOG_PART(c), LOG_PART(d),   \
               LOG_PART(e), LOG_PART(f), LOG_PART(g))
#define log_i8(a, b, c, d, e, f, g, h)                                         \
    LOG_RECORD(log_INFO, LOG_PART(a), LOG_PART(b), LOG_PART(c), LOG_PART(d),   \
               LOG_PART(e), LOG_PART(f), LOG_PART(g), LOG_PART(h))

#define log_d1(a) LOG_RECORD(log_DEBUG, LOG_PART(a))
#define log_d2(a, b) LOG_RECORD(log_DEBUG, LOG_PART(a), LOG_PART(b))
#define log_d3(a, b, c)                                                        \
    LOG_RECORD(log_DEBUG, LOG_PART(a), LOG_PART(b), LOG_PART(c))
#define log_d4(a, b, c, d)                                                     \
    LOG_RECORD(log_DEBUG, LOG_PART(a), LOG_PART(b), LOG_PART(c), LOG_PART(d))
#define log_d5(a, b, c, d, e)                                                  \
    LOG_RECORD(log_DEBUG, LOG_PART(a), LOG_PART(b), LOG_PART(c), LOG_PART(d),  \
               LOG_PART(e))

#define log_t1(a) LOG_RECORD(log_TRACE, LOG_PART(a))
#define log_t2(a, b) LOG_RECORD(log_TRACE, LOG_PART(a), LOG_PART(b))
#define log_t3(a, b, c)                                                        \
    LOG_RECORD(log_TRACE, LOG_PART(a), LOG_PART(b), LOG_PART(c))
#define log_t4(a, b, c, d)                                                     \
    LOG_RECORD(log_TRACE, LOG_PART(a), LOG_PART(b), LOG_PART(c), LOG_PART(d))

#endif
