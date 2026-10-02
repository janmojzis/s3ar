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
#include <strings.h>

static struct s3ar_config list_config;
static struct s3ar_selection_set selections;
static struct s3ar_config_env config;
static bool buckets;
static bool objects;
static bool bucket_acl;
static bool need_metadata;
static char *delimiter;
static size_t delimiter_size = 1;

struct output_field {
    int option;
    const char *name;
};
static struct output_field *fields;
static size_t field_count;

enum {
    OPT_OBJECT_SIZE = 256,
    OPT_OBJECT_MTIME,
    OPT_OBJECT_ETAG,
    OPT_BUCKET_ACL,
    OPT_OBJECT_METADATA,
    OPT_OBJECT_META,
    OPT_DELIMITER,
};
static int option;

static const struct option long_options[] = {
    {"buckets", no_argument, NULL, 'b'},
    {"objects", no_argument, NULL, 'o'},
    {"object-size", no_argument, NULL, OPT_OBJECT_SIZE},
    {"object-mtime", no_argument, NULL, OPT_OBJECT_MTIME},
    {"object-etag", no_argument, NULL, OPT_OBJECT_ETAG},
    {"object-metadata", no_argument, NULL, OPT_OBJECT_METADATA},
    {"object-meta", required_argument, NULL, OPT_OBJECT_META},
    {"delimiter", required_argument, NULL, OPT_DELIMITER},
    {"bucket-acl", no_argument, NULL, OPT_BUCKET_ACL},
    {"verbose", no_argument, NULL, 'v'},
    {"help", no_argument, NULL, 'h'},
    {NULL, 0, NULL, 0},
};

static void usage(void) {
    log_usage(stderr, "Usage: s3ar-list [-v] [-b] [-o] [FIELDS] s3://[BUCKET[/KEY]]...\n"
                      "List buckets and objects by default; -b lists only buckets,\n"
                      "-o only objects, and -bo both. Bucket-only mode requires s3://.\n"
                      "Fields: --object-size --object-mtime --object-etag --bucket-acl\n"
                      "        --object-metadata --object-meta NAME (repeatable)\n"
                      "  --delimiter STRING  Field separator (default: space)\n"
                      "  -b, --buckets   Include buckets\n"
                      "  -o, --objects   Include objects\n"
                      "  -v, --verbose   Increase diagnostic verbosity only\n"
                      "  -h, --help      Show this help\n");
}

static _Noreturn void s3ar_list_exit(int status) {
    free(fields);
    free(delimiter);
    s3ar_selection_set_free(&selections);
    s3_client_close(list_config.s3);
    s3ar_config_free(&config);
    exit(status);
}

