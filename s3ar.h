#ifndef S3AR_H____
#define S3AR_H____

#include "s3.h"
#include "s3ar_selection.h"

#include <stdbool.h>

#define S3AR_XATTR_FORMAT_VERSION "1"

enum s3ar_command {
    S3AR_COMMAND_NONE,
    S3AR_COMMAND_CREATE,
    S3AR_COMMAND_EXTRACT,
    S3AR_COMMAND_LIST_ARCHIVE,
};

struct s3ar_transform;

struct s3ar_config {
    enum s3ar_command command;
    bool verbose;
    bool zstd;
    int operand_count;
    char **operands;
    const char *archive_path;
    struct s3_client *s3;
    struct s3ar_transform *transforms;
};

void s3ar_create(const struct s3ar_config *config);
void s3ar_create_cleanup(void);
void s3ar_extract(const struct s3ar_config *config);
void s3ar_list_archive(const struct s3ar_config *config);
/* Archive CLI lifecycle. Options and client configuration are owned by s3ar.c;
 * s3ar_die() releases the client, transforms and archive creation resources. */
struct s3ar_config *s3ar_config_get(void);
int s3ar_connect(void);
_Noreturn void s3ar_die(int status);

#endif
