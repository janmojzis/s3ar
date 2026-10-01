/* SPDX-License-Identifier: MIT-0 */
#include "s3_internal.h"

#include <stdlib.h>
#include <string.h>

void s3_object_properties_free(struct s3_object_properties *properties) {
    struct s3_metadata *metadata;
    if (properties == NULL) return;
    free((char *) properties->content_type);
    free((char *) properties->content_encoding);
    free((char *) properties->cache_control);
    free((char *) properties->content_disposition);
    free((char *) properties->content_language);
    free((char *) properties->expires);
    properties->content_type = NULL;
    properties->content_encoding = NULL;
    properties->cache_control = NULL;
    properties->content_disposition = NULL;
    properties->content_language = NULL;
    properties->expires = NULL;
    metadata = (struct s3_metadata *) properties->metadata;
    for (size_t i = 0; i < properties->metadata_count; ++i) {
        free((char *) metadata[i].name);
        free((char *) metadata[i].value);
    }
    free(metadata);
    properties->metadata = NULL;
    properties->metadata_count = 0;
}
