/* SPDX-License-Identifier: MIT-0 */

#include "die.h"
#include "s3.h"
#include "s3ar.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct list_context {
    const struct s3 *s3;
    bool verbose;
};

struct list_buckets_context {
    const struct s3 *s3;
    bool verbose;
};

static void write_bucket_name(const struct s3_bucket *bucket, bool verbose) {
    int result = verbose
                     ? fprintf(stdout, "%s\tacl=%s\n", bucket->name,
                               bucket->acl != NULL ? bucket->acl
                                                   : "unavailable")
                     : fprintf(stdout, "%s\n", bucket->name);
    if (result < 0) {
        die_fatal("s3ar: unable to write standard output", bucket->name,
                  NULL);
    }
}

static void list_bucket_acl(const struct s3_bucket *bucket,
                            void *callback_data) {
    (void) callback_data;
    write_bucket_name(bucket, true);
}

static void list_bucket_name(const struct s3_bucket *bucket,
                             void *callback_data) {
    struct list_buckets_context *context = callback_data;
    if (context->verbose) {
        s3_bucket_acl(context->s3, bucket->name, list_bucket_acl, NULL);
    }
    else { write_bucket_name(bucket, false); }
}

static void log_head_object(const struct s3_object *object,
                            void *callback_data) {
    const struct list_context *context = callback_data;
    log_s3_object(object, context->verbose);
}

static void log_object(const struct s3_object *object, void *callback_data) {
    struct list_context *context = callback_data;
    if (context->verbose) {
        s3_object_head(context->s3, object->bucket, object->key,
                       log_head_object, context);
    }
    else { log_s3_object(object, false); }
}

static void list_all_bucket(const struct s3_bucket *bucket,
                            void *callback_data) {
    struct list_context *context = callback_data;
    s3_object_list_prefix(context->s3, bucket->name, NULL, log_object, context);
}

static void list_selection(struct list_context *context,
                           const struct s3ar_selection *selection) {
    const struct s3 *s3 = context->s3;
    if (selection->bucket == NULL) {
        s3_bucket_list(s3, list_all_bucket, context);
        return;
    }
    if (selection->key == NULL) {
        s3_bucket_check(s3, selection->bucket);
        s3_object_list_prefix(s3, selection->bucket, NULL, log_object, context);
        return;
    }

    bool found = s3_object_head(s3, selection->bucket, selection->key,
                                log_head_object, context);
    size_t length = strlen(selection->key);
    if (length > SIZE_MAX - 2) {
        errno = ENOMEM;
        die_fatal("s3ar: out of memory", NULL, NULL);
    }
    char *prefix = malloc(length + 2);
    if (prefix == NULL) { die_fatal("s3ar: out of memory", NULL, NULL); }
    memcpy(prefix, selection->key, length);
    prefix[length] = '/';
    prefix[length + 1] = '\0';
    size_t descendants = s3_object_list_prefix(s3, selection->bucket, prefix,
                                               log_object, context);
    free(prefix);
    if (!found && descendants == 0) {
        errno = 0;
        die_fatal("s3ar: not found", selection->uri, NULL);
    }
}

void s3ar_list_objects(const struct s3ar_config *config) {
    struct list_context context = {
        .s3 = &config->s3,
        .verbose = config->verbose,
    };
    for (int i = 0; i < config->operand_count; ++i) {
        struct s3ar_selection selection;
        s3ar_selection_parse(&selection, config->operands[i]);
        list_selection(&context, &selection);
        s3ar_selection_free(&selection);
    }
}

void s3ar_list_buckets(const struct s3ar_config *config) {
    struct list_buckets_context context = {
        .s3 = &config->s3,
        .verbose = config->verbose,
    };
    s3_bucket_list(&config->s3, list_bucket_name, &context);
}
