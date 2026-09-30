/* SPDX-License-Identifier: MIT-0 */
#include "main.h"
#include "s3ar_config.h"
#include "s3ar.h"
#include "log.h"
#include "s3ar_log.h"
#include "sig.h"

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct bucket_names {
    char **items;
    size_t count;
    size_t capacity;
};

struct operation {
    struct s3_client *client;
    struct s3_error error;
    const struct s3ar_selection *selection;
    bool dry_run;
    bool error_diagnosed;
    size_t versions;
    size_t uploads;
    size_t buckets;
};

static void usage(FILE *out) {
    log_usage(
        out,
        "Usage: s3ar-delete [-v|-vv|-vvv] [--dry-run] s3://[BUCKET[/KEY]]\n"
        "KEY selects the exact key and objects below KEY/.\n"
        "A trailing slash does not change the selection.\n");
}

static enum s3_result fetch_page(struct operation *op, const char *bucket,
                                 bool uploads, const char *key_marker,
                                 const char *id_marker,
                                 struct s3_listing_page *page) {
    log_d3("listing ", uploads ? "uploads in " : "versions in ",
           s3_log_uri("s3", bucket, NULL));
    if (key_marker != NULL)
        log_d2("key marker: ", s3_log_uri(NULL, key_marker, NULL));
    if (id_marker != NULL) log_d2("id marker: ", id_marker);
    if (uploads)
        return s3_listing_uploads_page(op->client, &op->error, bucket,
                                       op->selection->key, key_marker,
                                       id_marker, page);
    return s3_listing_versions_page(op->client, &op->error, bucket,
                                    op->selection->key, key_marker, id_marker,
                                    page);
}

static enum s3_result delete_batch(struct operation *op, const char *bucket,
                                   const struct s3_listing_item *const *targets,
                                   size_t count) {
    struct s3_object_version_ref refs[S3_DELETE_BATCH_LIMIT];
    struct s3_object_delete_result results[S3_DELETE_BATCH_LIMIT] = {0};
    enum s3_result result;
    for (size_t i = 0; i < count; ++i)
        refs[i] = (struct s3_object_version_ref) {.key = targets[i]->key,
                                                  .version_id = targets[i]->id};
    result = s3_object_delete_batch(op->client, &op->error, bucket, refs, count,
                                    results);
    if (result != S3_RESULT_OK) {
        log_f4("unable to delete object versions in ",
               s3_log_uri("s3", bucket, count == 1 ? targets[0]->key : NULL),
               ": ", s3ar_log_error(&op->error));
        op->error_diagnosed = true;
    }
    else {
        for (size_t i = 0; i < count; ++i) {
            if (results[i].deleted) {
                log_i3(s3_log_uri("s3", bucket, refs[i].key),
                       " deleted version=", refs[i].version_id);
                ++op->versions;
                continue;
            }
            (void) snprintf(op->error.s3_code, sizeof(op->error.s3_code), "%s",
                            results[i].code);
            (void) snprintf(op->error.message, sizeof(op->error.message), "%s",
                            results[i].message);
            op->error.result = S3_RESULT_ERROR;
            log_f6("unable to delete object version ",
                   s3_log_uri("s3", bucket, refs[i].key),
                   " version=", s3_log_uri(NULL, refs[i].version_id, NULL),
                   ": ", s3ar_log_error(&op->error));
            op->error_diagnosed = true;
            result = S3_RESULT_ERROR;
        }
    }
    s3_object_delete_results_free(results, count);
    return result;
}

