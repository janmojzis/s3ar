#ifndef LOG_H____
#define LOG_H____

#include "s3.h"

#include <stdbool.h>
#include <stdio.h>

int log_url_encoded_name(FILE *stream, const char *name);
void log_s3_name(FILE *stream, const char *bucket, const char *key);
void log_s3_bucket(const struct s3_bucket *bucket, bool verbose);
void log_s3_object(const struct s3_object *object, bool verbose);

#endif
