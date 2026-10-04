/* SPDX-License-Identifier: MIT-0 */
#ifndef S3AR_ARCHIVE_READER_H
#define S3AR_ARCHIVE_READER_H

#include "s3ar.h"

/* Shared PAX reader for archive listing and restoration. */
void s3ar_archive_reader_read(const struct s3ar_config *config, bool list_only);

#endif
