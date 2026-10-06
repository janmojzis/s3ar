---
title: S3AR-COPY
section: 1
date: October 2026
footer: s3ar
header: User Commands
---

# NAME

s3ar-copy - copy S3 objects and prefix contents on the server

# SYNOPSIS

```text
s3ar-copy [OPTIONS] SOURCE... DEST
s3ar-copy [OPTIONS] -t DEST SOURCE...
```

# DESCRIPTION

**s3ar-copy**
copies objects on the configured S3 endpoint using server-side multipart copy.
Object data does not pass through the client. An empty object uses CopyObject.
Existing destination keys are overwritten; other destination objects remain.

Operands use s3://BUCKET/KEY. A bucket root can be written as s3://BUCKET or
s3://BUCKET/. Bucket sources require -r. With -r, a source ending in /
selects all objects under that literal prefix; other sources select exact
objects. Without -r, a source ending in / is an exact object key.

A destination bucket root, a destination ending in /, or a destination supplied
with -t is a target prefix. For an exact source, its basename is appended to
the target prefix (preserving trailing slashes of literal source objects).
For a recursive source, the source prefix is removed from each key and the
remainder is appended to the target prefix. Unlike cp, the source prefix's
last component is not automatically inserted into the destination.
Recursive copies and multiple sources require a target prefix.

Use -T to treat the destination as an exact object key even when it ends in /.
This requires one exact source object and a nonempty destination key.
An object cannot be copied onto itself or onto another exact source operand.
Overlapping recursive source and destination prefixes in the same bucket are
rejected. Multiple sources run in argument order; repeated target keys are
errors and the later write is not performed, even if the earlier copy failed.

Object errors are diagnosed and copying continues with other objects. A listing
error stops the affected source. Completed copies remain after a failure.
An empty source bucket succeeds; an unmatched source prefix fails. Objects
ending in / are copied too, except prefix markers that would map to an empty
key, which are skipped with a warning. The summary reports copied (or planned),
skipped, and failed operations. Only current objects are copied; bucket settings
and version history are not copied. Listings are processed one page at a time;
with multiple sources, destination keys are retained in memory to detect
collisions.

The source is checked with HEAD. Each copied part uses the source ETag as a
condition; if the source ETag changes, copying stops. User metadata, content
type, content encoding, cache control, content disposition, content language,
expiry, and object tags are applied to the destination. ACL and
server-side encryption settings are determined by the destination bucket.

For nonempty objects, tags are read with GetObjectTagging before starting the
multipart upload. This requires read access to source tags and, for tagged
objects, write access to destination tags (s3:GetObjectTagging and
s3:PutObjectTagging on AWS S3). If reading tags fails, the copy fails.
Metadata and tags are read before copying; ETag conditions do not detect
changes to metadata or tags that leave object data unchanged.

Keys are literal, not URL-decoded. Slashes and dot components are not normalized.
For a key beginning with /, use two slashes after the bucket name.
Quote operands containing spaces or shell metacharacters. Wildcards are not
expanded by s3ar-copy.

# OPTIONS

**-r, -R, --recursive**

Copy bucket or prefix contents recursively. Source prefixes must end in /.

**-t, --target-directory URI**

Copy all sources into this bucket or prefix. A nonempty prefix missing a
trailing slash gets one appended. Cannot be combined with -T.

**-T, --no-target-directory**

Treat the destination as an exact object key, including a trailing slash.

**--dry-run**

Print planned source-to-target mappings to standard output without writing.
Exact sources are checked with HEAD; recursive sources are listed. This does
not verify that later copying will succeed or that destination writes are
permitted. Does not create the destination bucket, even with --create-bucket.

**--create-bucket**

Create the destination bucket if it does not exist.

**--multipart-size SIZE**

Set the part size. SIZE must be between 5M and 5G, inclusive, with binary
suffix M or G. The default is 16M. At most 10,000 parts are allowed.

**-v, --verbose**

Increase verbosity. Repeat twice for debug messages or three times for HTTP
trace. Further repetitions have no effect.

**-h, --help**

Display command usage on standard output and exit successfully.

# EXAMPLES

Copy one object to an exact destination key:

```sh
s3ar-copy s3://a/dir/file.txt s3://b/new.txt
```

Copy all current objects from one bucket to another, preserving their keys:

```sh
s3ar-copy -r s3://bucket1/ s3://bucket2/
```

Copy the contents of a prefix into another prefix:

```sh
s3ar-copy -r s3://a/photos/ s3://b/backup/
```

For example, `photos/2026/a.jpg` becomes `backup/2026/a.jpg`. To keep
`photos` in the destination key, specify `s3://b/backup/photos/` instead.

Copy one object into a destination prefix, appending its basename:

```sh
s3ar-copy s3://a/dir/file.txt s3://b/backup/
```

This creates the key `backup/file.txt`.

Copy multiple objects into a target prefix:

```sh
s3ar-copy -t s3://b/backup/ s3://a/x.txt s3://a/y.txt
```

This creates `backup/x.txt` and `backup/y.txt`. With `-t`, the target
`s3://b/backup` would have the same meaning as `s3://b/backup/`.

Treat a destination ending in `/` as an exact object key:

```sh
s3ar-copy -T s3://a/dir/file.txt s3://b/backup/
```

This creates the exact key `backup/` without appending `file.txt`.

Copy literal keys beginning with `/`:

```sh
s3ar-copy -T s3://a//dir/file.txt s3://b//backup/
```

The first slash after the bucket separates the bucket from the key;
the second belongs to the key. This copies `/dir/file.txt` to `/backup/`.

Preview the source-to-target mappings without writing:

```sh
s3ar-copy --dry-run -r s3://a/ s3://b/
```

Create the destination bucket if necessary and choose a larger multipart
part size:

```sh
s3ar-copy -r --create-bucket --multipart-size 64M s3://a/ s3://b/
```

The default part size is 16M; increase it when a large object would exceed
the limit of 10,000 parts.

# OUTPUT

Normal copies emit diagnostics and summaries to standard error. With -v,
each successful copy is reported. Dry-run mappings are written to standard
output using URL-escaped names, as in other s3ar listings. Those escaped names
are display output; command operands still use literal keys.

# SIGNALS

**SIGINT, SIGTERM**

Stop copying and attempt to abort an unfinished multipart upload.

**SIGUSR1**

Increase diagnostic verbosity by one level.

**SIGUSR2**

Decrease diagnostic verbosity by one level.

# ENVIRONMENT

`S3AR_ENDPOINT`, `S3AR_REGION`, `S3AR_URI_STYLE`, `S3AR_ACCESS_KEY`,
`S3AR_SECRET_KEY`,
and
`S3AR_SESSION_TOKEN`
configure the S3 connection as for
**s3ar**(1).

# EXIT STATUS

**0**

All copies (or dry-run mappings) or help completed successfully. An empty
bucket or skipped root prefix marker does not cause failure.

**2**

Invalid arguments, configuration error, or S3 failure.

# SEE ALSO

**s3ar**(1), **s3ar-delete**(1), **s3ar-get**(1), **s3ar-list**(1),
**s3ar-put**(1)
