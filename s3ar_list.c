/* SPDX-License-Identifier: MIT-0 */

#include "die.h"
#include "log.h"
#include "s3.h"
#include "s3ar.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct list_context {
    struct s3_client *s3;
    bool verbose;
};

struct list_buckets_context {
    struct s3_client *s3;
    bool verbose;
};

static bool list_bucket_acl(void *callback_data,
                            const struct s3_bucket *bucket) {
    (void) callback_data;
    log_s3_bucket(bucket, true);
    return true;
}

static bool list_bucket_name(void *callback_data,
                             const struct s3_bucket *bucket) {
    struct list_buckets_context *context = callback_data;
    if (context->verbose) {
        struct s3_error error;
        enum s3_result result = s3_bucket_acl(
            context->s3, &error, bucket->name, list_bucket_acl, NULL);
        if (result != S3_RESULT_OK) {
            die_s3fatal("s3ar: unable to read bucket ACL", bucket->name, NULL,
                        result, &error);
        }
    }
    else { log_s3_bucket(bucket, false); }
    return true;
}

static bool log_object(void *callback_data, const struct s3_object *object) {
    const struct list_context *context = callback_data;
    log_s3_object(object, context->verbose);
    return true;
}

static bool list_all_bucket(void *callback_data,
                            const struct s3_bucket *bucket) {
    struct list_context *context = callback_data;
    struct s3_error error;
    enum s3_result result = s3_object_list(
        context->s3, &error, bucket->name, NULL, log_object, context, NULL);
    if (result != S3_RESULT_OK) {
        die_s3fatal("s3ar: unable to list objects", bucket->name, NULL,
                    result, &error);
    }
    return true;
}

static void list_selection(struct list_context *context,
                           const struct s3ar_selection *selection) {
    struct s3_client *s3 = context->s3;
    struct s3_error error;
    enum s3_result result;
    if (selection->bucket == NULL) {
        result = s3_bucket_list(s3, &error, list_all_bucket, context);
        if (result != S3_RESULT_OK) {
            die_s3fatal("s3ar: unable to list buckets", NULL, NULL, result,
                        &error);
        }
        return;
    }
    if (selection->key == NULL) {
        result = s3_bucket_head(s3, &error, selection->bucket);
        if (result != S3_RESULT_OK) {
            die_s3fatal("s3ar: unable to access bucket", selection->bucket,
                        NULL, result, &error);
        }
        result = s3_object_list(s3, &error, selection->bucket, NULL,
                                log_object, context, NULL);
        if (result != S3_RESULT_OK) {
            die_s3fatal("s3ar: unable to list objects", selection->bucket,
                        NULL, result, &error);
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
        log_s3_object(&object, context->verbose);
        s3_object_properties_free(&properties);
    }
    else if (result != S3_RESULT_NOT_FOUND) {
        die_s3fatal("s3ar: unable to inspect object", selection->bucket,
                    selection->key, result, &error);
    }
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
    size_t descendants = 0;
    result = s3_object_list(s3, &error, selection->bucket, prefix, log_object,
                            context, &descendants);
    free(prefix);
    if (result != S3_RESULT_OK) {
        die_s3fatal("s3ar: unable to list objects", selection->bucket, NULL,
                    result, &error);
    }
    if (!found && descendants == 0) {
        errno = 0;
        die_fatal("s3ar: not found", selection->uri, NULL);
    }
}

void s3ar_list_objects(const struct s3ar_config *config) {
    struct list_context context = {
        .s3 = config->s3,
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
        .s3 = config->s3,
        .verbose = config->verbose,
    };
    struct s3_error error;
    enum s3_result result =
        s3_bucket_list(config->s3, &error, list_bucket_name, &context);
    if (result != S3_RESULT_OK) {
        die_s3fatal("s3ar: unable to list buckets", NULL, NULL, result,
                    &error);
    }
}
