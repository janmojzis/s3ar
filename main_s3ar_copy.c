/* SPDX-License-Identifier: MIT-0 */
#include "main.h"
#include "s3ar_client.h"
#include "s3ar_config.h"
#include "s3ar_parse.h"
#include "s3ar_interrupt.h"
#include "s3ar_log.h"
#include "log.h"
#include "sig.h"

#include <getopt.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct s3_client *client;
static struct s3ar_config_env config;
struct copy_source {
    struct s3_uri_buffer uri;
    bool recursive;
};

struct copied_key {
    struct copied_key *next;
    char key[];
};

static struct copy_source *sources;
static size_t source_count, source_index;
static struct s3_uri_buffer destination;
static struct copied_key **copied_keys;
static size_t copied_capacity, copied_count;
static size_t copied, skipped, failed;
static bool destination_prefix, dry_run;
static size_t part_size = S3_MULTIPART_PART_SIZE;
static struct s3_error error;
static volatile sig_atomic_t interrupted_signal;

static void handle_interrupt(int signal_number) {
    interrupted_signal = signal_number;
}

static void usage(FILE *stream) {
    log_usage(
        stream,
        "Usage: s3ar-copy [OPTIONS] SOURCE... DEST\n"
        "       s3ar-copy [OPTIONS] -t DEST SOURCE...\n"
        "Copy S3 objects on the server.\n"
        "  -r, -R, --recursive        Copy bucket or prefix contents\n"
        "  -t, --target-directory URI Copy into this bucket or prefix\n"
        "  -T, --no-target-directory  Treat DEST as an exact object key\n"
        "      --dry-run              Show copies without writing\n"
        "      --create-bucket        Create destination bucket if needed\n"
        "      --multipart-size SIZE  Threshold and part size (5M-5G)\n"
        "  -v, --verbose              Increase verbosity (up to -vvv)\n"
        "  -h, --help                 Show this help\n"
        "Use s3://BUCKET/KEY for objects; recursive prefixes end in /.\n");
}

static _Noreturn void die(int status) {
    if (status == 0 && interrupted_signal != 0) {
        log_f1("interrupted");
        status = 2;
    }
    for (size_t i = 0; i < copied_capacity; ++i) {
        struct copied_key *entry = copied_keys[i];
        while (entry != NULL) {
            struct copied_key *next = entry->next;
            free(entry);
            entry = next;
        }
    }
    free(copied_keys);
    free(sources);
    s3_client_close(client);
    s3ar_config_free(&config);
    exit(status);
}

/* Copy operands additionally allow a bucket root. Keep the shared object URI
 * parser strict so get/put and other object APIs still require a key. */
static void parse_operand(const char *text, struct s3_uri_buffer *uri) {
    const char *bucket = text + (strncmp(text, "s3://", 5) == 0 ? 5 : 0);
    const char *slash = strchr(bucket, '/');
    size_t bucket_size =
        slash != NULL ? (size_t) (slash - bucket) : strlen(bucket);
    const char *key = slash != NULL ? slash + 1 : "";
    if (strncmp(text, "s3://", 5) != 0 || bucket_size == 0 ||
        bucket_size >= sizeof(uri->bucket) || strlen(key) >= sizeof(uri->key)) {
        log_f2("invalid S3 operand: ", text);
        die(2);
    }
    memcpy(uri->bucket, bucket, bucket_size);
    uri->bucket[bucket_size] = '\0';
    strcpy(uri->key, key);
}

static bool ends_in_slash(const char *key) {
    size_t size = strlen(key);
    return size != 0 && key[size - 1] == '/';
}

static bool has_prefix(const char *key, const char *prefix) {
    return strncmp(key, prefix, strlen(prefix)) == 0;
}

static void validate_operand(const struct s3_uri_buffer *uri) {
    /* A dummy valid key validates the bucket under the configured URI style. */
    if (s3_url_validate_object_name(client, uri->bucket,
                                    uri->key[0] != '\0' ? uri->key : "/",
                                    &error) != S3_RESULT_OK) {
        log_f2("invalid operand: ", s3ar_log_error(&error));
        die(2);
    }
}

