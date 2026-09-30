/* SPDX-License-Identifier: MIT-0 */
#include "s3.h"

#include <string.h>

void s3_config_init(struct s3_client_config *config) {
    if (config == NULL) return;
    memset(config, 0, sizeof(*config));
    config->uri_style = S3_URI_STYLE_PATH;
    config->max_attempts = 8;
    config->connect_timeout_ms = 10000;
    config->low_speed_time_s = 30;
}
