/* SPDX-License-Identifier: MIT-0 */
#include "s3_internal.h"
#include "s3_xml.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool acl_permission(const char *permission, bool *read, bool *write) {
    if (strcmp(permission, "READ") == 0)
        *read = true;
    else if (strcmp(permission, "WRITE") == 0)
        *write = true;
    else if (strcmp(permission, "FULL_CONTROL") == 0)
        *read = *write = true;
    else
        return false;
    return true;
}

static enum s3_result parse_acl(const char *body, size_t size, char *summary,
                                size_t capacity, struct s3_error *error) {
    static const char all_users[] =
        "http://acs.amazonaws.com/groups/global/AllUsers";
    static const char authenticated_users[] =
        "http://acs.amazonaws.com/groups/global/AuthenticatedUsers";
    xmlDoc *doc = NULL;
    xmlNode *root;
    xmlChar *owner_id = NULL;
    bool public_read = false, public_write = false;
    bool auth_read = false, auth_write = false, custom = false;
    doc = s3_xml_read(body, size, S3_XML_BODY_LIMIT, "s3-acl.xml");
    root = doc != NULL ? xmlDocGetRootElement(doc) : NULL;
    if (!s3_xml_name(root, "AccessControlPolicy")) goto invalid;
    {
        xmlNode *owner = s3_xml_child(root, "Owner");
        owner_id = s3_xml_content(owner, "ID");
    }
    for (xmlNode *node = root; node != NULL;) {
        if (s3_xml_name(node, "Grant")) {
            xmlNode *grantee = s3_xml_child(node, "Grantee");
            xmlChar *permission = s3_xml_content(node, "Permission");
            xmlChar *uri = s3_xml_content(grantee, "URI");
            xmlChar *id = s3_xml_content(grantee, "ID");
            if (permission == NULL)
                custom = true;
            else if (uri != NULL &&
                     strcmp((const char *) uri, all_users) == 0) {
                if (!acl_permission((const char *) permission, &public_read,
                                    &public_write))
                    custom = true;
            }
            else if (uri != NULL &&
                     strcmp((const char *) uri, authenticated_users) == 0) {
                if (!acl_permission((const char *) permission, &auth_read,
                                    &auth_write))
                    custom = true;
            }
            else if (id == NULL || owner_id == NULL ||
                     xmlStrcmp(id, owner_id) != 0 ||
                     strcmp((const char *) permission, "FULL_CONTROL") != 0)
                custom = true;
            xmlFree(permission);
            xmlFree(uri);
            xmlFree(id);
        }
        if (node->children != NULL)
            node = node->children;
        else {
            while (node != root && node->next == NULL) node = node->parent;
            node = node == root ? NULL : node->next;
        }
    }
    {
        const char *values[5];
        size_t count = 0, used = 0;
        if (public_read) values[count++] = "public-read";
        if (public_write) values[count++] = "public-write";
        if (auth_read) values[count++] = "authenticated-read";
        if (auth_write) values[count++] = "authenticated-write";
        if (custom) values[count++] = "custom";
        if (count == 0) values[count++] = "private";
        for (size_t i = 0; i < count; ++i) {
            int n = snprintf(summary + used, capacity - used, "%s%s",
                             i == 0 ? "" : ",", values[i]);
            if (n < 0 || (size_t) n >= capacity - used) goto invalid;
            used += (size_t) n;
        }
    }
    xmlFree(owner_id);
    xmlFreeDoc(doc);
    return S3_RESULT_OK;
invalid:
    xmlFree(owner_id);
    if (doc != NULL) xmlFreeDoc(doc);
    return s3_error_set(error, S3_RESULT_PROTOCOL_ERROR,
                        "invalid GetBucketAcl XML");
}

enum s3_result s3_bucket_acl(struct s3_client *client, struct s3_error *error,
                             const char *bucket, s3_bucket_callback callback,
                             void *data) {
    char *body = NULL;
    size_t size = 0;
    char summary[128];
    enum s3_result result;
    s3_error_clear(error);
    if (client == NULL || error == NULL || callback == NULL)
        return s3_error_set(error, S3_RESULT_CONFIGURATION_ERROR,
                            "invalid GetBucketAcl arguments");
    result = s3_request_bucket(client, error, bucket, "acl", "GET", NULL, &body,
                               &size);
    if (result != S3_RESULT_OK) return result;
    result = parse_acl(body, size, summary, sizeof(summary), error);
    free(body);
    if (result == S3_RESULT_OK) {
        const struct s3_bucket value = {.name = bucket, .acl = summary};
        if (!callback(data, &value)) {
            error->callback_errno = errno != 0 ? errno : EIO;
            return s3_error_set(error, S3_RESULT_CALLBACK_ERROR,
                                "bucket callback failed");
        }
    }
    return result;
}
