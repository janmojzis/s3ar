#ifndef S3AR_H____
#define S3AR_H____

#include "s3.h"

#include <stdbool.h>

enum s3ar_command {
    S3AR_COMMAND_NONE,
    S3AR_COMMAND_CREATE,
    S3AR_COMMAND_LIST,
};

struct s3ar_config {
    enum s3ar_command command;
    bool verbose;
    int operand_count;
    char **operands;
    const char *archive_path;
    struct s3 s3;
};

struct s3ar_selection {
    const char *uri;
    char *storage;
    const char *bucket;
    const char *key;
};

void s3ar_create(const struct s3ar_config *config);
void s3ar_list(const struct s3ar_config *config);
void s3ar_selection_parse(struct s3ar_selection *selection, const char *uri);
void s3ar_selection_free(struct s3ar_selection *selection);
void log_s3_bucket(const struct s3_bucket *bucket, bool verbose);
void log_s3_object(const struct s3_object *object, bool verbose);

#endif
