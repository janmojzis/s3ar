#ifndef DIE_H____
#define DIE_H____

#include <libs3.h>

_Noreturn void die_fatal(const char *text, const char *x, const char *y);
_Noreturn void die_s3fatal(const char *text, const char *bucket,
                           const char *key, S3Status status,
                           const S3ErrorDetails *details);

#endif
