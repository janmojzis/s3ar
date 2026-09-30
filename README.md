# S3 Archiver

`s3ar` creates tar archives directly from S3-compatible storage and restores
them back to S3. It preserves S3 user metadata and records informational
bucket ACL summaries in PAX headers.

Its command line follows the familiar `tar` style, including `-c`, `-x`, `-f`,
and `-v`. It is not a general replacement for `tar`: it works with live S3
resources and implements only the options described here.

## Installation

Install the build dependencies, then build and install into `/usr/local`.
libcurl 8.19.0 or newer is required; use `make install PREFIX=/path` to
choose another installation directory.

```sh
sudo apt install build-essential libarchive-dev libcurl4-openssl-dev librandombytes-dev libssl-dev libxml2-dev pkgconf
make
sudo make install
```

## S3 configuration

Set your S3 credentials and endpoint. The values below are only an example:

```sh
export S3AR_ACCESS_KEY='access-key'
export S3AR_SECRET_KEY='secret-key'
export S3AR_ENDPOINT='https://s3.example.net'
export S3AR_URI_STYLE='path'
export S3AR_REGION='us-east-1'
export S3AR_SESSION_TOKEN='temporary-session-token' # optional
```

`S3AR_ENDPOINT` must use `http://` or `https://` and may contain a host and
port, but not a URL path or trailing slash.

`S3AR_URI_STYLE` accepts `path` or `virtual` and defaults to `path`.

`S3AR_REGION` selects the SigV4 signing region and the location constraint used
when creating a bucket that does not already exist. It defaults to
`us-east-1`; for that value, `s3ar` omits the location constraint.

`S3AR_SESSION_TOKEN` supplies an optional token for temporary credentials.

## Quick start

Back up the `photos` bucket and restore it later:

```sh
./s3ar -c -f photos.tar s3://photos
./s3ar -x -f photos.tar
```

Use `s3://` instead of a bucket name to include everything your credentials
can access.

Upload a stream as one S3 object:

```sh
producer | ./s3ar-put s3://bucket/key
./s3ar-put -f local-file s3://bucket/key
./s3ar-put --create-bucket -f local-file s3://bucket/key
./s3ar-put --multipart-size 64M -f local-file s3://bucket/key
./s3ar-get s3://bucket/key > local-file
./s3ar-get -f local-file s3://bucket/key
./s3ar-delete --dry-run s3://bucket/photo/
./s3ar-delete s3://bucket/photo/
./s3ar-list -b s3://
./s3ar-list s3://bucket/
```

`s3ar-list` lists live buckets with `-b s3://` and live objects with S3 URI
operands. Results are printed to standard output; diagnostics go to standard
error.

`s3ar-delete` deletes without prompting and prints a summary on success. Use
`--dry-run` to leave S3 unchanged, and add `-v` to list matching resources.
`-vv` enables debug messages; `-vvv` traces S3 requests.
`s3://` removes every accessible bucket, `s3://BUCKET` removes one bucket,
`s3://BUCKET/KEY` removes only that exact key, and a key ending in `/` selects
every key beginning with that prefix while keeping the bucket. Unlike `s3ar`
archive selections, `s3://BUCKET/photo` does not select `photo/a`.
The command removes object versions and delete markers and aborts matching
multipart uploads. Deletion stops at the first error, leaving any resources
not yet processed in place.

`s3ar-put` always uses multipart upload. It reads at most one multipart part at
a time, so standard input does not need a known size and is not staged in a
temporary file. `--multipart-size SIZE` selects a part size from `5M` through
`5G`; only the binary suffixes `M` and `G` are accepted. The default is `16M`.
`-f -` is equivalent to omitting `-f`.
`--create-bucket` checks the destination bucket before uploading and creates it
if it is missing, using `S3AR_REGION`. Existing buckets are used without
modification; other errors, including access denied, stop the command.
Without this option, the bucket must already exist.
Transient errors while completing a multipart upload are retried. If the
completion response is lost and the final state cannot be determined, the
command reports that the outcome is uncertain and does not send an abort.
Inspect the destination object before retrying the command. The same behavior
applies to multipart uploads during `s3ar --extract`.
`-v` selects info verbosity, `-vv` debug, and `-vvv` HTTP tracing for
`s3ar-put`.

