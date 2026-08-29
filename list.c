/* SPDX-License-Identifier: MIT-0 */

#include "die.h"
#include "s3.h"
#include "s3ar.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void log_object(const struct s3_object *object, void *callback_data) {
    (void) callback_data;
    log_s3_name(object->bucket, object->key);
}

static void log_bucket(const struct s3_bucket *bucket, void *callback_data) {
    (void) callback_data;
    log_s3_name(bucket->name, NULL);
}

static void list_all_bucket(const struct s3_bucket *bucket,
                            void *callback_data) {
    const struct s3 *s3 = callback_data;
    log_bucket(bucket, NULL);
    s3_object_list_prefix(s3, bucket->name, NULL, log_object, NULL);
}

static void list_uri(const struct s3 *s3, const char *uri) {
    if (strcmp(uri, "s3://") == 0) {
        s3_bucket_list(s3, list_all_bucket, (void *) s3);
        return;
    }
    if (strncmp(uri, "s3://", 5) != 0 || uri[5] == '\0' || uri[5] == '/') {
        errno = 0;
        die_fatal("s3ar: invalid S3 operand", uri, NULL);
    }

    char *member = strdup(uri + 5);
    if (member == NULL) { die_fatal("s3ar: out of memory", NULL, NULL); }
    size_t length = strlen(member);
    while (length > 0 && member[length - 1] == '/') { member[--length] = '\0'; }

    char *key = strchr(member, '/');
    if (key == NULL) {
        s3_bucket_check(s3, member);
        const struct s3_bucket bucket = {
            .name = member,
            .creation_date = -1,
        };
        log_bucket(&bucket, NULL);
        s3_object_list_prefix(s3, member, NULL, log_object, NULL);
        free(member);
        return;
    }

    *key++ = '\0';
    bool found = s3_object_head(s3, member, key, log_object, NULL);
    length = strlen(key);
    if (length > SIZE_MAX - 2) {
        errno = ENOMEM;
        die_fatal("s3ar: out of memory", NULL, NULL);
    }
    char *prefix = malloc(length + 2);
    if (prefix == NULL) { die_fatal("s3ar: out of memory", NULL, NULL); }
    memcpy(prefix, key, length);
    prefix[length] = '/';
    prefix[length + 1] = '\0';
    size_t descendants =
        s3_object_list_prefix(s3, member, prefix, log_object, NULL);
    free(prefix);
    free(member);
    if (!found && descendants == 0) {
        errno = 0;
        die_fatal("s3ar: not found", uri, NULL);
    }
}

void s3ar_list(const struct s3ar_config *config) {
    const struct s3 *s3 = &config->s3;
    for (int i = 0; i < config->operand_count; ++i) {
        list_uri(s3, config->operands[i]);
    }
}
