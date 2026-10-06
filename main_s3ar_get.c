/* SPDX-License-Identifier: MIT-0 */
#include "s3ar_io.h"
#include "fsyncfile.h"
#include "log.h"
#include "s3ar_log.h"
#include "s3ar.h"
#include "main.h"
#include "s3ar_client.h"
#include "s3ar_config.h"
#include "s3ar_hash.h"
#include "s3ar_interrupt.h"
#include "s3ar_xattr.h"
#include "sig.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void usage(FILE *stream) {
    log_usage(stream, "Usage: s3ar-get [-v|-vv] [--hash] [-f FILE [-t TEMP]] "
                      "s3://BUCKET/KEY\n"
                      "Download to FILE or standard output.\n");
}

static int fn_isreplaceable(const char *fn) {
    struct stat st;
    if (lstat(fn, &st) == 0) {
        if (S_ISREG(st.st_mode)) return 1;
        log_f2("output is not a regular file: ", fn);
        return 0;
    }
    if (errno == ENOENT) return 1;
    log_f4("unable to stat ", fn, ": ", log_errno());
    return 0;
}

static int tmpfn_open(char *tmpfn, const char *fn, const char *tmpfn_template) {
    if (tmpfn_template != NULL) {
        size_t length = strlen(fn);
        size_t template_length = strlen(tmpfn_template) + 1;
        if (length + template_length > PATH_MAX) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(tmpfn, fn, length);
        memcpy(tmpfn + length, tmpfn_template, template_length);
        return mkstemp(tmpfn);
    }
    return open(tmpfn, O_WRONLY | O_CREAT | O_EXCL, 0600);
}

static const char *fn = NULL;
static int fd = -1;
static char tmpfn[PATH_MAX];
static const char *tmpfn_template = ".tmp.XXXXXX";
static char cached_etag[256];
static char downloaded_etag[256];
static char encoded_bucket[S3_URI_ENCODED_MAX_BYTES];
static char encoded_key[S3_URI_ENCODED_MAX_BYTES];
static char xattr_value[S3_URI_ENCODED_MAX_BYTES];
static bool tmpfn_created = false;
static bool hash_enabled;
static struct sha512_ctx download_hash;
static char downloaded_hash[S3AR_HASH_TEXT_SIZE] = "none";
static uint64_t downloaded_size;
static off_t stdout_start = -1;
static struct s3ar_config_env config;
static struct s3_client *client = NULL;
static struct s3_uri_buffer uri;
static struct s3_error error = {0};
static enum s3_result result;
static volatile sig_atomic_t interrupted_signal;

static void handle_interrupt(int signal_number) {
    interrupted_signal = signal_number;
}

static int option;
static const struct option long_options[] = {
    {"hash", no_argument, NULL, 256},
    {"file", required_argument, NULL, 'f'},
    {"temporary", required_argument, NULL, 't'},
    {"verbose", no_argument, NULL, 'v'},
    {"help", no_argument, NULL, 'h'},
    {NULL, 0, NULL, 0},
};

static _Noreturn void die(int status) {
    if (status == 0 && interrupted_signal != 0) {
        log_f1("interrupted");
        status = 2;
    }
    s3_client_close(client);
    s3ar_config_free(&config);

    if (fn != NULL && fd >= 0) {
        if (close(fd) != 0) {
            if (status == 0) log_f4("cannot close ", fn, ": ", log_errno());
            status = 2;
        }
    }
    if (tmpfn_created && unlink(tmpfn) != 0 && status == 0) {
        log_f4("cannot remove ", tmpfn, ": ", log_errno());
        status = 2;
    }
    exit(status);
}

static void check_interrupted(void) {
    if (interrupted_signal == 0) return;
    log_f1("interrupted");
    die(2);
}

static bool etag_isvalid(const char *etag, size_t size) {
    if (size < 2 || etag[0] != '"' || etag[size - 1] != '"') return false;
    for (size_t i = 1; i + 1 < size; ++i) {
        unsigned char c = (unsigned char) etag[i];
        if (c < 32 || c == 127 || c == '"' || c == '\\') return false;
    }
    return true;
}

static bool read_xattr_exact(int source_fd, const char *name,
                             const char *expected) {
    size_t length = strlen(expected);
    ssize_t size = s3ar_xattr_get(source_fd, name, xattr_value, length + 1);
    if (size < 0) {
        if (errno == ENODATA)
            s3ar_xattr_debug(name, NULL, 0, "missing");
        else if (errno == ERANGE)
            s3ar_xattr_debug(name, NULL, 0, "mismatch");
        else
            log_w4("xattr ", name, ": read failed: ", log_errno());
        return false;
    }
    if (size != (ssize_t) length ||
        memcmp(xattr_value, expected, length) != 0) {
        s3ar_xattr_debug(name, xattr_value, (size_t) size, "mismatch");
        return false;
    }
    s3ar_xattr_debug(name, xattr_value, (size_t) size, "match");
    return true;
}