static size_t key_hash(const char *key) {
    size_t hash = 5381;
    for (const unsigned char *p = (const unsigned char *) key; *p; ++p)
        hash = hash * 33U + *p;
    return hash;
}

/* Only multi-source copies need a registry. Listings themselves remain paged;
 * the registry stores target keys, not source metadata or object data. */
static bool claim_key(const char *key) {
    if (source_count == 1) return true;
    if (copied_capacity == 0 || copied_count >= copied_capacity / 2) {
        size_t capacity = copied_capacity == 0 ? 64 : copied_capacity * 2;
        if (capacity < copied_capacity ||
            capacity > SIZE_MAX / sizeof(*copied_keys)) {
            log_f1("too many destination keys");
            die(2);
        }
        struct copied_key **table = calloc(capacity, sizeof(*table));
        if (table == NULL) {
            log_f1("out of memory");
            die(2);
        }
        for (size_t i = 0; i < copied_capacity; ++i) {
            struct copied_key *entry = copied_keys[i];
            while (entry != NULL) {
                struct copied_key *next = entry->next;
                size_t slot = key_hash(entry->key) % capacity;
                entry->next = table[slot];
                table[slot] = entry;
                entry = next;
            }
        }
        free(copied_keys);
        copied_keys = table;
        copied_capacity = capacity;
    }
    size_t slot = key_hash(key) % copied_capacity;
    for (struct copied_key *entry = copied_keys[slot]; entry != NULL;
         entry = entry->next) {
        if (strcmp(key, entry->key) == 0) {
            log_f2("destination key collision: ",
                   s3_log_uri("s3", destination.bucket, key));
            return false;
        }
    }
    struct copied_key *entry = malloc(sizeof(*entry) + strlen(key) + 1);
    if (entry == NULL) {
        log_f1("out of memory");
        die(2);
    }
    strcpy(entry->key, key);
    entry->next = copied_keys[slot];
    copied_keys[slot] = entry;
    ++copied_count;
    return true;
}

static const char *object_basename(const char *key) {
    size_t end = strlen(key);
    while (end != 0 && key[end - 1] == '/') --end;
    while (end != 0 && key[end - 1] != '/') --end;
    return key + end;
}

static bool copy_key(const char *key) {
    const struct copy_source *source = &sources[source_index];
    char target[sizeof(destination.key)];
    if (interrupted_signal != 0) return false;
    if (source->recursive && !has_prefix(key, source->uri.key)) {
        log_f1("listing returned a key outside the source prefix");
        ++failed;
        return false;
    }
    const char *suffix = source->recursive ? key + strlen(source->uri.key)
                                           : object_basename(key);
    if (destination_prefix) {
        if (strlen(destination.key) + strlen(suffix) >= sizeof(target)) {
            log_f2("destination key is too long for ",
                   s3_log_uri("s3", source->uri.bucket, key));
            ++failed;
            return true;
        }
        strcpy(target, destination.key);
        strcat(target, suffix);
    }
    else
        strcpy(target, destination.key);
    if (target[0] == '\0') {
        log_w2("skipping prefix marker that maps to an empty key: ",
               s3_log_uri("s3", source->uri.bucket, key));
        ++skipped;
        return true;
    }
    if (s3_url_validate_object_name(client, destination.bucket, target,
                                    &error) != S3_RESULT_OK) {
        log_f2("invalid destination: ", s3ar_log_error(&error));
        ++failed;
        return true;
    }
    /* Protect exact sources too, including operands that have not run yet. */
    for (size_t i = 0; i < source_count; ++i) {
        if (!sources[i].recursive &&
            strcmp(sources[i].uri.bucket, destination.bucket) == 0 &&
            strcmp(sources[i].uri.key, target) == 0) {
            log_f1(i == source_index
                       ? "source and destination are identical"
                       : "destination would overwrite another source");
            ++failed;
            return true;
        }
    }
    if (!claim_key(target)) {
        ++failed;
        return true;
    }
    enum s3_result result;
    if (dry_run) {
        if (!source->recursive) {
            struct s3_object_properties properties = {0};
            result = s3_object_head(client, &error, &properties,
                                    source->uri.bucket, key);
            s3_object_properties_free(&properties);
            if (result != S3_RESULT_OK) goto copy_failed;
        }
        log_o3(s3_log_uri("s3", source->uri.bucket, key), " -> ",
               s3_log_uri("s3", destination.bucket, target));
    }
    else {
        result = s3_object_copy(client, &error, source->uri.bucket, key,
                                destination.bucket, target, part_size);
        if (result != S3_RESULT_OK) goto copy_failed;
        log_i3(s3_log_uri("s3", source->uri.bucket, key), " to ",
               s3_log_uri("s3", destination.bucket, target));
    }
    ++copied;
    return interrupted_signal == 0;
copy_failed:
    log_f4("copy failed for ", s3_log_uri("s3", source->uri.bucket, key), ": ",
           s3ar_log_error(&error));
    ++failed;
    return interrupted_signal == 0;
}