`s3ar-get` streams an object to standard output or to `-f FILE`. Named output
is written to a temporary file and atomically renamed after a successful
download. Without `-t`, the temporary file is created beside FILE using
`mkstemp()`. With `-t TEMP`, the specified path is used and must not exist.
`-v` selects info verbosity, `-vv` debug, and `-vvv` HTTP tracing. Further
`-v` options have no effect; fatal errors are reported at every level.

While `s3ar-get` or `s3ar-put` runs, `SIGUSR1` raises verbosity by one level
(up to trace) and `SIGUSR2` lowers it by one level (down to fatal).

HTTP tracing writes one bounded line per sent request and one result line per
transfer attempt to standard error. A retry adds a line with its reason and
planned delay. Traces include status, timing, byte counts, and selected request
details; they omit bodies, credentials, authorization headers, session tokens,
and values of URL query parameters other than known numeric controls. A
connection failure before any HTTP request is sent has `wire=0` in its result.

The new file uses mode `0600` modified by the umask; an existing FILE must be
a regular file.
For an existing regular file carrying matching `user.s3ar.format`,
`user.s3ar.bucket`, `user.s3ar.key`, and `user.s3ar.etag` attributes,
`s3ar-get` sends `If-None-Match`. A `304 Not Modified` response preserves the
file and its timestamps. Successful downloads save the same attribute names
and identity encoding as `s3ar` archives when filesystem xattrs are available.
Successful downloads and unchanged objects are reported on standard error.
An archive can be unpacked locally with `tar --xattrs` and an individual file
then refreshed with `s3ar-get -f FILE s3://BUCKET/KEY`.

## Command line

```text
s3ar (-c | --create) [-v | --verbose] [--zstd] [-f TARFILE] S3...
s3ar (-x | --extract) [-v | --verbose] [--zstd] [--transform EXPR] [-f TARFILE] [S3...]
s3ar (-t | --list) [-v | --verbose] [--zstd] [--transform EXPR] [-f TARFILE] [S3...]
s3ar (-h | --help)
```

The options are:

- `-c`, `--create`: create a tar archive from the selected S3 resources.
- `-x`, `--extract`, `--get`: extract a tar archive into S3.
- `-t`, `--list`: list archived objects without connecting to S3.
- `-f`, `--file TARFILE`: read or write the archive named `TARFILE`.
- `--zstd`: create a zstd-compressed archive or require zstd when extracting.
- `--transform EXPR`: rename archived `BUCKET/KEY` identities during extraction
  or archive listing; repeat to apply substitutions in command-line order.
- `-v`, `--verbose`: increase log verbosity; repeat for debug and trace.
- `-h`, `--help`: display command-line help on standard error and exit successfully.

For `s3ar`, `-v` enables info messages, `-vv` debug, and `-vvv` trace. During
execution, `SIGUSR1` raises the log level by one step and `SIGUSR2` lowers it.
The archive listing option `-v` also adds object metadata to its stdout rows.

`--create` needs at least one S3 URI. `--extract` and `--list` do not: without
a URI, they process the whole archive. Multiple URIs are processed in
command-line order.

Without `-f`, or with `-f -`, `--create` writes to standard output and
`--extract` and `--list` read from standard input.

`--extract` and `--list` detect uncompressed and zstd-compressed tar streams
automatically. Passing `--zstd` explicitly rejects uncompressed input.

S3 bucket and key names in listings, verbose output, and diagnostics use
byte-oriented URL encoding. `s3ar` prints member names as `BUCKET/KEY`, without
the `s3://` scheme used in selection arguments. ASCII letters, digits, `-`, `.`, `_`, `~`, and `/`
are written unchanged; every other byte is written as `%HH` with uppercase hexadecimal
digits. A space is therefore `%20`, a literal `%` is `%25`, and UTF-8 names
are encoded byte by byte. The format is independent of the current locale.

## Selecting S3 resources

A trailing slash does not change the selection:

| Operand | Selection |
| --- | --- |
| `s3://` | All buckets and all their objects |
| `s3://BUCKET` | The named bucket and all its objects |
| `s3://BUCKET/` | The same as `s3://BUCKET` |
| `s3://BUCKET/NAME` | The exact `NAME` key and objects below `NAME/` |
| `s3://BUCKET/NAME/` | The same as `s3://BUCKET/NAME` |

Matches stop at path boundaries. For example, `photo` matches the exact key
`photo` and keys below `photo/`, but not `photo1.jpg` or `photo-old.jpg`. The
exact key itself does not need to exist if matching descendants do.

If a URI matches neither an object nor a prefix, the command fails. Empty
buckets are valid and still produce a bucket entry when creating an archive.

The examples below assume that S3 contains these objects:

```text
s3://photos/2026/photo1.jpg
s3://photos/2026/photo2.jpg
s3://photos/2026/photoN.jpg
s3://photos/2027/photo1.jpg
s3://photos/2027/photo2.jpg
s3://photos/2027/photoN.jpg
s3://videos/2026/video1.jpg
s3://videos/2026/video2.jpg
s3://videos/2026/videoN.jpg
s3://videos/2027/video1.jpg
s3://videos/2027/video2.jpg
s3://videos/2027/videoN.jpg
```

## Creating archives

`--create` always needs an S3 URI.

Back up everything:

```sh
./s3ar -c -f media.tar s3://
```

The same backup, with both buckets named explicitly:

```sh
./s3ar -c -f media.tar s3://photos s3://videos
```

One bucket:

```sh
./s3ar -c -f photos.tar s3://photos
```

One prefix:

```sh
./s3ar -c -f photos-2026.tar s3://photos/2026
```

Two prefixes:

```sh
./s3ar -c -f media-2026.tar s3://photos/2026 s3://videos/2026
```

Without `-f`, the archive is streamed to standard output:

```sh
./s3ar -c s3://photos/2026 >photos-2026.tar
```

A new archive starts with mode `0666`, modified by the current `umask`.
Overwriting an existing archive keeps its permissions.
Regular archive files are written to a temporary file in the same directory
and atomically renamed only after the archive has been completed and synced.
Standard output and non-regular destinations are streamed directly.

Each selected bucket becomes a `BUCKET/` directory entry followed by its
selected objects, which are stored as regular UTF-8 `BUCKET/KEY` entries and
streamed directly from S3 into libarchive. Operands are processed independently,
so overlapping or repeated operands produce repeated bucket and object members.
An empty bucket still appears in the archive.

The archive file is installed atomically, but the S3 snapshot is not: an object
may change between the S3 LIST and GET requests. If a listed object disappears
before its GET starts, `s3ar` warns, skips that object, and completes the
archive. Other GET failures still stop archive creation.

An interrupted GET resumes from the first byte not yet written, using `Range`
and `If-Match` to stay on the object selected by the initial response. Object
properties and S3 user metadata are captured by that response and must remain
identical on every resumed response. A mismatch is deliberately treated as a
protocol error: `s3ar` stops instead of silently combining responses whose
snapshot identity is uncertain. This conservative "better safe than sorry"
policy never switches an in-progress download to a newer object. When the
archive is a regular file, the previously installed archive remains unchanged.

Object transfers are intentionally sequential because they are written to a
single streaming tar output. The implementation avoids redundant validation
requests but does not prefetch object bodies.

With `-v`, archived members are logged as `info` on standard error with the
`s3://` prefix. Object lines also contain byte size, modification time as a
Unix timestamp, ETag, and a final `-` reserved for SHA-512. A tar stream
written to standard output remains untouched.

### Archive format

S3-specific information is stored in namespaced SCHILY extended attributes:

```text
SCHILY.xattr.user.s3ar.format=1
SCHILY.xattr.user.s3ar.bucket=BUCKET
SCHILY.xattr.user.s3ar.key=URL-ENCODED-KEY
SCHILY.xattr.user.s3ar.etag="3472a7..."
SCHILY.xattr.user.s3ar.bucket-acl=public-read,custom
SCHILY.xattr.user.s3ar.metadata.NAME=VALUE
```

Every bucket and object carries the format marker and URL-encoded bucket name.
Object entries also carry their URL-encoded key and, when available, their
informational ETag. `/` remains unescaped in the identity attributes; bytes
outside the RFC 3986 unreserved set are encoded as uppercase `%HH`. The bucket
and key PAX attributes are the authoritative S3 identity.

