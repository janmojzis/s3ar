---
title: S3AR
section: 1
date: September 2026
footer: S3 Archiver
header: User Commands
---

# NAME

s3ar - create tar archives from S3 and extract them back to S3

# SYNOPSIS

```text
s3ar (-c|--create) [-v|--verbose] [--hash] [--multipart-size SIZE] [--zstd] [-f TARFILE] S3...
s3ar (-x|--extract|--get) [-v|--verbose] [--hash] [--multipart-size SIZE] [--zstd] [--transform EXPR] [-f TARFILE] S3...
s3ar (-t|--list) [-v|--verbose] [--zstd] [--transform EXPR] [-f TARFILE] [S3...]
s3ar (-h|--help)
```

# DESCRIPTION

**`s3ar`**
(S3 Archiver) creates tar archives directly from S3-compatible storage and
restores them back to S3. It preserves S3 user metadata and records
informational bucket ACL summaries in PAX
**`SCHILY`**
extended-attribute headers.

The command line follows the familiar style of
**`tar(1)`**
and supports
**`-c,`**
**`-x,`**
**`-t,`**
**`-f,`**
and
**`-v.`**
**`s3ar`**
works with live S3 resources and implements only the options documented here;
it is not a general replacement for
**`tar.`**

# S3 CONFIGURATION

Set the S3 connection through environment variables. For example:

```sh
export S3AR_ACCESS_KEY='access-key'
export S3AR_SECRET_KEY='secret-key'
export S3AR_ENDPOINT='https://s3.example.net'
export S3AR_URI_STYLE='path'
export S3AR_REGION='us-east-1'
export S3AR_SESSION_TOKEN='temporary-session-token'
```

S3 configuration is required for creation and extraction, but not for archive
listing. The session token is needed only for temporary credentials.

## Environment variables

**`S3AR_ACCESS_KEY`**

S3 access key. Required.

**`S3AR_SECRET_KEY`**

S3 secret key. Required.

**`S3AR_ENDPOINT`**

S3 endpoint in the form
**`http://HOST[:PORT]`**
or
**`https://HOST[:PORT].`**
The scheme is required; URL paths and a trailing slash are not accepted.
Required.

**`S3AR_URI_STYLE`**

S3 addressing style:
**`path`**
or
**`virtual.`**
Optional; defaults to
**`path.`**

**`S3AR_REGION`**

SigV4 signing region and location constraint used when creating a bucket that
does not already exist. Optional; defaults to
**`us-east-1,`**
which omits the location constraint.

**`S3AR_SESSION_TOKEN`**

Token for temporary S3 credentials. Optional.

# COMMAND LINE

## Operations

**`-c, --create`**

Create a tar archive from the selected S3 resources. At least one S3 URI is
required. URIs are processed in command-line order. If overlapping URIs select
an object more than once, it is stored once for each match. Each operand also
stores its bucket directory entry, including repeated bucket entries.

**`-x, --extract, --get`**

Restore a tar archive to S3. At least one S3 URI is required. Use
**`s3://`**
to restore the whole archive. Other URIs limit extraction to matching entries;
overlapping matches do not upload an entry more than once.

**`-t, --list`**

List archived buckets and objects without connecting to S3. With no S3 URI,
all entries are listed. One or more URIs limit the listing to matching entries.

## Options

**`-f TARFILE, --file=TARFILE`**

Read or write the named archive. This option is valid only with
**`--create,`**
**`--extract,`**
or
**`--list.`**
TARFILE must be a local filesystem path or
**`-.`**
**`--create`**
writes regular archive files through a temporary file in the same directory
and atomically replaces the destination only after successful completion.
Standard output and non-regular destinations are streamed directly and can
contain a partial archive after a failure.

A new archive starts with mode 0666, modified by the current
**`umask.`**
Overwriting an existing archive keeps its permissions.

**`--zstd`**

With
**`--create,`**
write a zstd-compressed archive.
**`--extract`**
and
**`--list`**
detect plain and zstd-compressed input automatically; specifying this option
rejects plain input.

**`--transform=EXPR`**

Rename decoded archive identities during extraction or archive listing.
Expressions operate on
**`BUCKET/KEY`**
without a
**`s3://`**
prefix. Selection operands match resulting identities after all transformations.
Bucket members are transformed as
**`BUCKET/;`**
the first component of the result names the destination bucket.
Repeat this option to apply substitutions in order. It is not supported with
**`-c.`**

