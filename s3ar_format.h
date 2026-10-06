/* SPDX-License-Identifier: MIT-0 */
#ifndef S3AR_FORMAT_H
#define S3AR_FORMAT_H

/* Shared attribute names for PAX archives and downloaded-object xattrs. */
#define S3AR_XATTR_FORMAT_VERSION "1"
#define S3AR_XATTR_PREFIX "user.s3ar."
#define S3AR_XATTR_FORMAT S3AR_XATTR_PREFIX "format"
#define S3AR_XATTR_BUCKET S3AR_XATTR_PREFIX "bucket"
#define S3AR_XATTR_KEY S3AR_XATTR_PREFIX "key"
#define S3AR_XATTR_BUCKET_ACL S3AR_XATTR_PREFIX "bucket-acl"
#define S3AR_XATTR_ETAG S3AR_XATTR_PREFIX "etag"
#define S3AR_XATTR_HASH S3AR_XATTR_PREFIX "hash"
#define S3AR_XATTR_METADATA_PREFIX S3AR_XATTR_PREFIX "metadata."
#define S3AR_LEGACY_XATTR_PREFIX "user."
#define S3AR_PAX_XATTR_PREFIX "SCHILY.xattr."

#endif
