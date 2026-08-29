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
void log_s3_name(const char *bucket, const char *key);

#endif