Use
**`s<delimiter>REGEX<delimiter>REPLACEMENT<delimiter>[gi]`**
with a punctuation delimiter and POSIX basic regular expressions.
The first match is replaced by default;
**`g`**
replaces all matches and
**`i`**
ignores case. Replacements support
**`&`**
and capture references
**`\1`**
through
**`\9.`**
Backslash escapes the delimiter, ampersand, or backslash. Other flags,
semicolon scripts, and case-conversion escapes are rejected.

Regular expressions operate on bytes in the C locale; Unicode character
classes and case folding are not supported. Empty global matches advance
over a complete UTF-8 character. A match or a capture used in the replacement
that splits a UTF-8 character is rejected.

Use
**`-t`**
to preview transformed names without connecting to S3. Preview validates
path-style names; restore also applies the actual endpoint's naming rules.
For example:

```sh
s3ar -tf archive.tar --transform='s|^uploads/|test/|'
s3ar -xvf archive.tar s3://test --transform='s|^uploads/|test/|'
```

**`--hash`**

With -c, compute SHA-512 for objects up to --multipart-size, inclusive
(default 16 MiB), by buffering
the entire object in memory before writing its archive header. Larger objects
are streamed with hash none and a warning. If buffer allocation fails, also
store none and warn; a successful fallback exits with status 0.
Without this option, creation streams every object and stores none.
With -x, verify stored SHA-512 values before PUT or multipart completion.
A mismatch aborts the upload and stops restoration with status 2. Objects
with none are restored with a warning. A missing hash attribute is rejected.
Without this option, restoration does not verify object content.
This option requires -c or -x.
With -x --hash -v, the stored hash is followed by verified or unverified.

**`--multipart-size SIZE`**

Set the hash buffer limit for -c --hash and the upload threshold and part size
for -x. The default is 16M. Use an integer with an uppercase M or G suffix
(MiB or GiB), between 5M and 5G inclusive.
With -c --hash, objects above this limit are streamed with hash none and a
warning; buffering uses at most SIZE bytes per object. Without --hash,
creation streams objects without buffering them in full.
With -x, objects up to SIZE inclusive use a simple PUT; larger objects use
multipart uploads with SIZE-byte parts and a possibly smaller final part.
Uploads buffer at most SIZE bytes per object. Objects exceeding 10,000 parts
are rejected; increase SIZE to restore them. This option requires -c or -x.

**`-v, --verbose`**

Increase log verbosity. Use once for info, twice for debug, and three times for
trace. During creation and extraction, info logs report each successfully
processed entry on standard error as
**`BUCKET/KEY`**
without a URI scheme. Creation also logs object size, modification time, ETag, and a hash
(sha512: followed by 128 lowercase hex digits, or none when not computed). Extraction logs names followed by the stored hash
value, or none when absent. Archive
listing fields are described in
**`LISTING AN ARCHIVE.`**
**`-v`**
also adds object metadata to archive listing rows on standard output.

While the command runs,
**`SIGUSR1`**
raises the log level by one step and
**`SIGUSR2`**
lowers it by one step.

**`-h, --help`**

Display command-line help on standard error and exit successfully.

Without
**`-f,`**
or with
**`-f -,`**
**`--create`**
writes to standard output and
**`--extract`**
and
**`--list`**
read from standard input.

# NAME QUOTING

S3 bucket and key names in listings, verbose output, and diagnostics use
byte-oriented URL encoding. ASCII letters, digits,
**`-,`**
**`.,`**
**`_,`**
**`~,`**
and
**`/`**
are written unchanged. Every other byte is written as
**`%HH`**
with uppercase hexadecimal digits. A space is therefore
**`%20,`**
a literal percent sign is
**`%25,`**
and UTF-8 names are encoded byte by byte. The format is independent of the
current locale.

# SELECTING S3 RESOURCES

A trailing slash does not change the selection.

**`s3://`**

Select all buckets and all their objects.

**`s3://BUCKET or s3://BUCKET/`**

Select the named bucket and all its objects.

**`s3://BUCKET/NAME or s3://BUCKET/NAME/`**

Select the exact
**`NAME`**
key and all objects below
**`NAME/.`**

Matches stop at path boundaries. For example,
**`photo`**
matches the exact key
**`photo`**
and keys below
**`photo/,`**
but not
**`photo1.jpg`**
or
**`photo-old.jpg.`**
The exact key itself does not need to exist if matching descendants do.

If a URI matches neither an object nor a prefix, the command fails. Empty
buckets are valid and still produce a bucket entry during archive creation.
Input URIs use the
**`s3://`**
form. Bucket and key names in operands are literal, not URL-decoded; quote
operands containing spaces or shell metacharacters. Archive listings and
creation/extraction info logs omit the URI prefix.

