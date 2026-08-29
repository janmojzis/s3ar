#ifndef S3AR_H____
#define S3AR_H____

#include "s3.h"

#include <stdbool.h>

enum s3ar_command {
    S3AR_COMMAND_NONE,
    S3AR_COMMAND_LIST,
};

struct s3ar_config {
    enum s3ar_command command;
    bool verbose;
    int operand_count;
    char **operands;
    struct s3 s3;
};

void s3ar_list(const struct s3ar_config *config);
void log_s3_bucket(const struct s3_bucket *bucket, bool verbose);
void log_s3_object(const struct s3_object *object, bool verbose);

#endif
