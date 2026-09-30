/* SPDX-License-Identifier: MIT-0 */
#include "s3_xml.h"

#include <libxml/parser.h>

#include <limits.h>

xmlDoc *s3_xml_read(const char *body, size_t size, size_t limit,
                    const char *document_name) {
    xmlDoc *doc;
    if (body == NULL || size == 0 || size > limit || size > (size_t) INT_MAX)
        return NULL;
    doc = xmlReadMemory(body, (int) size, document_name, NULL,
                        XML_PARSE_NONET | XML_PARSE_NOERROR |
                            XML_PARSE_NOWARNING | XML_PARSE_NOBLANKS);
    if (doc != NULL && xmlGetIntSubset(doc) == NULL) return doc;
    if (doc != NULL) xmlFreeDoc(doc);
    return NULL;
}

bool s3_xml_name(const xmlNode *node, const char *name) {
    return node != NULL && node->type == XML_ELEMENT_NODE &&
           xmlStrcmp(node->name, (const xmlChar *) name) == 0;
}

xmlNode *s3_xml_child(xmlNode *parent, const char *name) {
    if (parent == NULL) return NULL;
    for (xmlNode *node = parent->children; node != NULL; node = node->next)
        if (s3_xml_name(node, name)) return node;
    return NULL;
}