# CREATING ARCHIVES

The
**`--create`**
operation always needs an S3 URI. To select everything, pass
**`s3://`**
explicitly; omitting the operand is an error.

Select all resources, named buckets, or prefixes:

```sh
# All buckets and objects
s3ar -c -f media.tar s3://

# Two buckets
s3ar -c -f media.tar s3://photos s3://videos

# One bucket
s3ar -c -f photos.tar s3://photos

# One or two prefixes
s3ar -c -f photos-2026.tar s3://photos/2026
s3ar -c -f media-2026.tar s3://photos/2026 s3://videos/2026
```

Without
**`-f,`**
the archive is streamed to standard output:

```sh
s3ar -c s3://photos/2026 >photos-2026.tar
```

Each selected bucket becomes a
**`BUCKET/`**
directory entry followed by its selected objects, stored as regular
**`BUCKET/KEY`**
entries and streamed directly from S3 into libarchive. Operands are processed
independently, so overlapping or repeated operands produce repeated bucket and
object members. An empty bucket still appears in the archive.

Regular archive files are written to a temporary file in the same directory
and atomically renamed only after the archive has been completed and synced.
Standard output and non-regular destinations are streamed directly.

The archive file is installed atomically, but the S3 snapshot is not. An object
may change between the S3 LIST and GET requests. If a listed object disappears
before its GET starts,
**`s3ar`**
warns, skips that object, and completes the archive. Other GET failures still
stop archive creation.

An interrupted GET resumes from the first byte not yet written, using
**`Range`**
and
**`If-Match`**
to stay on the object selected by the initial response. Object properties and
S3 user metadata are captured by that response and must remain identical on
every resumed response. A mismatch is deliberately treated as a protocol
error. The download stops rather than combining responses from different
snapshots or switching to a newer object. When the archive is
a regular file, the previously installed archive remains unchanged.

With
**`-v,`**
archived entry names are logged as info on standard error without the
**`s3://`**
prefix. A tar stream written to standard output remains untouched.

## Archive format

S3-specific information is stored in namespaced SCHILY extended attributes:

```sh
SCHILY.xattr.user.s3ar.format=1
SCHILY.xattr.user.s3ar.bucket=URL-ENCODED-BUCKET
SCHILY.xattr.user.s3ar.key=URL-ENCODED-KEY
SCHILY.xattr.user.s3ar.etag="3472a7..."
SCHILY.xattr.user.s3ar.hash=sha512:<128 lowercase hex digits>
SCHILY.xattr.user.s3ar.bucket-acl=public-read,custom
SCHILY.xattr.user.s3ar.metadata.NAME=VALUE
```

The hash attribute contains sha512: followed by the hexadecimal digest,
or none when no hash was computed.

Every bucket and object carries the format marker and URL-encoded bucket name.
Object entries also carry their URL-encoded key and, when available, their
informational ETag. Slash remains unescaped;
bytes outside the RFC 3986 unreserved set use uppercase
**`%HH`**
encoding. The bucket and key attributes are the authoritative S3 identity.
Every bucket and object entry must have exactly one format marker with value
1. Missing, duplicate, or unsupported markers are rejected, including on
unselected entries. Every object must also carry a valid hash attribute.
On a filesystem, the attribute names are
**`user.s3ar.format,`**
**`user.s3ar.bucket,`**
and
**`user.s3ar.bucket-acl`**
for bucket directories, plus
**`user.s3ar.key,`**
**`user.s3ar.etag,`**
**`user.s3ar.hash,`**
and
**`user.s3ar.metadata.NAME`**
for objects.

The tar pathname is an informational
**`BUCKET/KEY`**
encoded directly as UTF-8. It is not interpreted during S3 restore. A final
slash is represented as
**`%2F`**
because tar readers otherwise treat the regular object entry as a directory
and hide its body. This is only a pathname transport workaround; the PAX
identity remains authoritative.

Standard HTTP properties such as Content-Type, Content-Encoding, and
Cache-Control are not stored in the archive and are not restored.

GNU tar can restore these attributes on filesystems that support them, but
xattr handling must be enabled explicitly:

```sh
mkdir restored
tar --xattrs --xattrs-include='user.s3ar.*' -xf backup.tar -C restored
getfattr -d -m 'user.s3ar.*' restored/BUCKET
getfattr -d -m 'user.s3ar.*' restored/BUCKET/KEY
```

This is useful for inspecting or testing S3 metadata. It does not make the
archive safe or lossless to extract with a general-purpose tar program. Raw S3
keys can contain leading or trailing slashes, empty,
**`.`**
or
**`..`**
components, and an object such as
**`a`**
can collide with a prefix such as
**`a/b.`**
Use s3ar to restore archives to S3.

