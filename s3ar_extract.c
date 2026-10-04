/* SPDX-License-Identifier: MIT-0 */
#include "s3ar_archive_reader.h"

void s3ar_extract(const struct s3ar_config *config) {
    s3ar_archive_reader_read(config, false);
}
