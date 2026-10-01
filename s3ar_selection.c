/* SPDX-License-Identifier: MIT-0 */

#include "s3ar_selection.h"
#include "log.h"
#include "s3ar_log.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

int s3ar_selection_parse(struct s3ar_selection *selection, const char *uri) {
    *selection = (struct s3ar_selection) {.uri = uri};
    if (strcmp(uri, "s3://") == 0) { return 0; }
    if (strncmp(uri, "s3://", 5) != 0 || uri[5] == '\0' || uri[5] == '/') {
        errno = EINVAL;
        return -1;
    }

    selection->storage = strdup(uri + 5);
    if (selection->storage == NULL) return -1;
    size_t length = strlen(selection->storage);
    while (length > 0 && selection->storage[length - 1] == '/') {
        selection->storage[--length] = '\0';
    }

    char *key = strchr(selection->storage, '/');
    selection->bucket = selection->storage;
    if (key != NULL) {
        *key++ = '\0';
        selection->key = key;
    }
    return 0;
}

void s3ar_selection_free(struct s3ar_selection *selection) {
    free(selection->storage);
    *selection = (struct s3ar_selection) {0};
}

enum s3ar_selection_match
s3ar_selection_match(const struct s3ar_selection *selection, const char *bucket,
                     const char *key) {
    if (selection->bucket == NULL) return S3AR_SELECTION_DIRECT;
    if (strcmp(selection->bucket, bucket) != 0) return S3AR_SELECTION_NONE;
    if (key == NULL)
        return selection->key == NULL ? S3AR_SELECTION_DIRECT
                                      : S3AR_SELECTION_PARENT;
    if (selection->key == NULL) return S3AR_SELECTION_DIRECT;
    size_t length = strlen(selection->key);
    return strcmp(selection->key, key) == 0 ||
                   (strncmp(selection->key, key, length) == 0 &&
                    key[length] == '/')
               ? S3AR_SELECTION_DIRECT
               : S3AR_SELECTION_NONE;
}

void s3ar_selection_set_free(struct s3ar_selection_set *set) {
    for (size_t i = 0; i < set->count; ++i) s3ar_selection_free(&set->items[i]);
    free(set->items);
    *set = (struct s3ar_selection_set) {0};
}

int s3ar_selection_set_parse(struct s3ar_selection_set *set, size_t count,
                             char *const *operands) {
    *set = (struct s3ar_selection_set) {0};
    if (count == 0) return 0;
    set->items = calloc(count, sizeof(*set->items));
    if (set->items == NULL) {
        log_f1("out of memory");
        s3ar_selection_set_free(set);
        errno = ENOMEM;
        return -1;
    }
    set->count = count;
    for (size_t i = 0; i < count; ++i) {
        if (s3ar_selection_parse(&set->items[i], operands[i]) != 0) {
            int saved_errno = errno;
            if (saved_errno == EINVAL)
                log_f2("invalid S3 operand: ", operands[i]);
            else
                log_f1("out of memory");
            s3ar_selection_set_free(set);
            errno = saved_errno;
            return -1;
        }
    }
    return 0;
}

bool s3ar_selection_set_match(struct s3ar_selection_set *set,
                              const char *bucket, const char *key) {
    if (set->count == 0) return true;
    bool selected = false;
    for (size_t i = 0; i < set->count; ++i) {
        enum s3ar_selection_match match =
            s3ar_selection_match(&set->items[i], bucket, key);
        if (match != S3AR_SELECTION_NONE) selected = true;
        if (match == S3AR_SELECTION_DIRECT) set->items[i].matched = true;
    }
    return selected;
}

struct walk_context {
    struct s3_client *client;
    const struct s3ar_selection *selection;
    const struct s3ar_selection_callbacks *callbacks;
    void *data;
    enum s3_result result;
    size_t matched;
};

enum s3_result s3ar_selection_buckets(struct s3_client *client,
                                      struct s3_error *error,
                                      const struct s3ar_selection *selection,
                                      s3_bucket_callback callback, void *data) {
    if (selection->bucket == NULL)
        return s3_bucket_list(client, error, callback, data);
    *error = (struct s3_error) {0};
    struct s3_bucket bucket = {.name = selection->bucket};
    if (callback(data, &bucket)) return S3_RESULT_OK;
    error->result = S3_RESULT_CALLBACK_ERROR;
    return error->result;
}

static bool walk_object(void *data, const struct s3_object *object) {
    struct walk_context *context = data;
    if (s3ar_selection_match(context->selection, object->bucket, object->key) !=
        S3AR_SELECTION_DIRECT)
        return true;
    ++context->matched;
    return context->callbacks->object(context->data, object);
}

static bool walk_bucket(void *data, const struct s3_bucket *bucket) {
    struct walk_context *context = data;
    const struct s3ar_selection *selection = context->selection;
    const struct s3ar_selection_callbacks *callbacks = context->callbacks;
    callbacks->bucket(context->data, bucket);
    struct s3_error error = {0};
    context->result =
        s3_object_list(context->client, &error, bucket->name, selection->key,
                       walk_object, context, NULL);
    if (context->result != S3_RESULT_OK) {
        log_f4("unable to list objects ", s3_log_uri(NULL, bucket->name, NULL),
               ": ", s3ar_log_error(&error));
        return false;
    }
    if (selection->key != NULL && context->matched == 0) {
        log_f2("not found ", selection->uri);
        context->result = S3_RESULT_NOT_FOUND;
        return false;
    }
    return true;
}

enum s3_result s3ar_selection_walk(
    struct s3_client *client, const struct s3ar_selection *selection,
    const struct s3ar_selection_callbacks *callbacks, void *data) {
    struct walk_context context = {
        .client = client,
        .selection = selection,
        .callbacks = callbacks,
        .data = data,
    };
    struct s3_error error = {0};
    enum s3_result result = s3ar_selection_buckets(client, &error, selection,
                                                   walk_bucket, &context);
    if (context.result != S3_RESULT_OK) return context.result;
    if (result != S3_RESULT_OK)
        log_f2("unable to list buckets: ", s3ar_log_error(&error));
    return result;
}