static bool copy_listed_object(void *data, const struct s3_object *object) {
    (void) data;
    return copy_key(object->key);
}

int main_s3ar_copy(int argc, char **argv) {
    enum { OPTION_CREATE_BUCKET = 256, OPTION_MULTIPART_SIZE, OPTION_DRY_RUN };
    static const struct option long_options[] = {
        {"recursive", no_argument, NULL, 'r'},
        {"target-directory", required_argument, NULL, 't'},
        {"no-target-directory", no_argument, NULL, 'T'},
        {"dry-run", no_argument, NULL, OPTION_DRY_RUN},
        {"create-bucket", no_argument, NULL, OPTION_CREATE_BUCKET},
        {"multipart-size", required_argument, NULL, OPTION_MULTIPART_SIZE},
        {"verbose", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    bool recursive = false, exact_destination = false;
    bool create_bucket = false, part_size_seen = false;
    const char *target_directory = NULL;
    int verbosity = 0;
    int option;

    log_set_name("s3ar-copy");
    sig_ignore(SIGPIPE);
    sig_catch(SIGINT, handle_interrupt);
    sig_catch(SIGTERM, handle_interrupt);
    sig_catch(SIGUSR1, log_inc_level);
    sig_catch(SIGUSR2, log_dec_level);

    opterr = 0;
    while ((option = getopt_long(argc, argv, "rRt:Tvh", long_options, NULL)) !=
           -1) {
        if (option == 'r' || option == 'R')
            recursive = true;
        else if (option == 'T')
            exact_destination = true;
        else if (option == 't') {
            if (target_directory != NULL) {
                log_f1("target directory specified twice");
                die(2);
            }
            target_directory = optarg;
        }
        else if (option == OPTION_DRY_RUN)
            dry_run = true;
        else if (option == OPTION_CREATE_BUCKET)
            create_bucket = true;
        else if (option == OPTION_MULTIPART_SIZE) {
            if (part_size_seen) {
                log_f1("multipart size specified twice");
                die(2);
            }
            part_size_seen = true;
            if (!s3ar_parse_multipart_size(optarg, &part_size)) {
                log_f1("--multipart-size must be between 5M and 5G");
                die(2);
            }
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
    int operands = argc - optind;
    if ((target_directory == NULL && operands < 2) ||
        (target_directory != NULL && operands < 1)) {
        usage(stderr);
        die(2);
    }
    if (target_directory != NULL && exact_destination) {
        log_f1("-t and -T cannot be combined");
        die(2);
    }
    source_count = (size_t) (operands - (target_directory == NULL ? 1 : 0));
    sources = calloc(source_count, sizeof(*sources));
    if (sources == NULL) {
        log_f1("out of memory");
        die(2);
    }
    parse_operand(target_directory != NULL ? target_directory : argv[argc - 1],
                  &destination);
    destination_prefix = !exact_destination && (target_directory != NULL ||
                                                destination.key[0] == '\0' ||
                                                ends_in_slash(destination.key));
    if (exact_destination &&
        (source_count != 1 || destination.key[0] == '\0')) {
        log_f1("-T requires one source object and a destination object key");
        die(2);
    }
    if (destination_prefix && destination.key[0] != '\0' &&
        !ends_in_slash(destination.key)) {
        size_t size = strlen(destination.key);
        if (size + 1 >= sizeof(destination.key)) {
            log_f1("target directory is too long");
            die(2);
        }
        destination.key[size] = '/';
        destination.key[size + 1] = '\0';
    }
    if (source_count > 1 && !destination_prefix) {
        log_f1("multiple sources require a destination bucket or prefix ending "
               "in /");
        die(2);
    }
    for (size_t i = 0; i < source_count; ++i) {
        struct copy_source *source = &sources[i];
        parse_operand(argv[optind + (int) i], &source->uri);
        source->recursive = recursive && (source->uri.key[0] == '\0' ||
                                          ends_in_slash(source->uri.key));
        if (source->uri.key[0] == '\0' && !recursive) {
            log_f1("copying a bucket requires -r/--recursive");
            die(2);
        }
        if (source->recursive && !destination_prefix) {
            log_f1("recursive copies require a destination bucket or prefix "
                   "ending in /");
            die(2);
        }
        if (strcmp(source->uri.bucket, destination.bucket) == 0) {
            if (source->recursive &&
                (has_prefix(destination.key, source->uri.key) ||
                 has_prefix(source->uri.key, destination.key))) {
                log_f1("source and destination prefixes overlap");
                die(2);
            }
            if (!source->recursive && !destination_prefix &&
                strcmp(source->uri.key, destination.key) == 0) {
                log_f1("source and destination are identical");
                die(2);
            }
        }
    }

    log_d3("(option -v) verbosity = '", log_num(verbosity), "'");
    log_d1("(option -h) help = 'false'");
    log_d3("(option --create-bucket) create-bucket = '",
           create_bucket ? "true" : "false", "'");
    log_d3("(option -r) recursive = '", recursive ? "true" : "false", "'");
    log_d3("(option -T) no-target-directory = '",
           exact_destination ? "true" : "false", "'");
    log_d3("(option --dry-run) dry-run = '", dry_run ? "true" : "false", "'");
    for (size_t i = 0; i < source_count; ++i)
        log_d3("(argument) source = '",
               s3_log_uri("s3", sources[i].uri.bucket, sources[i].uri.key),
               "'");
    log_d3("(argument) destination = '",
           s3_log_uri("s3", destination.bucket, destination.key), "'");
    log_d5("(option --multipart-size) multipart-size = '",
           log_bytes((long long) part_size), "'; maximum object size = '",
           log_bytes((long long) part_size * 10000), "' (10000 parts)");

    if (s3ar_client_open(&client, &config) != 0) die(2);
    s3ar_interrupt_bind(client, &interrupted_signal);
    validate_operand(&destination);
    for (size_t i = 0; i < source_count; ++i) validate_operand(&sources[i].uri);
    if (create_bucket && !dry_run) {
        if (s3_bucket_ensure(client, &error, destination.bucket) !=
            S3_RESULT_OK) {
            log_f2("cannot ensure destination bucket: ",
                   s3ar_log_error(&error));
            die(2);
        }
    }
    for (source_index = 0; source_index < source_count; ++source_index) {
        const struct copy_source *source = &sources[source_index];
        if (interrupted_signal != 0) break;
        if (!source->recursive) {
            (void) copy_key(source->uri.key);
            continue;
        }
        struct s3_error listing_error = {0};
        size_t count = 0;
        enum s3_result result =
            s3_object_list(client, &listing_error, source->uri.bucket,
                           source->uri.key, copy_listed_object, NULL, &count);
        if (interrupted_signal != 0) break;
        if (result != S3_RESULT_OK) {
            log_f4("cannot list source ",
                   s3_log_uri("s3", source->uri.bucket, source->uri.key), ": ",
                   s3ar_log_error(&listing_error));
            ++failed;
        }
        else if (count == 0 && source->uri.key[0] != '\0') {
            log_f2("source prefix not found: ",
                   s3_log_uri("s3", source->uri.bucket, source->uri.key));
            ++failed;
        }
    }
    if (interrupted_signal != 0) {
        log_f1("interrupted");
        ++failed;
    }
    if (dry_run && (fflush(stdout) != 0 || ferror(stdout))) {
        log_f1("cannot write dry-run output");
        ++failed;
    }
    log_s6(log_num((long long) copied), dry_run ? " planned, " : " copied, ",
           log_num((long long) skipped), " skipped, ",
           log_num((long long) failed), " failed");
    die(failed != 0 ? 2 : 0);
}