static enum s3_result scan_targets(struct operation *op, const char *bucket,
                                   bool uploads) {
    char *marker_key = NULL, *marker_id = NULL;
    enum s3_result result = S3_RESULT_OK;
    for (;;) {
        struct s3_listing_page page = {0};
        const struct s3_listing_item *batch[S3_DELETE_BATCH_LIMIT];
        size_t matched = 0;
        result = fetch_page(op, bucket, uploads, marker_key, marker_id, &page);
        if (result != S3_RESULT_OK) {
            s3_listing_page_free(&page);
            break;
        }
        for (size_t i = 0; i < page.count; ++i) {
            const struct s3_listing_item *target = &page.items[i];
            if (!s3ar_selection_matches(op->selection, bucket, target->key))
                continue;
            ++matched;
            if (!op->dry_run && !uploads) {
                batch[matched - 1] = target;
                continue;
            }
            if (!op->dry_run) {
                result =
                    uploads ? s3_multipart_abort(op->client, &op->error, bucket,
                                                 target->key, target->id)
                            : s3_object_delete_version(op->client, &op->error,
                                                       bucket, target->key,
                                                       target->id);
                if (result != S3_RESULT_OK) {
                    log_f4(uploads ? "unable to abort upload "
                                   : "unable to delete object version ",
                           s3_log_uri("s3", bucket, target->key), ": ",
                           s3ar_log_error(&op->error));
                    op->error_diagnosed = true;
                    break;
                }
            }
            log_i3(s3_log_uri("s3", bucket, target->key),
                   op->dry_run
                       ? (uploads ? " would abort upload="
                                  : " would delete version=")
                       : (uploads ? " aborted upload=" : " deleted version="),
                   target->id);
            if (uploads)
                ++op->uploads;
            else
                ++op->versions;
        }
        if (result == S3_RESULT_OK && !op->dry_run && !uploads && matched != 0)
            result = delete_batch(op, bucket, batch, matched);
        /* A server-side prefix can include neighbors outside the selection.
         * Continue past such pages even when there was nothing to delete. */
        bool advance = page.truncated && (op->dry_run || matched == 0);
        if (result == S3_RESULT_OK && advance) {
            if (marker_key != NULL && strcmp(marker_key, page.next_key) == 0 &&
                ((marker_id == NULL && page.next_id == NULL) ||
                 (marker_id != NULL && page.next_id != NULL &&
                  strcmp(marker_id, page.next_id) == 0))) {
                result = S3_RESULT_PROTOCOL_ERROR;
                op->error.result = result;
                (void) snprintf(op->error.message, sizeof(op->error.message),
                                "repeated listing marker");
            }
            else {
                free(marker_key);
                free(marker_id);
                marker_key = page.next_key;
                marker_id = page.next_id;
                page.next_key = page.next_id = NULL;
            }
        }
        if (!op->dry_run && matched != 0) {
            /* Rescan after deletion because the listing has changed. */
            free(marker_key);
            free(marker_id);
            marker_key = marker_id = NULL;
        }
        bool again = result == S3_RESULT_OK &&
                     (op->dry_run ? page.truncated : matched != 0 || advance);
        if (again && !op->dry_run)
            log_d2("rescanning after deletion in ",
                   s3_log_uri("s3", bucket, NULL));
        s3_listing_page_free(&page);
        if (!again) break;
    }
    free(marker_key);
    free(marker_id);
    return result;
}

static enum s3_result process_bucket(struct operation *op, const char *bucket) {
    enum s3_result result = scan_targets(op, bucket, true);
    if (result == S3_RESULT_OK) result = scan_targets(op, bucket, false);
    if (result == S3_RESULT_OK && op->selection->key == NULL) {
        if (op->dry_run)
            log_i2(s3_log_uri("s3", bucket, NULL), " would delete");
        else {
            result = s3_bucket_delete(op->client, &op->error, bucket);
            if (result == S3_RESULT_OK)
                log_i2(s3_log_uri("s3", bucket, NULL), " deleted");
        }
        if (result == S3_RESULT_OK) ++op->buckets;
    }
    return result;
}

