/* SPDX-License-Identifier: MIT-0 */
#include "s3ar_transform.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static void check(const char *expression, const char *input,
                  const char *expected) {
    struct s3ar_transform *transforms = NULL;
    char error[256];
    assert(s3ar_transform_add(&transforms, expression, error, sizeof(error)));
    char *result =
        s3ar_transform_apply(transforms, input, error, sizeof(error));
    assert(result != NULL);
    assert(strcmp(result, expected) == 0);
    free(result);
    s3ar_transform_free(transforms);
}

static void check_split(const char *expression, const char *input,
                        const char *expected_error) {
    struct s3ar_transform *transforms = NULL;
    char error[256];
    assert(s3ar_transform_add(&transforms, expression, error, sizeof(error)));
    char *result =
        s3ar_transform_apply(transforms, input, error, sizeof(error));
    assert(result == NULL);
    assert(strcmp(error, expected_error) == 0);
    s3ar_transform_free(transforms);
}

int main(void) {
    check("s|^uploads/|test/|", "uploads/key", "test/key");
    check("s|^uploads/|test/|", "uploads/", "test/");
    check("s|old|new|", "bucket/old/old", "bucket/new/old");
    check("s|old|new|g", "bucket/old/old", "bucket/new/new");
    check("s|OLD|new|i", "bucket/old", "bucket/new");
    check("s|OLD|new|gi", "OLD/old", "new/new");
    check("s|\\(old\\)/\\(.*\\)|\\2/\\1|", "old/path", "path/old");
    check("s|old|(&)|", "old", "(old)");
    check("s|old|\\&\\\\|", "old", "&\\");
    check("s#old\\#x#new\\#&#", "old#x", "new#old#x");
    check("s.old\\.path.new.", "old.path", "new");
    check("s|old\\|path|new|", "old|path", "new");
    check("s|^|prefix/|g", "bucket/key", "prefix/bucket/key");
    check("s|$|/suffix|g", "bucket/key", "bucket/key/suffix");
    check("s|x*|_|g", "abc", "_a_b_c_");
    check("s|a*|X|g", "baa", "XbX");
    check("s|.*|X|g", "abc", "X");
    check("s|.*|X|g", "", "X");
    check("s|old||", "old", "");
    check("s|old|new|", "bucket/key", "bucket/key");
    check("s|x*|_|g", "ž🙂", "_ž_🙂_");
    check("s|a*|X|g", "žaa🙂", "XžX🙂X");
    check("s|ž|🙂|g", "ž/ž", "🙂/🙂");
    check("s|\\(ž\\)|\\1-copy|", "ž", "ž-copy");
    check("s|.*|X|g", "ž🙂", "X");
    check("s|\\(.\\).|&|", "ž", "ž");
    check_split("s|.|X|", "ž", "transform match splits a UTF-8 character");
    check_split("s|\xbe|X|", "ž", "transform match splits a UTF-8 character");
    check_split("s|\\(.\\).|\\1|", "ž",
                "transform capture group splits a UTF-8 character");
    check_split("s|.\\(.\\)|\\1|", "ž",
                "transform capture group splits a UTF-8 character");

    struct s3ar_transform *transforms = NULL;
    char error[256];
    assert(s3ar_transform_add(&transforms, "s|^old/|middle/|", error,
                              sizeof(error)));
    assert(s3ar_transform_add(&transforms, "s|^middle/|new/|", error,
                              sizeof(error)));
    char *result =
        s3ar_transform_apply(transforms, "old/key", error, sizeof(error));
    assert(result != NULL && strcmp(result, "new/key") == 0);
    free(result);
    s3ar_transform_free(transforms);

    static const char *const invalid[] = {
        "",
        "s",
        "x|old|new|",
        "s old new ",
        "s1old1new1",
        "s|old|new",
        "s|[|new|",
        "s|old|new|e",
        "s|old|new|2",
        "s|old|new|;s|x|y|",
        "s|old|\\1|",
        "s|old|\\L&|",
        "s|old|\\",
        "s|old|new|S",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        transforms = NULL;
        error[0] = '\0';
        assert(
            !s3ar_transform_add(&transforms, invalid[i], error, sizeof(error)));
        assert(transforms == NULL && error[0] != '\0');
    }
    return 0;
}
