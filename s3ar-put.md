---
title: S3AR-PUT
section: 1
date: September 2026
footer: s3ar
header: User Commands
---

# NAME

s3ar-put - upload a file or standard input to an S3 object

# SYNOPSIS

```text
s3ar-put [-v|-vv|-vvv] [-f FILE] [--create-bucket] [--multipart-size SIZE] s3://BUCKET/KEY
```

# DESCRIPTION

**`s3ar-put`**
reads data from standard input, or from
*`FILE`*
when
**`-f`**
is specified, and writes it to the selected S3 object using multipart upload.
Only one multipart part is buffered at a time.  The default part size is 16M.
An upload is limited to 10,000 parts; if the input exceeds that limit for the
selected part size, the upload fails and its multipart state is aborted.
Successful uploads are reported on standard error, even without
**`-v.`**

**`-f -`**
selects standard input explicitly.

The operand names exactly one object; an existing object with that key is
overwritten. KEY is literal, not URL-decoded, and a trailing slash is part of
the key. Quote operands containing spaces or shell metacharacters.

# OPTIONS

**`-f FILE, --file=FILE`**

Read data from FILE instead of standard input.

**`--create-bucket`**

Check the destination bucket before uploading and create it if it is missing,
using the configured S3 region.  Existing buckets are used without modification.
Other errors, including access denied, stop the command.
Without this option, the destination bucket must already exist.

**`--multipart-size SIZE`**

Set the multipart part size.  SIZE must be between 5M and 5G, inclusive.
Only the binary suffixes M (1024 squared bytes) and G (1024 cubed bytes) are
accepted.

**`-v, --verbose`**

Increase verbosity.  One occurrence selects informational messages; two
select debug messages; three select HTTP trace. Further occurrences have no effect.

**`-h, --help`**

Display command usage on standard output and exit successfully.

# EXAMPLES

Upload a local file:

```sh
s3ar-put -f original.jpg s3://photos/original.jpg
```

Upload standard input:

```sh
cat original.jpg | s3ar-put s3://photos/original.jpg
```

Create the destination bucket if necessary and use 64M multipart parts:

```sh
s3ar-put --create-bucket --multipart-size 64M -f backup.tar s3://backups/backup.tar
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

The upload or help completed successfully.

**`2`**

A fatal error occurred, including an invalid command line, S3 configuration,
bucket check or creation failure, upload failure, or input-file failure.

When an S3 operation fails after a retry, the diagnostic includes the number
of attempts.

# SEE ALSO

**`s3ar(1),`**
**`s3ar-copy(1),`**
**`s3ar-delete(1),`**
**`s3ar-get(1),`**
**`s3ar-list(1)`**
