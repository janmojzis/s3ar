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
static struct s3ar_config_env config;
static struct s3_error error = {0};
static enum s3_result result;
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

static bool list_all_bucket(void *callback_data,
                            const struct s3_bucket *bucket) {
    struct list_context *context = callback_data;
    output_name(bucket->name, NULL);
    struct s3_error error = {0};
    enum s3_result result = s3_object_list(context->s3, &error, bucket->name,
                                           NULL, list_object, context, NULL);
    if (result != S3_RESULT_OK) {
        log_f4("unable to list objects ", s3_log_uri(NULL, bucket->name, NULL),
               ": ", s3ar_log_error(&error));
        s3ar_list_exit(2);
    }
    return true;
}

static void list_selection(struct list_context *context,
                           const struct s3ar_selection *selection) {
    struct s3_client *s3 = context->s3;
    struct s3_error error = {0};
    enum s3_result result;
    if (selection->bucket == NULL) {
        result = s3_bucket_list(s3, &error, list_all_bucket, context);
        if (result != S3_RESULT_OK) {
            log_f2("unable to list buckets: ", s3ar_log_error(&error));
            s3ar_list_exit(2);
        }
        return;
    }
    output_name(selection->bucket, NULL);
    if (selection->key == NULL) {
        result = s3_object_list(s3, &error, selection->bucket, NULL,
                                list_object, context, NULL);
        if (result != S3_RESULT_OK) {
            log_f4("unable to list objects ",
                   s3_log_uri(NULL, selection->bucket, NULL), ": ",
                   s3ar_log_error(&error));
            s3ar_list_exit(2);
        }
        return;
    }

    struct s3_object_properties properties = {0};
    result = s3_object_head(s3, &error, &properties, selection->bucket,
                            selection->key);
    bool found = result == S3_RESULT_OK;
    if (found) {
        const struct s3_object object = {
            .bucket = selection->bucket,
            .key = selection->key,
            .size = properties.size,
            .last_modified = properties.last_modified,
            .etag = properties.etag,
        };
        output_object(&object, context->verbose);
        s3_object_properties_free(&properties);
    }
    else if (result != S3_RESULT_NOT_FOUND) {
        log_f4("unable to inspect object ",
               s3_log_uri(NULL, selection->bucket, selection->key), ": ",
               s3ar_log_error(&error));
        s3ar_list_exit(2);
    }
    size_t length = strlen(selection->key);
    if (length > SIZE_MAX - 2) {
        errno = ENOMEM;
        list_fatal("out of memory", NULL, NULL);
    }
    char *prefix = malloc(length + 2);
    if (prefix == NULL) { list_fatal("out of memory", NULL, NULL); }
    memcpy(prefix, selection->key, length);
    prefix[length] = '/';
    prefix[length + 1] = '\0';
    size_t descendants = 0;
    result = s3_object_list(s3, &error, selection->bucket, prefix, list_object,
                            context, &descendants);
    free(prefix);
    if (result != S3_RESULT_OK) {
        log_f4("unable to list objects ",
               s3_log_uri(NULL, selection->bucket, NULL), ": ",
               s3ar_log_error(&error));
        s3ar_list_exit(2);
    }
    if (!found && descendants == 0) {
        log_f2("not found ", selection->uri);
        s3ar_list_exit(2);
    }
}

static void s3ar_list_objects(const struct s3ar_config *config) {
    struct list_context context = {
        .s3 = config->s3,
        .verbose = config->verbose,
    };
    for (int i = 0; i < config->operand_count; ++i) {
        struct s3ar_selection selection;
        if (s3ar_selection_parse(&selection, config->operands[i]) != 0) {
            if (errno == EINVAL) {
                log_f2("invalid S3 operand ", config->operands[i]);
                s3ar_list_exit(2);
            }
            list_fatal("out of memory", NULL, NULL);
        }
        list_selection(&context, &selection);
        s3ar_selection_free(&selection);
    }
}

static void s3ar_list_buckets(const struct s3ar_config *config) {
    struct list_context context = {
        .s3 = config->s3,
        .verbose = config->verbose,
    };
    struct s3_error error = {0};
    enum s3_result result =
        s3_bucket_list(config->s3, &error, list_bucket_name, &context);
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
    else {
        for (int i = optind; i < argc; ++i) {
            if (strncmp(argv[i], "s3://", 5) != 0) {
                log_f2("invalid S3 operand: ", argv[i]);
                s3ar_list_exit(2);
            }
        }
    }
    list_config.operands = &argv[optind];
    list_config.operand_count = argc - optind;

    log_d3("(option -v) verbosity = '", log_num(verbosity), "'");
    log_d1("(option -h) help = 'false'");
    log_d3("(option -b) buckets = '", buckets ? "true" : "false", "'");
    for (int i = optind; i < argc; ++i)
        log_d3("(argument) selection = '", s3_log_uri("s3", argv[i] + 5, NULL),
               "'");

    /* configure S3 client */
    result = s3ar_config_from_env(&config, &error);
    if (result != S3_RESULT_OK) {
        log_f2("invalid configuration: ", s3ar_log_error(&error));
        s3ar_list_exit(2);
    }
    result = s3_client_open(&list_config.s3, &error, &config.client);
    if (result != S3_RESULT_OK) {
        log_f2("unable to initialize S3 client: ", s3ar_log_error(&error));
        s3ar_list_exit(1);
    }

    /* list matching resources */
    if (buckets)
        s3ar_list_buckets(&list_config);
    else
        s3ar_list_objects(&list_config);

    if (fflush(stdout) != 0) {
        log_f2("cannot flush standard output: ", log_errno());
        s3ar_list_exit(1);
    }
    s3ar_list_exit(0);
}
