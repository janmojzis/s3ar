/* SPDX-License-Identifier: MIT-0 */
#include "../s3_object_delete.c"

#include <assert.h>

static struct {
    bool mixed;
    bool batch_denied;
    bool single_denied;
    unsigned batches;
    unsigned singles;
} transport;

enum s3_result
__wrap_s3_request_bucket(struct s3_client *client, struct s3_error *error,
                         const char *bucket, const char *query,
                         const char *method, const char *request_body,
                         char **response_body, size_t *response_size) {
    (void) client;
    assert(transport.mixed);
    assert(strcmp(bucket, "bucket") == 0);
    assert(strcmp(query, "delete") == 0);
    assert(strcmp(method, "POST") == 0);
    xmlDoc *doc = s3_xml_read(request_body, strlen(request_body),
                              S3_XML_BODY_LIMIT, "request.xml");
    assert(doc != NULL);
    size_t count = 0;
    for (xmlNode *node = xmlDocGetRootElement(doc)->children; node != NULL;
         node = node->next) {
        if (!s3_xml_name(node, "Object")) continue;
        char *key = s3_xml_text(node, "Key");
        assert(key != NULL);
        assert(strcmp(key, count == 0 ? "a" : "z") == 0);
        free(key);
        ++count;
    }
    assert(count == 2);
    xmlFreeDoc(doc);
    const char *body =
        transport.batch_denied
            ? "<DeleteResult><Deleted><Key>a</Key><VersionId>v1"
              "</VersionId></Deleted><Error><Key>z</Key>"
              "<VersionId>v2</VersionId><Code>AccessDenied</Code>"
              "<Message>Batch denied.</Message></Error></DeleteResult>"
            : "<DeleteResult><Deleted><Key>z</Key><VersionId>v2"
              "</VersionId></Deleted><Deleted><Key>a</Key>"
              "<VersionId>v1</VersionId></Deleted></DeleteResult>";
    *response_body = strdup(body);
    assert(*response_body != NULL);
    *response_size = strlen(body);
    s3_error_clear(error);
    error->http_status = 200;
    ++transport.batches;
    return S3_RESULT_OK;
}

enum s3_result __wrap_s3_request_url(struct s3_client *client,
                                     struct s3_error *error, const char *url,
                                     const char *method,
                                     const char *request_body,
                                     char **response_body,
                                     size_t *response_size) {
    (void) client;
    assert(strcmp(url, "http://example.test/bucket/b%01?versionId=v%2F%261") ==
           0);
    assert(strcmp(method, "DELETE") == 0);
    assert(request_body == NULL && response_body == NULL &&
           response_size == NULL);
    s3_error_clear(error);
    error->http_status = transport.single_denied ? 403 : 204;
    ++transport.singles;
    return transport.single_denied
               ? s3_error_set(error, S3_RESULT_ACCESS_DENIED, "Denied.")
               : S3_RESULT_OK;
}

static void test_batch_fallback(void) {
    struct s3_client client = {.endpoint = "http://example.test",
                               .uri_style = S3_URI_STYLE_PATH};
    struct s3_object_version_ref targets[] = {
        {"z", "v2"}, {"b\001", "v/&1"}, {"a", "v1"}};
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        memset(&transport, 0, sizeof(transport));
        transport.mixed = scenario != 0;
        transport.batch_denied = scenario == 2 || scenario == 4;
        transport.single_denied = scenario >= 3;
        struct s3_error error = {0};
        struct s3_object_delete_result results[3] = {0};
        size_t count = transport.mixed ? 3 : 1;
        enum s3_result result = s3_object_delete_batch(
            &client, &error, "bucket", transport.mixed ? targets : &targets[1],
            count, results);
        assert(transport.batches == (unsigned) transport.mixed);
        assert(transport.singles == 1);
        if (transport.single_denied) {
            assert(result == S3_RESULT_ACCESS_DENIED);
            assert(error.http_status == 403);
            for (size_t i = 0; i < count; ++i) {
                assert(!results[i].deleted);
                assert(results[i].code == NULL && results[i].message == NULL);
            }
        }
        else {
            assert(result == S3_RESULT_OK);
            assert(error.http_status == (transport.mixed ? 200 : 204));
            if (transport.mixed) {
                assert(results[1].deleted && results[2].deleted);
                assert(results[0].deleted == !transport.batch_denied);
                if (transport.batch_denied)
                    assert(strcmp(results[0].code, "AccessDenied") == 0);
            }
            else
                assert(results[0].deleted);
        }
        s3_object_delete_results_free(results, count);
    }
}

static void test_argument_errors_clear_results(void) {
    struct s3_client client = {0};
    char sentinel[] = "not owned";
    memset(&transport, 0, sizeof(transport));
    for (unsigned scenario = 0; scenario < 8; ++scenario) {
        struct s3_object_version_ref targets[] = {{"a", "v1"}, {"z", "v2"}};
        struct s3_object_delete_result results[2] = {
            {true, sentinel, sentinel}, {true, sentinel, sentinel}};
        struct s3_error error = {.http_status = 503, .message = "old error"};
        struct s3_client *client_arg = &client;
        struct s3_error *error_arg = &error;
        const char *bucket_arg = "bucket";
        const struct s3_object_version_ref *items_arg = targets;
        switch (scenario) {
            case 0:
                client_arg = NULL;
                break;
            case 1:
                error_arg = NULL;
                break;
            case 2:
                bucket_arg = NULL;
                break;
            case 3:
                items_arg = NULL;
                break;
            case 4:
                targets[1].key = "";
                break;
            case 5:
                targets[1].version_id = NULL;
                break;
            case 6:
                targets[1].version_id = "";
                break;
            case 7:
                targets[1] = targets[0];
                break;
        }
        enum s3_result result = s3_object_delete_batch(
            client_arg, error_arg, bucket_arg, items_arg, 2, results);
        assert(result == (scenario == 7 ? S3_RESULT_PROTOCOL_ERROR
                                        : S3_RESULT_CONFIGURATION_ERROR));
        if (error_arg != NULL) {
            assert(error.result == result);
            assert(error.http_status == 0);
        }
        for (size_t i = 0; i < 2; ++i) {
            assert(!results[i].deleted);
            assert(results[i].code == NULL && results[i].message == NULL);
        }
        s3_object_delete_results_free(results, 2);
    }
    assert(transport.batches == 0 && transport.singles == 0);
}

