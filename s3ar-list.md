---
title: S3AR-LIST
section: 1
date: October 2026
footer: S3 Archiver
header: User Commands
---

# NAME

s3ar-list - list live S3 buckets and objects

# SYNOPSIS

```text
s3ar-list [-v] [-b] [-o] [FIELDS] [--delimiter STRING] s3://[BUCKET[/KEY]]...
s3ar-list -b [-v] s3://
```

# DESCRIPTION

**`s3ar-list`**
lists live S3 buckets and objects selected by one or more S3 URIs. The URI
**`s3://`**
selects all buckets and their objects. A bucket URI selects that bucket and
its objects; a key URI selects the exact key and keys below KEY/.
Matches stop at path boundaries, and a trailing slash does not change the
selection. See
**`s3ar(1)`**
for selection examples.

Bucket and key names in operands are literal, not URL-decoded. Quote operands
containing spaces or shell metacharacters. Operands are processed in order;
overlapping selections produce repeated output. A key selection that matches
neither an object nor descendants is an error. Empty buckets are valid.

Listing results are written to standard output; diagnostics go to standard
error. When bucket output is enabled, bucket names are printed before their
selected objects, including for key selections. Names have the
**`s3://`**
prefix and use the byte-oriented URL encoding described in
**`s3ar(1).`**

The URI is followed by requested fields in command-line option order,
without field names or prefixes. Repeated requests for the same field use
its first position. Object fields apply to object rows; bucket ACL applies
to bucket rows.

Fields are separated by a space by default. Records end with a newline.
Missing values produce empty fields, including at the end of a record.
Selected metadata values are printed as received, without added quoting or
encoding. For parsing, use
**`--delimiter`**
to choose a separator that does not occur in the values and preserve empty
fields and trailing separators.

For example:

```sh
s3ar-list -o --object-meta source --object-size --delimiter '|' s3://bucket/
s3://bucket/a|manual upload|123
s3://bucket/b||456
```

# OPTIONS

**`-b, --buckets`**

Include bucket rows. Without
**`-o,`**
list buckets only and require exactly one operand,
**`s3://.`**

**`-o, --objects`**

Include object rows. Without
**`-b,`**
omit bucket rows; an empty bucket produces empty successful output.
Combine as
**`-bo`**
for both row types. If neither option is given, both row types are enabled.

**`--object-size`**

Append object size in bytes.

**`--object-mtime`**

Append object modification time in seconds since the Unix epoch.

**`--object-etag`**

Append object ETag, or an empty field if unavailable.
Object field options require object output.

**`--object-metadata`**

Append all user metadata as one compact JSON object with keys sorted by name.
An object with no metadata produces
**`{}.`**
JSON strings use standard JSON escaping; the JSON object is not percent-encoded.

**`--object-meta NAME`**

Append one user metadata value. Repeat to select multiple fields.
Names are case-insensitive and omit the
**`x-amz-meta-`**
prefix. Missing and empty values both produce an empty field.
Can be combined with
**`--object-metadata.`**

Both metadata options require object output and make an additional HEAD
request for each listed object. Multiple metadata options share one HEAD
request per object. A failed HEAD request fails the listing.

**`--delimiter STRING`**

Separate fields with STRING instead of a space. Supports
**`\t,`**
**`\n,`**
**`\0,`**
and
**`\\`**
for tab, newline, NUL, and backslash. Other escapes and a trailing backslash
are errors. Quote the argument to preserve backslashes, for example
**`--delimiter '\t'.`**
An empty string joins fields without a separator. If repeated, the last
value is used. Record endings remain newlines.

**`--bucket-acl`**

Fetch and append bucket ACL information. Requires bucket output.

**`-v, --verbose`**

Increase diagnostic verbosity: one occurrence enables info diagnostics,
two enable debug, and three enable HTTP trace. Further occurrences have no
effect.

**`-h, --help`**

Display command usage on standard error and exit successfully.

# EXAMPLES

List all visible buckets:

```sh
s3ar-list -b s3://
```

List objects under a selection, adding modification time and size:

```sh
s3ar-list -o --object-mtime --object-size s3://photos/2026/
```

Select user metadata and size with a pipe separator:

```sh
s3ar-list -o --object-meta source --object-size --delimiter '|' s3://photos/
```

For example, a missing metadata value produces an empty field:

```text
s3://photos/a.jpg|manual upload|12345
s3://photos/b.jpg||456
```

List all user metadata as JSON, separating fields with a tab:

```sh
s3ar-list -o --object-metadata --delimiter '\t' s3://photos/
```

# SIGNALS

**`SIGUSR1`**

Increase diagnostic verbosity by one level, up to trace.

**`SIGUSR2`**

Decrease diagnostic verbosity by one level, down to fatal.

# ENVIRONMENT

**`S3AR_ENDPOINT,`**
**`S3AR_REGION,`**
**`S3AR_URI_STYLE,`**
**`S3AR_ACCESS_KEY,`**
**`S3AR_SECRET_KEY,`**
and
**`S3AR_SESSION_TOKEN`**
configure the S3 connection in the same way as for
**`s3ar(1).`**

# EXIT STATUS

**`0`**

The listing or help completed successfully.

**`1`**

S3 client initialization or standard-output flush failed.

**`2`**

Invalid usage, invalid configuration, allocation failure, or an S3 operation
failed.

When an S3 operation fails after a retry, the diagnostic includes the number
of attempts.

# SEE ALSO

**`s3ar(1),`**
**`s3ar-copy(1),`**
**`s3ar-delete(1),`**
**`s3ar-get(1),`**
**`s3ar-put(1)`**