static bool collect_bucket(void *data, const struct s3_bucket *bucket) {
    struct bucket_names *names = data;
    char **items;
    size_t capacity;
    if (names->count == names->capacity) {
        capacity = names->capacity == 0 ? 16 : names->capacity * 2;
        if (capacity < names->capacity ||
            capacity > SIZE_MAX / sizeof(*names->items))
            return false;
        items = realloc(names->items, capacity * sizeof(*items));
        if (items == NULL) return false;
        names->items = items;
        names->capacity = capacity;
    }
    names->items[names->count] = strdup(bucket->name);
    if (names->items[names->count] == NULL) return false;
    ++names->count;
    return true;
}

static void free_buckets(struct bucket_names *names) {
    for (size_t i = 0; i < names->count; ++i) free(names->items[i]);
    free(names->items);
}

static struct s3ar_selection selection;
static struct s3ar_config_env config;
static struct s3_client *client;
static struct bucket_names names;

static _Noreturn void die(int status) {
    free_buckets(&names);
    s3ar_selection_free(&selection);
    s3_client_close(client);
    s3ar_config_free(&config);
    exit(status);
}

int main_s3ar_delete(int argc, char **argv) {
    int verbosity = 0;
    bool dry_run = false;
    struct s3_error error = {0};
    enum s3_result result;
    int option;
    static const struct option options[] = {
        {"dry-run", no_argument, NULL, 'n'},
        {"verbose", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    log_set_name("s3ar-delete");

    /* ignore SIGPIPE */
    sig_ignore(SIGPIPE);

    /* adjust verbosity using signals */
    sig_catch(SIGUSR1, log_inc_level);
    sig_catch(SIGUSR2, log_dec_level);

    /* parse options */
    opterr = 0;
    while ((option = getopt_long(argc, argv, "nvh", options, NULL)) != -1) {
        if (option == 'n')
            dry_run = true;
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
    if (argc - optind != 1 ||
        s3ar_selection_parse(&selection, argv[optind]) != 0) {
        if (argc - optind == 1 && errno == ENOMEM) {
            log_f1("out of memory");
            die(2);
        }
        usage(stderr);
        die(2);
    }
    log_d3("(option -v) verbosity = '", log_num(verbosity), "'");
    log_d1("(option -h) help = 'false'");
    log_d3("(option -n) dry-run = '", dry_run ? "true" : "false", "'");
    log_d3("(argument) selection = '",
           s3_log_uri("s3", selection.bucket, selection.key), "'");

    /* configure S3 client */
    result = s3ar_config_from_env(&config, &error);
    if (result != S3_RESULT_OK) {
        log_f2("invalid configuration: ", s3ar_log_error(&error));
        die(2);
    }
    result = s3_client_open(&client, &error, &config.client);
    if (result != S3_RESULT_OK) {
        log_f2("unable to initialize S3 client: ", s3ar_log_error(&error));
        die(2);
    }

    /* collect selected buckets */
    if (selection.bucket == NULL) {
        result = s3_bucket_list(client, &error, collect_bucket, &names);
        if (result != S3_RESULT_OK) {
            log_f2("unable to list buckets: ", s3ar_log_error(&error));
            die(2);
        }
    }
    else {
        names.items = malloc(sizeof(*names.items));
        if (names.items == NULL) {
            log_f1("out of memory");
            die(2);
        }
        names.items[0] = strdup(selection.bucket);
        if (names.items[0] == NULL) {
            log_f1("out of memory");
            die(2);
        }
        names.count = 1;
    }

    /* delete matching resources */
    struct operation op = {
        .client = client, .selection = &selection, .dry_run = dry_run};
    for (size_t i = 0; i < names.count; ++i) {
        result = process_bucket(&op, names.items[i]);
        if (result != S3_RESULT_OK) {
            if (!op.error_diagnosed)
                log_f4("unable to process ",
                       s3_log_uri("s3", names.items[i], selection.key), ": ",
                       s3ar_log_error(&op.error));
            die(2);
        }
    }
    log_s6(log_num((long long) op.versions), " versions, ",
           log_num((long long) op.uploads), " uploads, ",
           log_num((long long) op.buckets), " buckets");
    die(0);
}
