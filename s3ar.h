#ifndef S3AR_H____
#define S3AR_H____

#include "s3.h"

#include <stdbool.h>

enum s3ar_command {
    S3AR_COMMAND_NONE,
    S3AR_COMMAND_CREATE,
    S3AR_COMMAND_EXTRACT,
    S3AR_COMMAND_LIST_OBJECTS,
    S3AR_COMMAND_LIST_BUCKETS,
};

struct s3ar_config {
    enum s3ar_command command;
    bool verbose;
    bool zstd;
    int operand_count;
    char **operands;
    const char *archive_path;
    struct s3_client *s3;
};

struct s3ar_selection {
    const char *uri;
    char *storage;
    const char *bucket;
    const char *key;
};

void s3ar_create(const struct s3ar_config *config);
void s3ar_extract(const struct s3ar_config *config);
void s3ar_list_objects(const struct s3ar_config *config);
void s3ar_list_buckets(const struct s3ar_config *config);
void s3ar_selection_parse(struct s3ar_selection *selection, const char *uri);
void s3ar_selection_free(struct s3ar_selection *selection);
bool s3ar_selection_matches(const struct s3ar_selection *selection,
                            const char *bucket, const char *key);
bool s3ar_key_is_safe(const char *key);

#endif