The tar pathname is an informational `BUCKET/KEY` encoded directly as UTF-8.
It is not interpreted during S3 restore. A final slash is represented as
`%2F`, because tar readers otherwise treat the regular object entry as a
directory and hide its body. This is only a pathname transport workaround;
the PAX identity remains authoritative.

Standard HTTP properties such as `Content-Type`, `Content-Encoding`, and
`Cache-Control` are not stored in the archive and are not restored.

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
keys can contain leading or trailing slashes, empty, `.` or `..` components,
and pathnames such as `a` can collide with prefixes such as `a/b`. Use `s3ar`
to restore archives to S3. A future manifest-based pathname mapping can make
filesystem extraction safe without changing the authoritative PAX identity.

## Extracting archives

Without an S3 URI, `--extract` restores the whole archive:

```sh
./s3ar -x -f media.tar
```

To restore only part of it, use the same URI syntax described in
[Selecting S3 resources](#selecting-s3-resources). Restore the `photos` bucket:

```sh
./s3ar -x -f media.tar s3://photos
```

Restore one prefix:

```sh
./s3ar -x -f media.tar s3://photos/2026
```

Restore two prefixes:

```sh
./s3ar -x -f media.tar s3://photos/2026 s3://videos/2026
```

Without `-f`, the archive is read from standard input:

```sh
cat media.tar | ./s3ar -x s3://photos/2026
```

Extraction identifies S3 destinations from the URL-encoded `user.s3ar.bucket`
and `user.s3ar.key` attributes. The raw UTF-8 pathname is informational only.
Bucket entries create buckets that do not exist yet. Object data is streamed
from libarchive into S3 PUT requests, overwriting objects with the same keys.
Multipart restore uses at most 10,000 parts per object and automatically
increases the part size for large objects.

### Transforming destination names

`--transform` applies to the decoded `BUCKET/KEY` identity from the PAX
attributes, without a `s3://` prefix. Selection arguments match the resulting
identity after all substitutions. Bucket members are transformed as
`BUCKET/`; the first component of the result determines the destination
bucket. Object transforms can also introduce another destination bucket,
which is initialized before upload.

Restore the `uploads` bucket into `test`:

```sh
./s3ar -xvf archive.tar --transform='s|^uploads/|test/|'
```

Rename a key prefix and select the resulting destination prefix:

```sh
./s3ar -xvf archive.tar s3://uploads/new/path \
    --transform='s|^uploads/old/path/|uploads/new/path/|'
```

Preview the resulting names without connecting to S3:

```sh
./s3ar -tf archive.tar --transform='s|^uploads/|test/|'
```

Expressions use `s<delimiter>REGEX<delimiter>REPLACEMENT<delimiter>[gi]`,
with a punctuation delimiter such as `|` or `/` and POSIX basic regular
expressions. A substitution replaces the first match unless `g` requests
all matches. The `i` flag ignores case. Replacements support `&` for the
whole match, `\1` through `\9` for capture groups, and backslash escapes
for the delimiter, `&`, and backslash. Use `\(` and `\)` for capture groups.
Multiple expressions require repeated `--transform` options; semicolon
scripts, other flags, and GNU case-conversion escapes are rejected.
`--transform` is supported only with `-x` and `-t`.

Regular expressions operate on bytes in the C locale; Unicode character
classes and case folding are not supported. Empty global matches advance
over a complete UTF-8 character. A match or a capture used in the replacement
that splits a UTF-8 character is rejected.

Transformed names are validated before use. Offline previews use path-style
name validation; extraction additionally uses the actual endpoint's naming
rules. Existing destination objects are overwritten as during normal restore.

An accepted archive contains bucket directory entries and regular object
entries with complete S3 identity attributes. `s3ar` rejects incomplete or
duplicate identity headers, links, and other entry types. It accepts
uncompressed and zstd archives. A URI that matches no archive entry is an
error.

Every object must have a preceding `BUCKET/` entry for its bucket. Bucket
entries may be repeated. `s3ar` creates or checks each destination bucket only
once even when its archive member repeats. Selecting an object or prefix also
processes its bucket entry; unselected buckets are not created.

On entries marked with `SCHILY.xattr.user.s3ar.format=1`,
`SCHILY.xattr.user.s3ar.metadata.NAME` values are restored as S3 user metadata.
For unmarked entries from older releases, `SCHILY.xattr.user.NAME` is accepted
instead, including legacy names that begin with the now-reserved `s3ar.`
prefix. Unknown format-marker values are rejected.

Bucket ACL summaries are informational and are not restored. New buckets and
uploaded objects use private ACLs. With `-v`, restored names are logged as
`info` on standard error with the `s3://` prefix. Object lines end with `-`,
reserved for a future SHA-512 hash.

## Listing an archive

`--list` reads an archive and prints its buckets and objects without contacting
S3:

```sh
./s3ar -t -f media.tar
cat media.tar | ./s3ar -t s3://photos/2026
```

Selection URIs follow the extraction rules. Bucket and object names come from
the authoritative PAX bucket and key attributes, not from the informational
tar pathname, and use the same byte-oriented URL quoting as other listings.
Bucket directory members are printed as bucket-name-only lines, so empty
buckets remain visible. With `-v`, object lines include these fields:

```text
BUCKET
BUCKET/KEY SIZE LAST_MODIFIED ETAG HASH
```

Size and modification time come from the archive member. Archives created by
current versions store the ETag in an informational PAX attribute; older
archives without it show `-`. The hash field is currently `-`; SHA-512
calculation will be added later.

## Listing live S3 resources

Use `s3ar-list -b s3://` for buckets and `s3ar-list s3://[BUCKET[/KEY]]`
for objects.

## Exit status

- `0`: the listing or help completed successfully.
- `2`: invalid usage, invalid configuration, allocation failure, S3 failure,
  or output failure.

Fatal errors go to standard error and start with `s3ar:`.
When an S3 operation fails after a retry, its diagnostic includes the number
of attempts.

## Testing with the local S3 server

Prepare a filesystem-backed test store and start the server:

```sh
test_data=$(mktemp -d)
mkdir -p "$test_data/photos/2026" "$test_data/photos/2027"
mkdir -p "$test_data/videos/2026" "$test_data/videos/2027"

for year in 2026 2027; do
    for number in 1 2 N; do
        printf 'photo%s from %s\n' "$number" "$year" \
            >"$test_data/photos/$year/photo$number.jpg"
        printf 'video%s from %s\n' "$number" "$year" \
            >"$test_data/videos/$year/video$number.jpg"
    done
done

python3 -m pip install -r requirements-test.txt
python3 ./s3testserver.py "$test_data" --host 127.0.0.1 --port 9000
```

In another shell, configure and run `s3ar`:

```sh
export S3AR_ENDPOINT='http://127.0.0.1:9000'
export S3AR_URI_STYLE='path'
export S3AR_REGION='us-east-1'
export S3AR_ACCESS_KEY='test-access'
export S3AR_SECRET_KEY='test-secret'

./s3ar-list -b s3://
./s3ar-list s3://
./s3ar-list s3://photos s3://videos
./s3ar-list s3://photos/2026
./s3ar-list s3://photos/2026 s3://videos/2026

./s3ar -c -f media.tar s3://
./s3ar -c -f media-buckets.tar s3://photos s3://videos
./s3ar -c -f photos-2026.tar s3://photos/2026
./s3ar -c -f media-2026.tar s3://photos/2026 s3://videos/2026

./s3ar -x -f media.tar
./s3ar -x -f media.tar s3://photos/2026
./s3ar -x -f media.tar s3://photos/2026 s3://videos/2026
```

The test server reads the filesystem tree only at startup. Restart it after
changing anything directly in the test-data directory.

The directory tree cannot represent every valid S3 key. In particular, an
object named `a` cannot coexist with an object below `a/`, and keys containing
empty, `.` or `..` path components are rejected. Final path components starting
with `.s3-object-` are reserved for temporary files and are also rejected.
Such writes return HTTP 409.
S3 user metadata is stored in `.s3testserver/metadata.sqlite3`; an existing
`.s3testserver/metadata.json` is imported automatically on first use.