static bool verify_cached_hash(int source_fd) {
    char expected[S3AR_HASH_TEXT_SIZE], actual[S3AR_HASH_TEXT_SIZE];
    unsigned char buffer[65536];
    struct sha512_ctx hash;
    struct stat before, after;
    ssize_t size =
        s3ar_xattr_get(source_fd, "user.s3ar.hash", expected, sizeof(expected));
    if (size < 0 || !s3ar_hash_valid(expected, (size_t) size)) {
        log_w1(
            "xattr cache: SHA-512 unavailable or invalid; downloading again");
        return false;
    }
    expected[S3AR_HASH_TEXT_LENGTH] = '\0';
    if (fstat(source_fd, &before) != 0) goto read_error;
    sha512_init(&hash);
    for (;;) {
        check_interrupted();
        size = read(source_fd, buffer, sizeof(buffer));
        if (size < 0) {
            if (errno == EINTR) continue;
            goto read_error;
        }
        if (size == 0) break;
        sha512_update(&hash, (size_t) size, buffer);
    }
    if (fstat(source_fd, &after) != 0) goto read_error;
    s3ar_hash_text(&hash, actual);
    if (strcmp(expected, actual) != 0 || before.st_size != after.st_size ||
        before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
        before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
        before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
        before.st_ctim.tv_nsec != after.st_ctim.tv_nsec) {
        log_w1(
            "xattr cache: SHA-512 mismatch or file changed; downloading again");
        return false;
    }
    s3ar_xattr_debug("user.s3ar.hash", expected, S3AR_HASH_TEXT_LENGTH,
                     "verified");
    return true;
read_error:
    log_w2("xattr cache: cannot verify SHA-512: ", log_errno());
    return false;
}

static bool write_download(void *data, const unsigned char *buffer,
                           size_t size) {
    if (!s3ar_io_write(data, buffer, size)) return false;
    if (hash_enabled) sha512_update(&download_hash, size, buffer);
    downloaded_size += size;
    return true;
}

