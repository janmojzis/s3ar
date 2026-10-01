/* SPDX-License-Identifier: MIT-0 */
#include "s3ar_selection.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

static void check(const char *uri, const char *bucket, const char *key,
                  enum s3ar_selection_match expected) {
    struct s3ar_selection selection;
    assert(s3ar_selection_parse(&selection, uri) == 0);
    assert(s3ar_selection_match(&selection, bucket, key) == expected);
    s3ar_selection_free(&selection);
}

int main(void) {
    const char *uris[] = {"s3://photos/photo", "s3://photos/photo/",
                          "s3://photos/photo///"};
    const char *keys[] = {"photo",     "photo/",       "photo/a",
                          "photo//a",  "photo/deep/a", "photo1",
                          "photo-old", "phot",         ""};
    for (size_t i = 0; i < sizeof(uris) / sizeof(*uris); ++i) {
        check(uris[i], "photos", NULL, S3AR_SELECTION_PARENT);
        check(uris[i], "videos", NULL, S3AR_SELECTION_NONE);
        for (size_t j = 0; j < sizeof(keys) / sizeof(*keys); ++j) {
            check(uris[i], "photos", keys[j],
                  j < 5 ? S3AR_SELECTION_DIRECT : S3AR_SELECTION_NONE);
            check(uris[i], "videos", keys[j], S3AR_SELECTION_NONE);
        }
    }
    check("s3://", "photos", NULL, S3AR_SELECTION_DIRECT);
    check("s3://", "photos", "", S3AR_SELECTION_DIRECT);
    check("s3://photos///", "photos", NULL, S3AR_SELECTION_DIRECT);
    check("s3://photos/", "photos", "anything", S3AR_SELECTION_DIRECT);
    check("s3://photos", "photos2", "anything", S3AR_SELECTION_NONE);
    check("s3://photos/a%20b", "photos", "a b", S3AR_SELECTION_NONE);
    check("s3://photos/a%20b", "photos", "a%20b/x", S3AR_SELECTION_DIRECT);
    check("s3://photos/a b\n", "photos", "a b\n/x", S3AR_SELECTION_DIRECT);
    check("s3://photos/a//b", "photos", "a/b", S3AR_SELECTION_NONE);
    check("s3://photos/a/../b", "photos", "b", S3AR_SELECTION_NONE);

    struct s3ar_selection_set set;
    char *operands[] = {"s3://photos/photo",   "s3://photos/photo/",
                        "s3://photos/photo/a", "s3://photos/missing",
                        "s3://photos",         "s3://videos"};
    assert(s3ar_selection_set_parse(&set, 6, operands) == 0);
    assert(s3ar_selection_set_match(&set, "photos", NULL));
    for (size_t i = 0; i < set.count; ++i)
        assert(set.items[i].matched == (i == 4));
    assert(s3ar_selection_set_match(&set, "photos", "photo/a/deep"));
    assert(set.items[0].matched && set.items[1].matched &&
           set.items[2].matched);
    assert(!set.items[3].matched && !set.items[5].matched);
    assert(!s3ar_selection_set_match(&set, "other", "photo"));
    s3ar_selection_set_free(&set);
    assert(set.items == NULL && set.count == 0);

    assert(s3ar_selection_set_parse(&set, 0, NULL) == 0);
    assert(s3ar_selection_set_match(&set, "anything", NULL));
    assert(s3ar_selection_set_match(&set, "anything", "anything"));
    s3ar_selection_set_free(&set);

    const char *invalid[] = {
        "", "s3:", "s3:/", "s3:///", "s3:///bucket", "http://photos", "photos"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        char *mixed[] = {"s3://photos", (char *) invalid[i]};
        assert(s3ar_selection_set_parse(&set, 2, mixed) == -1);
        assert(errno == EINVAL);
        assert(set.items == NULL && set.count == 0);
    }
    return 0;
}
