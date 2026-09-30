/* SPDX-License-Identifier: MIT-0 */
#include "s3_internal.h"
#include "s3_xml.h"

#include <stdio.h>
#include <string.h>

static xmlNode *find_element(xmlNode *node, const char *name) {
    for (; node != NULL; node = node->next) {
        xmlNode *found;
        if (s3_xml_name(node, name)) return node;
        found = find_element(node->children, name);
        if (found != NULL) return found;
    }
    return NULL;
}

static void copy_element(xmlNode *root, const char *name, char *target,
                         size_t capacity) {
    xmlNode *node = find_element(root, name);
    xmlChar *text = node != NULL ? xmlNodeGetContent(node) : NULL;
    if (text != NULL) {
        (void) snprintf(target, capacity, "%s", (const char *) text);
        xmlFree(text);
    }
}

void s3_error_parse_xml(const char *body, size_t size, struct s3_error *error) {
    xmlDoc *doc;
    xmlNode *root;
    if (error == NULL) return;
    doc = s3_xml_read(body, size, S3_ERROR_BODY_LIMIT, "s3-error.xml");
    if (doc == NULL) return;
    root = xmlDocGetRootElement(doc);
    copy_element(root, "Code", error->s3_code, sizeof(error->s3_code));
    copy_element(root, "RequestId", error->request_id,
                 sizeof(error->request_id));
    copy_element(root, "Message", error->message, sizeof(error->message));
    xmlFreeDoc(doc);
}

void s3_error_clear(struct s3_error *error) {
    if (error != NULL) memset(error, 0, sizeof(*error));
}

enum s3_result s3_error_set(struct s3_error *error, enum s3_result result,
                            const char *message) {
    if (error != NULL) {
        error->result = result;
        if (message != NULL)
            (void) snprintf(error->message, sizeof(error->message), "%s",
                            message);
        else
            error->message[0] = '\0';
    }
    return result;
}