static void load_cached_etag(void) {
    int source_fd;
    struct stat st;
    ssize_t size;
    bool format_match, bucket_match, key_match;
    if (fn == NULL) return;
    cached_etag[0] = '\0';
    source_fd = open(fn, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (source_fd < 0) {
        if (errno == ENOENT)
            log_d1("xattr cache: output file does not exist");
        else
            log_w2("xattr cache: cannot open output: ", log_errno());
        return;
    }
    if (fstat(source_fd, &st) != 0) {
        log_w2("xattr cache: fstat failed: ", log_errno());
        goto close_source;
    }
    if (!S_ISREG(st.st_mode)) {
        log_w1("xattr cache: output file is not regular");
        goto close_source;
    }
    format_match = read_xattr_exact(source_fd, "user.s3ar.format",
                                    S3AR_XATTR_FORMAT_VERSION);
    bucket_match =
        read_xattr_exact(source_fd, "user.s3ar.bucket", encoded_bucket);
    key_match = read_xattr_exact(source_fd, "user.s3ar.key", encoded_key);
    if (!format_match || !bucket_match || !key_match) goto close_source;
    size = s3ar_xattr_get(source_fd, "user.s3ar.etag", cached_etag,
                          sizeof(cached_etag) - 1);
    if (size < 0) {
        if (errno == ENODATA)
            s3ar_xattr_debug("user.s3ar.etag", NULL, 0, "missing");
        else if (errno == ERANGE)
            log_w1("xattr user.s3ar.etag: too long");
        else
            log_w2("xattr user.s3ar.etag: read failed: ", log_errno());
        cached_etag[0] = '\0';
        goto close_source;
    }
    if (!etag_isvalid(cached_etag, (size_t) size)) {
        log_w1("xattr user.s3ar.etag: invalid");
        cached_etag[0] = '\0';
        goto close_source;
    }
    cached_etag[size] = '\0';
    s3ar_xattr_debug("user.s3ar.etag", cached_etag, (size_t) size, "valid");
    if (hash_enabled && !verify_cached_hash(source_fd)) cached_etag[0] = '\0';
close_source:
    if (close(source_fd) != 0)
        log_w2("xattr cache: close failed: ", log_errno());
}

static bool remember_etag(void *data,
                          const struct s3_object_properties *properties) {
    (void) data;
    memcpy(downloaded_etag, properties->etag, sizeof(downloaded_etag));
    downloaded_etag[sizeof(downloaded_etag) - 1] = '\0';
    return true;
}

static bool output_accepts_xattrs(void) {
    if (fn != NULL) return true;
    struct stat st;
    if (fstat(fd, &st) != 0) {
        log_w2("cannot inspect stdout for xattrs: ", log_errno());
        return false;
    }
    if (!S_ISREG(st.st_mode)) return false;
    int flags = fcntl(fd, F_GETFL);
    if (stdout_start != 0 || flags < 0 || (flags & O_APPEND) != 0 ||
        st.st_size < 0 || (uint64_t) st.st_size != downloaded_size ||
        lseek(fd, 0, SEEK_CUR) != st.st_size) {
        log_w1("stdout does not contain exactly the downloaded object; "
               "skipping xattrs");
        return false;
    }
    return true;
}

static void save_xattrs(void) {
    (void) s3ar_xattr_set(fd, "user.s3ar.hash", downloaded_hash,
                          strlen(downloaded_hash));
    if (!etag_isvalid(downloaded_etag, strlen(downloaded_etag))) {
        log_w1("response ETag missing or invalid; skipping identity xattrs");
        return;
    }
    (void) s3ar_xattr_set(fd, "user.s3ar.format", S3AR_XATTR_FORMAT_VERSION,
                          sizeof(S3AR_XATTR_FORMAT_VERSION) - 1);
    (void) s3ar_xattr_set(fd, "user.s3ar.bucket", encoded_bucket,
                          strlen(encoded_bucket));
    (void) s3ar_xattr_set(fd, "user.s3ar.key", encoded_key,
                          strlen(encoded_key));
    (void) s3ar_xattr_set(fd, "user.s3ar.etag", downloaded_etag,
                          strlen(downloaded_etag));
}

int main_s3ar_get(int argc, char **argv) {
    int verbosity = 0;

    log_set_name("s3ar-get");

    /* ignore SIGPIPE */
    sig_ignore(SIGPIPE);
    sig_catch(SIGINT, handle_interrupt);
    sig_catch(SIGTERM, handle_interrupt);

    /* adjust verbosity using signals */
    sig_catch(SIGUSR1, log_inc_level);
    sig_catch(SIGUSR2, log_dec_level);

    /* parse options */
    opterr = 0;
    while ((option = getopt_long(argc, argv, "f:t:vh", long_options, NULL)) !=
           -1) {
        if (option == 256)
            hash_enabled = true;
        else if (option == 'f') {
            if (fn != NULL) {
                log_f1("output file specified twice");
                die(2);
            }
            fn = optarg;
        }
        else if (option == 't') {
            if (tmpfn_template == NULL) {
                log_f1("temporary file specified twice");
                die(2);
            }
            size_t length = strlen(optarg);
            if (length + 1 > PATH_MAX) {
                log_f1("temporary path is too long");
                die(2);
            }
            memcpy(tmpfn, optarg, length + 1);
            tmpfn_template = NULL;
        }
        else if (option == 'v') {
            if (verbosity < 3) ++verbosity;
            log_inc_level(0);
        }
        else if (option == 'h') {
            log_d3("(option -v) verbosity = '", log_num(verbosity), "'");
            log_d1("(option -h) help = 'true'");
            usage(stdout);
            die(0);
        }
        else {
            usage(stderr);
            die(2);
        }
    }
    if (argc - optind != 1) {
        usage(stderr);
        die(2);
    }

    /* validate output paths */
    if (fn == NULL || strcmp(fn, "-") == 0) fn = NULL;
    if (tmpfn_template == NULL && fn == NULL) {
        log_f1("-t requires -f FILE");
        die(2);
    }
    if (tmpfn_template == NULL &&
        (tmpfn[0] == '\0' || strcmp(tmpfn, fn) == 0)) {
        log_f1("temporary file must differ from output");
        die(2);
    }

    /* parse source URI */
    result = s3_uri_parse_into(argv[optind], &uri, &error);
    if (result != S3_RESULT_OK) {
        log_f2("invalid source: ", s3ar_log_error(&error));
        die(2);
    }
    /* prepare xattr identity */
    if (!s3_uri_encode_into(encoded_bucket, sizeof(encoded_bucket), uri.bucket,
                            0)) {
        log_f2("unable to encode bucket from ",
               s3_log_uri("s3", uri.bucket, uri.key));
        die(2);
    }
    if (!s3_uri_encode_into(encoded_key, sizeof(encoded_key), uri.key, 1)) {
        log_f2("unable to encode key from ",
               s3_log_uri("s3", uri.bucket, uri.key));
        die(2);
    }
    log_d3("(option -v) verbosity = '", log_num(verbosity), "'");
    log_d1("(option -h) help = 'false'");
    log_d3("(option -f) output-file = '", fn != NULL ? fn : "-", "'");
    log_d3("(argument) source = '", s3_log_uri("s3", uri.bucket, uri.key), "'");

    /* configure S3 client */
    if (s3ar_client_open(&client, &config) != 0) die(2);
    s3ar_interrupt_bind(client, &interrupted_signal);
    result = s3_url_validate_object_name(client, uri.bucket, uri.key, &error);
    if (result != S3_RESULT_OK) {
        log_f2("invalid source: ", s3ar_log_error(&error));
        die(2);
    }

    /* inspect and open output */
    check_interrupted();
    if (fn != NULL && !fn_isreplaceable(fn)) die(2);
    load_cached_etag();
    if (fn != NULL) {
        fd = tmpfn_open(tmpfn, fn, tmpfn_template);
        if (fd < 0) {
            log_f4("cannot open ", tmpfn[0] != '\0' ? tmpfn : fn, ": ",
                   log_errno());
            die(2);
        }
        tmpfn_created = true;
    }
    else
        fd = STDOUT_FILENO;
    log_d3("(option -t) temporary-file = '", fn != NULL ? tmpfn : "(none)",
           "'");

    /* Redirection preserves xattrs. Clear old s3ar metadata before writing,
     * even without --hash or if the download subsequently fails. */
    if (fn == NULL) {
        struct stat st;
        if (fstat(fd, &st) != 0) {
            log_f2("cannot inspect stdout: ", log_errno());
            die(2);
        }
        if (S_ISREG(st.st_mode) &&
            s3ar_xattr_reset(fd, &interrupted_signal) != 0) {
            check_interrupted();
            log_f2("cannot reset stdout s3ar xattrs: ", log_errno());
            die(2);
        }
    }

    if (hash_enabled) sha512_init(&download_hash);
    if (fn == NULL) stdout_start = lseek(fd, 0, SEEK_CUR);

    /* download or reuse cached output */
    if (cached_etag[0] != '\0')
        log_d2("If-None-Match: ", cached_etag);
    else
        log_d1("conditional GET disabled: no valid cached ETag");
    struct s3ar_io_write_context output = {.fd = &fd,
                                           .interrupted = &interrupted_signal};
    result = s3_object_get_conditional(
        client, &error, remember_etag, write_download, &output, uri.bucket,
        uri.key, cached_etag[0] != '\0' ? cached_etag : NULL);
    check_interrupted();
    if (result == S3_RESULT_NOT_MODIFIED) {
        log_d1("S3 object not modified; keeping output");
        if (fn != NULL) {
            int output_fd = fd;
            fd = -1;
            if (close(output_fd) != 0) {
                log_f4("cannot close ", tmpfn, ": ", log_errno());
                die(2);
            }
            if (unlink(tmpfn) != 0) {
                log_f4("cannot remove ", tmpfn, ": ", log_errno());
                die(2);
            }
            tmpfn_created = false;
        }
        log_i2(s3_log_uri("s3", uri.bucket, uri.key), " not modified");
        goto success;
    }
    if (result != S3_RESULT_OK) {
        log_f2("download failed: ", s3ar_log_error(&error));
        die(2);
    }

    if (hash_enabled) {
        s3ar_hash_text(&download_hash, downloaded_hash);
        log_i2("download hash: ", downloaded_hash);
    }

    /* store xattrs before closing output */
    if (output_accepts_xattrs()) save_xattrs();

    /* sync and close output before reporting success */
    const char *output_name = fn != NULL ? tmpfn : "standard output";
    int output_fd = fd;
    if (fsyncfile(output_fd) != 0) {
        log_f4("cannot sync ", output_name, ": ", log_errno());
        die(2);
    }
    fd = -1;
    if (close(output_fd) != 0) {
        log_f4("cannot close ", output_name, ": ", log_errno());
        die(2);
    }

    /* rename the completed temporary file */
    check_interrupted();
    if (fn != NULL) {
        if (!fn_isreplaceable(fn)) die(2);
        if (rename(tmpfn, fn) != 0) {
            log_f6("unable to rename ", tmpfn, " to ", fn, ": ", log_errno());
            die(2);
        }
        tmpfn_created = false;
    }
    log_i2(s3_log_uri("s3", uri.bucket, uri.key), " downloaded");

success:
    check_interrupted();
    log_s3(s3_log_uri("s3", uri.bucket, uri.key), " to ",
           fn != NULL ? fn : "stdout");
    die(0);
}
