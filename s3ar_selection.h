/* SPDX-License-Identifier: MIT-0 */
#ifndef S3AR_SELECTION_H
#define S3AR_SELECTION_H

#include "s3.h"

struct s3ar_selection {
    const char *uri;
    char *storage;
    const char *bucket;
    const char *key;
    bool matched;
};

enum s3ar_selection_match {
    S3AR_SELECTION_NONE,
    S3AR_SELECTION_PARENT,
    S3AR_SELECTION_DIRECT,
};

struct s3ar_selection_set {
    struct s3ar_selection *items;
    size_t count;
};

int s3ar_selection_parse(struct s3ar_selection *selection, const char *uri);
void s3ar_selection_free(struct s3ar_selection *selection);
enum s3ar_selection_match
s3ar_selection_match(const struct s3ar_selection *selection, const char *bucket,
                     const char *key);
/* Parses all operands before processing; diagnoses errors and frees the set
 * on failure. URI strings are borrowed; count == 0 selects everything. */
int s3ar_selection_set_parse(struct s3ar_selection_set *set, size_t count,
                             char *const *operands);
void s3ar_selection_set_free(struct s3ar_selection_set *set);
/* Records DIRECT matches for every matching operand, including overlaps.
 * PARENT selects a bucket entry without satisfying a key operand. */
bool s3ar_selection_set_match(struct s3ar_selection_set *set,
                              const char *bucket, const char *key);

struct s3ar_selection_callbacks {
    void (*bucket)(void *data, const struct s3_bucket *bucket);
    s3_object_callback object;
};
/* Lists by prefix and filters every object through s3ar_selection_match().
 * Listing errors and empty key selections are diagnosed before returning.
 * Callback data and resource pointers are borrowed for the callback. */
enum s3_result s3ar_selection_walk(
    struct s3_client *client, const struct s3ar_selection *selection,
    const struct s3ar_selection_callbacks *callbacks, void *data);
/* Enumerates selected buckets without fetching objects. */
enum s3_result s3ar_selection_buckets(struct s3_client *client,
                                      struct s3_error *error,
                                      const struct s3ar_selection *selection,
                                      s3_bucket_callback callback, void *data);

#endif
