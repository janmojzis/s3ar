---
title: S3AR-GET
section: 1
date: September 2026
footer: s3ar
header: User Commands
---

# NAME

s3ar-get - download an S3 object

# SYNOPSIS

```text
s3ar-get [-v|-vv|-vvv] [--hash] [-f FILE [-t TEMP]] s3://BUCKET/KEY
```

# DESCRIPTION

**`s3ar-get`**
downloads an object to standard output, or to
*`FILE`*
when
**`-f`**
is specified. Standard output is synchronized when it is a regular file and
closed before success is reported. FILE is replaced atomically only after the
download succeeds.
Without
**`-t,`**
a temporary file is created beside FILE using
**`mkstemp(3).`**

When an existing regular FILE has matching
**`user.s3ar.format,`**
**`user.s3ar.bucket,`**
**`user.s3ar.key,`**
and a valid
**`user.s3ar.etag`**
extended attribute, the first GET sends
**`If-None-Match`**
with that ETag. Bucket and key use the same URL encoding as the attributes in
archives created by
**`s3ar(1).`**
An HTTP 304 response leaves FILE unchanged. A downloaded replacement receives
these attributes when the filesystem supports them; otherwise future calls
download normally. Without **`--hash`**, the attributes do not verify local file contents.
A success summary is always written to standard error. With
**`-v,`**
an additional message distinguishes downloaded from not modified.

An interrupted download resumes from the first byte not yet written, using
**`Range`**
and
**`If-Match`**
to stay on the object selected by the initial response. Object properties and
S3 user metadata must remain identical on every resumed response. A mismatch
is treated as a protocol error and stops the download rather than combining
responses from different snapshots or switching to a newer object. An existing
regular
*`FILE`*
remains unchanged after such a failure. Standard output can already contain
bytes written before the failure.

**`-f -`**
selects standard output explicitly.

The operand names exactly one object. KEY is literal, not URL-decoded, and
a trailing slash is part of the key. Quote operands containing spaces or shell
metacharacters.

# OPTIONS

**`-f FILE, --file=FILE`**

Write data to FILE instead of standard output.

FILE is replaced atomically after a successful download. The new file uses
mode 0600 modified by the current umask. An existing FILE must be a regular
file; devices, directories, and symbolic links are rejected.

**`-t TEMP, --temporary=TEMP`**

Use TEMP as the temporary file when writing to FILE. TEMP must not exist and
must be on the same filesystem as FILE. On success, TEMP replaces FILE
atomically. TEMP is removed if the download fails.

**`--hash`**

Compute SHA-512 while downloading and store
**`user.s3ar.hash`**
as sha512: followed by 128 lowercase hexadecimal digits, without a terminating
NUL. The hash covers the object contents and is reported at info verbosity.
Without this option, a downloaded FILE receives the hash value none.

Before using FILE's cached ETag, verify its contents against its SHA-512
attribute. Missing, invalid, unavailable, or mismatching hashes disable the
conditional GET and cause a full download. Matching format, bucket, key, and
valid ETag attributes are still required. HTTP 304 preserves the verified FILE.

Before downloading to a regular file through standard output, remove all
existing user.s3ar.* attributes, even without --hash. Other attributes are
preserved. Failure to list or remove the old attributes aborts the download;
filesystems without xattr support are allowed.
After a successful download, store the new format, bucket, key, ETag, and hash
attributes only when writing began at offset zero without append mode, and the
final position and file size equal the downloaded length. Without --hash,
the hash value is none. If these file conditions are not met, a regular-file
output gets a warning and no new attributes. Missing or invalid response ETags
prevent storing identity attributes. Failed downloads leave the old attributes
removed.
Pipes and terminals receive no attributes. Attribute write failures are warnings.

The downloaded hash is computed, not checked against an independent S3 digest.
No second read of newly downloaded data is performed.

**`-v, -vv, -vvv, --verbose`**

Increase diagnostic verbosity to info, debug, or HTTP trace. Further repetitions of
**`-v`**
have no effect. Fatal errors are shown at every level.

**`-h, --help`**

Display command usage on standard output and exit successfully.

# EXAMPLES

Download an object to standard output:

```sh
s3ar-get s3://photos/original.jpg >original.jpg
```

Download to a file that is replaced only after success:

```sh
s3ar-get -f original.jpg s3://photos/original.jpg
```

Compute SHA-512 and verify the local file before reusing its cached ETag:

```sh
s3ar-get --hash -f original.jpg s3://photos/original.jpg
```

Choose an explicit temporary file on the same filesystem:

```sh
s3ar-get -f original.jpg -t original.jpg.tmp s3://photos/original.jpg
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

The download or help completed successfully.

**`2`**

A fatal error occurred, including an invalid command line, S3 configuration,
download failure, or output-file failure.

When an S3 operation fails after a retry, the diagnostic includes the number
of attempts.

# SEE ALSO

**`getfattr(1),`**
**`s3ar(1),`**
**`s3ar-copy(1),`**
**`s3ar-delete(1),`**
**`s3ar-list(1),`**
**`s3ar-put(1)`**
