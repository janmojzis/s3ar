/* SPDX-License-Identifier: MIT-0 */
#include "s3_internal.h"

#include <libxml/parser.h>
#include <libxml/tree.h>

#include <limits.h>
#include <stdio.h>
#include <string.h>

static xmlNode *find_element(xmlNode *node, const char *name) {
    for (; node != NULL; node = node->next) {
        xmlNode *found;
        if (node->type == XML_ELEMENT_NODE &&
            xmlStrcmp(node->name, (const xmlChar *)name) == 0)
            return node;
        found = find_element(node->children, name);
        if (found != NULL) return found;
    }
    return NULL;
}

static void copy_element(xmlDoc *doc, xmlNode *root, const char *name,
                         char *target, size_t capacity) {
    xmlNode *node = find_element(root, name);
    xmlChar *text;
    if (node == NULL) return;
    text = xmlNodeListGetString(doc, node->children, 1);
    if (text != NULL) {
        (void)snprintf(target, capacity, "%s", (const char *)text);
        xmlFree(text);
    }
}

void s3_parse_error_xml(const char *body, size_t size, struct s3_error *error) {
    xmlDoc *doc;
    xmlNode *root;
    if (error == NULL || body == NULL || size == 0 || size > S3_ERROR_BODY_LIMIT ||
        size > (size_t)INT_MAX || strstr(body, "<!DOCTYPE") != NULL)
        return;
    doc = xmlReadMemory(body, (int)size, "s3-error.xml", NULL,
                        XML_PARSE_NONET | XML_PARSE_NOERROR |
                            XML_PARSE_NOWARNING | XML_PARSE_NOBLANKS);
    if (doc == NULL) return;
    if (xmlGetIntSubset(doc) != NULL) {
        xmlFreeDoc(doc);
        return;
    }
    root = xmlDocGetRootElement(doc);
    copy_element(doc, root, "Code", error->s3_code, sizeof(error->s3_code));
    copy_element(doc, root, "RequestId", error->request_id,
                 sizeof(error->request_id));
    copy_element(doc, root, "Message", error->message, sizeof(error->message));
    xmlFreeDoc(doc);
}
