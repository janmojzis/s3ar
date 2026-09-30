/* SPDX-License-Identifier: MIT-0 */
/* Small, shared wrapper around libxml2 used by S3 response parsers. */
#ifndef S3_XML_H
#define S3_XML_H

#include <libxml/tree.h>

#include <stdbool.h>
#include <stddef.h>

xmlDoc *s3_xml_read(const char *body, size_t size, size_t limit,
                    const char *document_name);
bool s3_xml_name(const xmlNode *node, const char *name);
xmlNode *s3_xml_child(xmlNode *parent, const char *name);

#endif
