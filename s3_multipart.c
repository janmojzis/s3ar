/* SPDX-License-Identifier: MIT-0 */
#include "s3_internal.h"

#include <stdlib.h>
#include <string.h>

enum s3_result s3_multipart_abort(struct s3_client *client,
                                  struct s3_error *error, const char *bucket,
                                  const char *key, const char *upload_id) {
    const struct s3_query_param params[] = {{"uploadId", upload_id},
                                            {NULL, NULL}};
    char *query = NULL, *url = NULL;
    enum s3_result result;
    s3_error_clear(error);
    if (client == NULL || error == NULL || bucket == NULL ||
        !s3_url_key_valid(key) || upload_id == NULL || upload_id[0] == '\0')
        return s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                            "invalid multipart upload");
    query = s3_query_build("", params);
    if (query == NULL)
        return s3_error_set(error, S3_RESULT_ERROR, "out of memory");
    result = s3_url_build_object(client, bucket, key, query, &url, error);
    free(query);
    if (result == S3_RESULT_OK)
        result = s3_request_url(client, error, url, "DELETE", NULL, NULL, NULL);
    free(url);
    return result;
}
