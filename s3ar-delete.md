---
title: S3AR-DELETE
section: 1
date: September 2026
footer: s3ar
header: User Commands
---

# NAME

s3ar-delete - delete S3 objects and buckets

# SYNOPSIS

```text
s3ar-delete [-v|-vv|-vvv] [-n|--dry-run] s3://[BUCKET[/KEY]]
```

# DESCRIPTION

**`s3ar-delete`**
lists and deletes matching resources without prompting for confirmation.
It rescans each bucket after deleting a batch of object versions or aborting
a group of multipart uploads.
It permanently removes object versions and delete markers, aborts matching
multipart uploads, and removes selected buckets after they are empty.

**`s3://`**
selects every bucket visible to the configured credentials.
**`s3://BUCKET`**
and
**`s3://BUCKET/`**
select one entire bucket.
**`s3://BUCKET/KEY`**
and
**`s3://BUCKET/KEY/`**
select the exact KEY object and all objects below KEY/, keeping the bucket.
Bucket and key names in operands are literal, not URL-decoded.
Quote operands containing spaces or shell metacharacters.
A trailing slash does not change the selection, as with
**`s3ar(1).`**
Matches stop at path boundaries. For example,
**`s3://photos/photo`**
and
**`s3://photos/photo/`**
select photo, photo/, and photo/a, but not photo1 or photo-old.

Deletion stops on the first S3 error. Objects protected by retention or legal
hold cannot be deleted until their protection is removed. Objects added while
the command runs may remain, so the operation is not an atomic snapshot.

# OPTIONS

**`-v, --verbose`**

Show each matching resource. Repeat twice for debug messages or three times
for S3 request tracing. A summary is always printed on standard error;
per-resource messages also go to standard error. Send
**`SIGUSR1`**
to increase verbosity while running, or
**`SIGUSR2`**
to decrease it.

**`-n, --dry-run`**

Count resources that would be deleted without changing S3. Add
**`-v`**
to show each matching resource.

**`-h, --help`**

Display command usage on standard output and exit successfully.

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

# EXAMPLES

Preview deleting an exact key and its descendants without changing S3:

```sh
s3ar-delete --dry-run -v s3://photos/2026
```

Delete all versions, delete markers, and unfinished uploads matching that
key or its descendants, keeping the bucket:

```sh
s3ar-delete s3://photos/2026
```

Delete an entire bucket and its contents:

```sh
s3ar-delete s3://photos/
```

An empty key selection is accepted; there may be no matching resources to delete.

# EXIT STATUS

**`0`**

Deletion, dry run, or help completed successfully.

**`2`**

A fatal error occurred, including invalid arguments, a configuration error,
or an S3 failure.

# SEE ALSO

**`s3ar(1),`**
**`s3ar-copy(1),`**
**`s3ar-get(1),`**
**`s3ar-list(1),`**
**`s3ar-put(1)`**
