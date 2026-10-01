/* SPDX-License-Identifier: MIT-0 */
#include "log.h"
#include "s3ar_log.h"
#include "s3ar.h"
#include "main.h"
#include "s3ar_config.h"
#include "sig.h"

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct s3ar_config list_config;
static struct s3ar_selection_set selections;
static struct s3ar_config_env config;
static int buckets;
static int option;

static const struct option long_options[] = {
    {"buckets", no_argument, NULL, 'b'},
    {"verbose", no_argument, NULL, 'v'},
    {"help", no_argument, NULL, 'h'},
    {NULL, 0, NULL, 0},
};

static void usage(void) {
    log_usage(stderr, "Usage: s3ar-list [-v] [-b] s3://[BUCKET[/KEY]]...\n"
                      "List live S3 objects, or all buckets with -b s3://.\n");
}

static _Noreturn void s3ar_list_exit(int status) {
    s3ar_selection_set_free(&selections);
    s3_client_close(list_config.s3);
    s3ar_config_free(&config);
    exit(status);
}

struct list_context {
    struct s3_client *s3;
    bool verbose;
};

static _Noreturn void list_fatal(const char *text, const char *bucket,
                                 const char *key) {
    if (bucket != NULL) {
        if (errno != 0)
            log_f5(text, " ", s3_log_uri(NULL, bucket, key), ": ", log_errno());
        else
            log_f3(text, " ", s3_log_uri(NULL, bucket, key));
    }
    else if (errno != 0)
        log_f3(text, ": ", log_errno());
    else
        log_f1(text);
    s3ar_list_exit(2);
}

static void encode_name(char *encoded_bucket, char *encoded_key,
                        const char *bucket, const char *key) {
    if (!s3_uri_encode_into(encoded_bucket, S3_URI_ENCODED_MAX_BYTES, bucket,
                            0) ||
        (key != NULL &&
         !s3_uri_encode_into(encoded_key, S3_URI_ENCODED_MAX_BYTES, key, 1))) {
        errno = 0;
        list_fatal("invalid S3 name", bucket, key);
    }
}

static void output_name(const char *bucket, const char *key) {
    char encoded_bucket[S3_URI_ENCODED_MAX_BYTES];
    char encoded_key[S3_URI_ENCODED_MAX_BYTES];
    encode_name(encoded_bucket, encoded_key, bucket, key);
    if (key != NULL)
        log_o4("s3://", encoded_bucket, "/", encoded_key);
    else
        log_o2("s3://", encoded_bucket);
}

static void output_object(const struct s3_object *object, bool verbose) {
    if (!verbose) {
        output_name(object->bucket, object->key);
        return;
    }
    char encoded_bucket[S3_URI_ENCODED_MAX_BYTES];
    char encoded_key[S3_URI_ENCODED_MAX_BYTES];
    char details[96];
    encode_name(encoded_bucket, encoded_key, object->bucket, object->key);
    (void) snprintf(details, sizeof(details),
                    " size=%" PRIu64 " mtime=%" PRId64 " etag=", object->size,
                    object->last_modified);
    log_o6("s3://", encoded_bucket, "/", encoded_key, details,
           object->etag != NULL ? object->etag : "-");
}

static bool list_bucket_acl(void *callback_data,
                            const struct s3_bucket *bucket) {
    (void) callback_data;
    char name[S3_URI_ENCODED_MAX_BYTES];
    encode_name(name, NULL, bucket->name, NULL);
    log_o4("s3://", name,
           " acl=", bucket->acl != NULL ? bucket->acl : "unavailable");
    return true;
}

static bool list_bucket_name(void *callback_data,
                             const struct s3_bucket *bucket) {
    struct list_context *context = callback_data;
    if (context->verbose) {
        struct s3_error error = {0};
        enum s3_result result = s3_bucket_acl(context->s3, &error, bucket->name,
                                              list_bucket_acl, context);
        if (result != S3_RESULT_OK) {
            log_f4("unable to read bucket ACL ",
                   s3_log_uri(NULL, bucket->name, NULL), ": ",
                   s3ar_log_error(&error));
            s3ar_list_exit(2);
        }
    }
    else
        output_name(bucket->name, NULL);
    return true;
}