struct list_context {
    struct s3_client *s3;
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

static void output_separator(void) {
    const char *bytes = delimiter != NULL ? delimiter : " ";
    (void) fwrite(bytes, 1, delimiter_size, stdout);
}

static void set_delimiter(const char *value) {
    char *decoded = malloc(strlen(value) + 1);
    if (decoded == NULL) list_fatal("out of memory", NULL, NULL);
    size_t length = 0;
    for (const char *p = value; *p; ++p) {
        char c = *p;
        if (c == '\\') {
            switch (*++p) {
            case 't': c = '\t'; break;
            case 'n': c = '\n'; break;
            case '0': c = '\0'; break;
            case '\\': c = '\\'; break;
            default:
                free(decoded);
                log_f1("invalid escape in --delimiter");
                s3ar_list_exit(2);
            }
        }
        decoded[length++] = c;
    }
    free(delimiter);
    delimiter = decoded;
    delimiter_size = length;
}

static void add_field(int option, const char *name) {
    for (size_t i = 0; i < field_count; ++i) {
        if (fields[i].option == option &&
            (name == NULL || strcasecmp(fields[i].name, name) == 0))
            return;
    }
    fields[field_count++] = (struct output_field) {option, name};
    if (option == OPT_OBJECT_META || option == OPT_OBJECT_METADATA)
        need_metadata = true;
}

static void output_uri(const char *bucket, const char *key) {
    char encoded_bucket[S3_URI_ENCODED_MAX_BYTES];
    char encoded_key[S3_URI_ENCODED_MAX_BYTES];
    encode_name(encoded_bucket, encoded_key, bucket, key);
    fprintf(stdout, "s3://%s", encoded_bucket);
    if (key != NULL) fprintf(stdout, "/%s", encoded_key);
}

static void output_json_string(const char *value) {
    fputc('"', stdout);
    for (const unsigned char *p = (const unsigned char *) value; *p; ++p) {
        if (*p == '"' || *p == '\\') {
            fputc('\\', stdout);
            fputc(*p, stdout);
        }
        else if (*p < 0x20)
            fprintf(stdout, "\\u%04x", (unsigned) *p);
        else
            fputc(*p, stdout);
    }
    fputc('"', stdout);
}

static void output_metadata(const struct s3_object_properties *properties) {
    /* HEAD properties already contain metadata sorted by name. */
    fputc('{', stdout);
    for (size_t i = 0; i < properties->metadata_count; ++i) {
        const struct s3_metadata *meta = &properties->metadata[i];
        if (i > 0) fputc(',', stdout);
        output_json_string(meta->name);
        fputc(':', stdout);
        output_json_string(meta->value);
    }
    fputc('}', stdout);
}

static void output_object(const struct s3_object *object,
                          const struct s3_object_properties *properties) {
    output_uri(object->bucket, object->key);
    for (size_t i = 0; i < field_count; ++i) {
        const struct output_field *field = &fields[i];
        output_separator();
        switch (field->option) {
        case OPT_OBJECT_SIZE:
            fprintf(stdout, "%" PRIu64, object->size);
            break;
        case OPT_OBJECT_MTIME:
            fprintf(stdout, "%" PRId64, object->last_modified);
            break;
        case OPT_OBJECT_ETAG:
            if (object->etag != NULL) fputs(object->etag, stdout);
            break;
        case OPT_OBJECT_METADATA:
            output_metadata(properties);
            break;
        case OPT_OBJECT_META:
            for (size_t j = 0; j < properties->metadata_count; ++j) {
                if (strcasecmp(field->name, properties->metadata[j].name) == 0) {
                    fputs(properties->metadata[j].value, stdout);
                    break;
                }
            }
            break;
        }
    }
    fputc('\n', stdout);
}

static bool list_bucket_acl(void *callback_data,
                            const struct s3_bucket *bucket) {
    (void) callback_data;
    output_uri(bucket->name, NULL);
    output_separator();
    if (bucket->acl != NULL) fputs(bucket->acl, stdout);
    fputc('\n', stdout);
    return true;
}

static bool list_bucket_name(void *callback_data,
                             const struct s3_bucket *bucket) {
    struct list_context *context = callback_data;
    if (bucket_acl) {
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
    else {
        output_uri(bucket->name, NULL);
        fputc('\n', stdout);
    }
    return true;
}

static bool list_object(void *callback_data, const struct s3_object *object) {
    const struct list_context *context = callback_data;
    struct s3_object_properties properties = {0};
    if (need_metadata) {
        struct s3_error error = {0};
        if (s3_object_head(context->s3, &error, &properties, object->bucket,
                           object->key) != S3_RESULT_OK) {
            log_f4("unable to read object metadata ",
                   s3_log_uri(NULL, object->bucket, object->key), ": ",
                   s3ar_log_error(&error));
            s3_object_properties_free(&properties);
            s3ar_list_exit(2);
        }
    }
    output_object(object, &properties);
    s3_object_properties_free(&properties);
    return true;
}

static void list_selected_bucket(void *data, const struct s3_bucket *bucket) {
    if (buckets)
        (void) list_bucket_name(data, bucket);
}

static void s3ar_list_objects(const struct s3ar_config *config,
                              const struct s3ar_selection_set *selections) {
    struct list_context context = {
        .s3 = config->s3,
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

    fields = calloc((size_t) argc, sizeof(*fields));
    if (fields == NULL) list_fatal("out of memory", NULL, NULL);

    /* parse options */
    opterr = 0;
    while ((option = getopt_long(argc, argv, "bovh", long_options, NULL)) !=
           -1) {
        if (option == 'b') {
            if (buckets) {
                log_f1("bucket mode specified twice");
                s3ar_list_exit(2);
            }
            buckets = true;
        }
        else if (option == 'o') {
            if (objects) {
                log_f1("object mode specified twice");
                s3ar_list_exit(2);
            }
            objects = true;
        }
        else if (option == OPT_OBJECT_SIZE || option == OPT_OBJECT_MTIME ||
                 option == OPT_OBJECT_ETAG || option == OPT_OBJECT_METADATA)
            add_field(option, NULL);
        else if (option == OPT_OBJECT_META) {
            if (*optarg == '\0') {
                log_f1("--object-meta requires a nonempty metadata name");
                s3ar_list_exit(2);
            }
            add_field(option, optarg);
        }
        else if (option == OPT_DELIMITER)
            set_delimiter(optarg);
        else if (option == OPT_BUCKET_ACL)
            bucket_acl = true;
        else if (option == 'v') {
            if (verbosity < 3) ++verbosity;
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
    if (!buckets && !objects)
        buckets = objects = true;
    if (!objects && field_count > 0) {
        log_f1("object fields require object output");
        s3ar_list_exit(2);
    }
    if (!buckets && bucket_acl) {
        log_f1("--bucket-acl requires bucket output");
        s3ar_list_exit(2);
    }
    if (!objects) {
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
    log_d3("(option -o) objects = '", objects ? "true" : "false", "'");
    for (int i = optind; i < argc; ++i)
        log_d3("(argument) selection = '", s3_log_uri("s3", argv[i] + 5, NULL),
               "'");

    /* configure S3 client */
    int status = s3ar_client_open(&list_config.s3, &config);
    if (status != 0) s3ar_list_exit(status);

    /* list matching resources */
    if (!objects)
        s3ar_list_buckets(&list_config);
    else
        s3ar_list_objects(&list_config, &selections);

    if (fflush(stdout) != 0 || ferror(stdout)) {
        log_f2("cannot flush standard output: ", log_errno());
        s3ar_list_exit(1);
    }
    s3ar_list_exit(0);
}