static void test_invalid_output_bounds_do_not_write(void) {
    struct s3_client client = {0};
    struct s3_error error = {0};
    struct s3_object_version_ref target = {"key", "version"};
    char sentinel[] = "not owned";
    struct s3_object_delete_result result = {true, sentinel, sentinel};
    const size_t invalid_counts[] = {0, S3_DELETE_BATCH_LIMIT + 1, SIZE_MAX};
    for (size_t i = 0; i < sizeof(invalid_counts) / sizeof(invalid_counts[0]);
         ++i) {
        assert(s3_object_delete_batch(&client, &error, "bucket", &target,
                                      invalid_counts[i], &result) ==
               S3_RESULT_CONFIGURATION_ERROR);
        assert(result.deleted && result.code == sentinel &&
               result.message == sentinel);
    }
    assert(s3_object_delete_batch(&client, &error, "bucket", &target, 1,
                                  NULL) == S3_RESULT_CONFIGURATION_ERROR);
}

static void test_xml_character_limits(void) {
    static const char *const invalid[] = {
        "a\001b",         "a\013b",     "a\037b",   "a\357\277\276b",
        "a\357\277\277b", "a\300\257b", "a\360\237"};
    assert(xml_value_valid("\t\n\r <& \303\251 \360\237\230\200"));
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        struct s3_object_version_ref target = {invalid[i], "version"};
        struct batch_entry entry = {.target = &target};
        assert(!batch_target_xml_valid(&target));
        assert(build_delete_xml(&entry, 1) == NULL);
        target.key = "key";
        target.version_id = invalid[i];
        assert(!batch_target_xml_valid(&target));
        assert(build_delete_xml(&entry, 1) == NULL);
    }
}

static void test_xml_preserves_special_key_bytes(void) {
    struct s3_object_version_ref target = {.key = "a\r<&", .version_id = "v&1"};
    struct batch_entry entry = {.target = &target};
    char *body = build_delete_xml(&entry, 1);
    xmlDoc *doc;
    xmlNode *object;
    char *key, *id;
    assert(body != NULL);
    assert(strstr(body, "&#13;") != NULL);
    doc = s3_xml_read(body, strlen(body), S3_XML_BODY_LIMIT, "test-delete.xml");
    assert(doc != NULL);
    object = s3_xml_child(xmlDocGetRootElement(doc), "Object");
    assert(object != NULL);
    key = s3_xml_text(object, "Key");
    id = s3_xml_text(object, "VersionId");
    assert(key != NULL && strcmp(key, target.key) == 0);
    assert(id != NULL && strcmp(id, target.version_id) == 0);
    free(key);
    free(id);
    xmlFreeDoc(doc);
    free(body);
}

static void test_mixed_result_preserves_input_order(void) {
    static const char response[] =
        "<DeleteResult><Error><Key>second</Key><Code>AccessDenied</Code>"
        "<Message>Denied.</Message></Error><Deleted><Key>first</Key>"
        "<VersionId>v1</VersionId></Deleted></DeleteResult>";
    struct s3_object_version_ref targets[] = {{"second", "v2"},
                                              {"first", "v1"}};
    struct batch_entry entries[] = {{.target = &targets[0], .index = 0},
                                    {.target = &targets[1], .index = 1}};
    struct s3_object_delete_result results[2] = {0};
    struct s3_error error = {0};
    qsort(entries, 2, sizeof(*entries), compare_batch_entries);
    assert(parse_delete_result(entries, 2, response, sizeof(response) - 1,
                               results, &error) == S3_RESULT_OK);
    assert(!results[0].deleted);
    assert(strcmp(results[0].code, "AccessDenied") == 0);
    assert(results[1].deleted);
    s3_object_delete_results_free(results, 2);
}

static void test_missing_acknowledgment_is_protocol_error(void) {
    static const char response[] =
        "<DeleteResult><Deleted><Key>first</Key><VersionId>v1</VersionId>"
        "</Deleted></DeleteResult>";
    struct s3_object_version_ref targets[] = {{"first", "v1"},
                                              {"second", "v2"}};
    struct batch_entry entries[] = {{.target = &targets[0], .index = 0},
                                    {.target = &targets[1], .index = 1}};
    struct s3_object_delete_result results[2] = {0};
    struct s3_error error = {0};
    assert(parse_delete_result(entries, 2, response, sizeof(response) - 1,
                               results, &error) == S3_RESULT_PROTOCOL_ERROR);
    assert(!results[0].deleted && !results[1].deleted);
    assert(strcmp(error.message, "invalid DeleteObjects XML") == 0);
}

int main(void) {
    test_argument_errors_clear_results();
    test_invalid_output_bounds_do_not_write();
    test_batch_fallback();
    test_xml_character_limits();
    test_xml_preserves_special_key_bytes();
    test_mixed_result_preserves_input_order();
    test_missing_acknowledgment_is_protocol_error();
    return 0;
}