static bool list_object(void *callback_data, const struct s3_object *object) {
    const struct list_context *context = callback_data;
    output_object(object, context->verbose);
    return true;
}

static void list_selected_bucket(void *data, const struct s3_bucket *bucket) {
    (void) data;
    output_name(bucket->name, NULL);
}

static void s3ar_list_objects(const struct s3ar_config *config,
                              const struct s3ar_selection_set *selections) {
    struct list_context context = {
        .s3 = config->s3,
        .verbose = config->verbose,
    };
    const struct s3ar_selection_callbacks callbacks = {
        .bucket = list_selected_bucket,
        .object = list_object,
    };
    for (size_t i = 0; i < selections->count; ++i) {
        if (s3ar_selection_walk(config->s3, &selections->items[i], &callbacks,
                                &context) != S3_RESULT_OK)
            s3ar_list_exit(2);
    }
}

static void s3ar_list_buckets(const struct s3ar_config *config) {
    struct list_context context = {
        .s3 = config->s3,
        .verbose = config->verbose,
    };
    struct s3_error error = {0};
    enum s3_result result = s3ar_selection_buckets(
        config->s3, &error, &selections.items[0], list_bucket_name, &context);
    if (result != S3_RESULT_OK) {
        log_f2("unable to list buckets: ", s3ar_log_error(&error));
        s3ar_list_exit(2);
    }
}

int main_s3ar_list(int argc, char **argv) {
    int verbosity = 0;
    log_set_name("s3ar-list");

    /* ignore SIGPIPE */
    sig_ignore(SIGPIPE);

    /* adjust verbosity using signals */
    sig_catch(SIGUSR1, log_inc_level);
    sig_catch(SIGUSR2, log_dec_level);

    /* parse options */
    opterr = 0;
    while ((option = getopt_long(argc, argv, "bvh", long_options, NULL)) !=
           -1) {
        if (option == 'b') {
            if (buckets) {
                log_f1("bucket mode specified twice");
                s3ar_list_exit(2);
            }
            buckets = 1;
        }
        else if (option == 'v') {
            if (verbosity < 3) ++verbosity;
            list_config.verbose = true;
            log_inc_level(0);
        }
        else if (option == 'h') {
            log_d3("(option -v) verbosity = '", log_num(verbosity), "'");
            log_d1("(option -h) help = 'true'");
            usage();
            s3ar_list_exit(0);
        }
        else {
            usage();
            s3ar_list_exit(2);
        }
    }

    /* validate selection */
    if (buckets) {
        if (argc - optind != 1 || strcmp(argv[optind], "s3://") != 0) {
            log_f1("-b requires exactly s3://");
            s3ar_list_exit(2);
        }
    }
    else if (argc - optind < 1) {
        log_f1("at least one S3 operand is required");
        s3ar_list_exit(2);
    }
    list_config.operands = &argv[optind];
    list_config.operand_count = argc - optind;
    if (s3ar_selection_set_parse(&selections,
                                 (size_t) list_config.operand_count,
                                 list_config.operands) != 0)
        s3ar_list_exit(2);

    log_d3("(option -v) verbosity = '", log_num(verbosity), "'");
    log_d1("(option -h) help = 'false'");
    log_d3("(option -b) buckets = '", buckets ? "true" : "false", "'");
    for (int i = optind; i < argc; ++i)
        log_d3("(argument) selection = '", s3_log_uri("s3", argv[i] + 5, NULL),
               "'");

    /* configure S3 client */
    int status = s3ar_client_open(&list_config.s3, &config);
    if (status != 0) s3ar_list_exit(status);

    /* list matching resources */
    if (buckets)
        s3ar_list_buckets(&list_config);
    else
        s3ar_list_objects(&list_config, &selections);

    if (fflush(stdout) != 0) {
        log_f2("cannot flush standard output: ", log_errno());
        s3ar_list_exit(1);
    }
    s3ar_list_exit(0);
}