# EXTRACTING ARCHIVES

At least one S3 URI is required for
**`--extract.`**
Use
**`s3://`**
to restore the whole archive, or select buckets and prefixes as described in
**`SELECTING S3 RESOURCES:`**

```sh
# Whole archive
s3ar -x -f media.tar s3://

# One bucket
s3ar -x -f media.tar s3://photos

# One or two prefixes
s3ar -x -f media.tar s3://photos/2026
s3ar -x -f media.tar s3://photos/2026 s3://videos/2026
```

Without
**`-f,`**
the archive is read from standard input; selection works the same way:

```sh
s3ar -x s3://photos/2026 <media.tar
```

Bucket entries create buckets that do not exist yet. Object data is streamed
from libarchive into S3 PUT requests, overwriting objects with the same keys.

**`s3ar`**
uses no more than 10,000 multipart parts per object and automatically increases
the part size for large objects.

**`s3ar`**
accepts bucket directory entries and regular object entries with complete S3
identity attributes. It rejects incomplete or duplicate identity headers,
links, and other entry types. It accepts uncompressed and zstd-compressed
archives. A URI that matches no archive entry is an error.

Every object must have a preceding
**`BUCKET/`**
entry for its bucket. Bucket entries may be repeated. The corresponding
**`BUCKET/KEY`**
objects may therefore also repeat.
**`s3ar`**
creates or checks each destination bucket only once even when its archive
member repeats. Selecting an object or prefix also processes its bucket entry;
unselected buckets are not created.

On entries marked with
**`SCHILY.xattr.user.s3ar.format=1,`**
**`SCHILY.xattr.user.s3ar.metadata.NAME`**
values are restored as S3 user metadata. Only this metadata namespace is
used; unrelated attributes are ignored. Unmarked entries and unknown
format-marker values are rejected.

Bucket ACL summaries are informational and are not restored. New buckets and
uploaded objects use private ACLs; existing bucket ACLs are unchanged. With
**`-v,`**
restored names are logged as info on standard error without the
**`s3://`**
prefix.

# LISTING AN ARCHIVE

**`--list`**
reads an archive and prints its buckets and objects without contacting S3:

```sh
s3ar -t -f media.tar
```

Without
**`-f,`**
the archive is read from standard input. Selection URIs use the same rules as
extraction:

```sh
cat media.tar | s3ar -t s3://photos/2026
```

Bucket and object names are taken from the authoritative PAX bucket and key
attributes, not from the informational tar pathname. Names are quoted as
described in
**`NAME QUOTING.`**

Bucket directory members are printed as bucket-name-only lines, so empty
buckets remain visible. With
**`-v,`**
object lines include these fields:

```sh
BUCKET
BUCKET/KEY SIZE LAST_MODIFIED ETAG HASH
```

Size in bytes and modification time in seconds since the Unix epoch come
from the archive member. The ETag is an optional informational PAX
attribute; entries without it show
**`-.`**
The hash field contains the stored sha512: value, or none when no hash is
available. Displaying the hash does not verify the object content.

# LISTING LIVE S3 RESOURCES

Use
**`s3ar-list(1)`**
to list live buckets and objects.

# ADDITIONAL EXAMPLES

Restore the `photos` bucket from `media.tar` into `photos2`, keeping object keys
unchanged:

```sh
s3ar -xvf media.tar s3://photos2 --transform='s|^photos/|photos2/|'
```

For example, `photos/2026/photo1.jpg` becomes `photos2/2026/photo1.jpg`.
The selection URI names the destination after transformation: `s3://photos2`,
not the original `s3://photos`. Object transforms can also introduce another
destination bucket, which is initialized before upload.

Creation processes operands in command-line order. Extraction and archive
listing process entries once in archive order, using the URIs as selection
filters. Processing a bucket entry alone does not satisfy a key selection.

# EXIT STATUS

**`0`**

Archive creation, extraction, listing, or help completed successfully.

**`2`**

Invalid usage, invalid configuration, allocation failure, S3 failure, or
output failure.

Fatal errors go to standard error and start with
**`s3ar:.`**
When an S3 operation fails after a retry, the diagnostic includes the number
of attempts.

# SEE ALSO

**`getfattr(1),`**
**`s3ar-copy(1),`**
**`s3ar-delete(1),`**
**`s3ar-get(1),`**
**`s3ar-list(1),`**
**`s3ar-put(1),`**
**`tar(1),`**
**`regex(7)`**
