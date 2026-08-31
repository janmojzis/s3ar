# S3 Archiver

`s3ar` creates tar archives directly from S3-compatible storage and restores
them back to S3. It preserves object metadata and records bucket ACLs in PAX
headers.

Its command line follows the familiar `tar` style, including `-c`, `-x`, `-f`,
and `-v`. It is not a general replacement for `tar`: it works with live S3
resources and implements only the options described here.

## Building

Install the dependencies and build `s3ar`:

```sh
apt install build-essential libs3-dev libarchive-dev
make
```

The executable is written to `./s3ar`.

## S3 configuration

Set your S3 credentials and endpoint. The values below are only an example:

```sh
export S3AR_ACCESS_KEY='access-key'
export S3AR_SECRET_KEY='secret-key'
export S3AR_ENDPOINT='https://s3.example.net'
export S3AR_URI_STYLE='path'
export S3AR_REGION='us-east-1'
```

`S3AR_ENDPOINT` may contain a host and port, but not a URL path. Both `http://`
and `https://` are accepted; without a scheme, `s3ar` uses HTTPS. A single
trailing slash is harmless.

`S3AR_URI_STYLE` accepts `path` or `virtual` and defaults to `path`.

`S3AR_REGION` sets the location constraint used when creating a bucket that
does not already exist. It defaults to `us-east-1`; for that value, `s3ar`
omits the location constraint. The variable does not select a region for
requests involving existing buckets.

The bundled `libs3` interface does not support temporary session credentials,
so `S3AR_SESSION_TOKEN` is not available.

## Quick start

Back up the `photos` bucket and restore it later:

```sh
./s3ar -c -f photos.tar s3://photos
./s3ar -x -f photos.tar
```

Use `s3://` instead of a bucket name to include everything your credentials
can access.

## Command line

```text
s3ar (-c | --create) [-v | --verbose] [--zstd] [-f TARFILE] S3...
s3ar (-x | --extract) [-v | --verbose] [--zstd] [-f TARFILE] [S3...]
s3ar --list-buckets [-v | --verbose] [S3...]
s3ar --list-objects [-v | --verbose] S3...
s3ar (-h | --help)
```

The options are:

- `-c`, `--create`: create a tar archive from the selected S3 resources.
- `-x`, `--extract`, `--get`: extract a tar archive into S3.
- `--list-buckets`: list all bucket names without listing their objects;
  positional arguments are ignored.
- `--list-objects`: list objects in the selected live S3 resources.
- `-f`, `--file TARFILE`: read or write the archive named `TARFILE`.
- `--zstd`: create a zstd-compressed archive or require zstd when extracting.
- `-v`, `--verbose`: enable verbose output.
- `-h`, `--help`: display command-line help and exit successfully.

`--create` and `--list-objects` need at least one S3 URI. `--extract` does not:
without a URI, it restores the whole archive. Multiple URIs are processed in
command-line order.

Without `-f`, or with `-f -`, `--create` writes to standard output and
`--extract` reads from standard input.

`--extract` detects uncompressed and zstd-compressed tar streams automatically.
Passing `--zstd` explicitly rejects uncompressed input. This option cannot be
used with `--list-buckets` or `--list-objects`.

Names in listings, verbose output, and diagnostics use GNU tar's default
`escape` quoting style. Control characters are written as backslash escapes
such as `\n` and `\t`, a backslash is written as `\\`, and other non-printable
bytes use three-digit octal escapes. Printable characters, including spaces
and printable characters in the current locale, are written unchanged.

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

Each bucket becomes a `BUCKET/` directory entry. Its objects are stored as
regular `BUCKET/KEY` entries and streamed directly from S3 into libarchive.
The bucket entry comes first, so an empty bucket still appears in the archive.

Note: archive creation is not atomic. An object may change between the S3 LIST
and GET requests.

With `-v`, archived member names are written to standard error without the
`s3://` prefix. A tar stream written to standard output remains untouched.

### Archive format

S3-specific information is stored in namespaced SCHILY extended attributes:

```text
SCHILY.xattr.user.s3ar.format=1
SCHILY.xattr.user.s3ar.bucket-acl=public-read,custom
SCHILY.xattr.user.s3ar.metadata.NAME=VALUE
```

Every bucket and object carries the format marker. This prevents metadata from
older archives whose name begins with `s3ar.` from being mistaken for part of
the current format. On a filesystem, the attributes are
`user.s3ar.format` and `user.s3ar.bucket-acl` for bucket directories, plus
`user.s3ar.metadata.NAME` for objects.

GNU tar can restore these attributes on filesystems that support them, but
xattr handling must be enabled explicitly:

```sh
mkdir restored
tar --xattrs --xattrs-include='user.s3ar.*' -xf backup.tar -C restored
getfattr -d -m 'user.s3ar.*' restored/BUCKET
getfattr -d -m 'user.s3ar.*' restored/BUCKET/KEY
```

This is useful for inspecting or testing S3 metadata. It does not make the
archive a general filesystem backup.

Unsafe object keys containing empty, `.` or `..` path components are rejected.

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

Bucket entries create buckets that do not exist yet. Object data is streamed
from libarchive into S3 PUT requests, overwriting objects with the same keys.

An accepted archive contains top-level bucket directories and regular
`BUCKET/KEY` object entries. `s3ar` rejects absolute paths, empty, `.` or `..`
components, links, and other entry types. It accepts uncompressed and zstd
archives. A URI that matches no archive entry is an error.

Each `BUCKET/` entry must appear exactly once and before its objects. `s3ar`
creates or checks the bucket before uploading anything into it. Selecting an
object or prefix also processes its bucket entry; unselected buckets are not
created.

On entries marked with `SCHILY.xattr.user.s3ar.format=1`,
`SCHILY.xattr.user.s3ar.metadata.NAME` values are restored as S3 user metadata.
For unmarked entries from older releases, `SCHILY.xattr.user.NAME` is accepted
instead, including legacy names that begin with the now-reserved `s3ar.`
prefix. Unknown format-marker values are rejected.

Bucket ACL summaries are informational and are not restored. New buckets and
uploaded objects use private ACLs. With `-v`, restored names are written to
standard error without the `s3://` prefix.

## Listing buckets and objects

### Listing buckets

`--list-buckets` lists all bucket names without listing their objects:

```sh
./s3ar --list-buckets
```

Bucket names are written to standard output, one per line:

```text
photos
videos
```

Add `-v` to include the ACL summary after a tab:

```text
photos	acl=public-read,custom
videos	acl=private
```

`--list-buckets` ignores positional arguments. A URI such as
`s3://bucket/prefix` neither filters the output nor accesses that resource.

### Listing objects

`--list-objects` needs one or more S3 URIs. It prints matching objects as
`BUCKET/KEY`, one per line and without the `s3://` prefix:

```sh
./s3ar --list-objects s3://photos/2026
```

produces:

```text
photos/2026/photo1.jpg
photos/2026/photo2.jpg
photos/2026/photoN.jpg
```

The URI `s3://` lists objects from every bucket. Buckets are processed in
sorted order, while objects keep S3's ordering. Listings continue across pages
of up to 512 objects. Empty buckets produce no output because bucket names are
not printed separately.

A bucket URI lists the whole bucket. An object URI lists the exact object
first, if it exists, followed by matching descendants in S3 order.

Add `-v` to include the byte size, modification time as a Unix timestamp, and
ETag. The fields are separated by tabs:

```text
photos/2026/photo1.jpg	1234	1788104297	"3472a7..."
```

An unavailable ETag is shown as `-`. The values come directly from the object
listing; `s3ar` does not send a HEAD request for every object.

## Exit status

- `0`: the listing or help completed successfully.
- `2`: invalid usage, invalid configuration, allocation failure, S3 failure,
  or output failure.

Fatal errors go to standard error and start with `s3ar:`.

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

python3 ./s3testserver.py "$test_data" --host 127.0.0.1 --port 9000
```

In another shell, configure and run `s3ar`:

```sh
export S3AR_ENDPOINT='http://127.0.0.1:9000'
export S3AR_URI_STYLE='path'
export S3AR_REGION='us-east-1'
export S3AR_ACCESS_KEY='test-access'
export S3AR_SECRET_KEY='test-secret'

./s3ar --list-buckets
./s3ar --list-objects s3://
./s3ar --list-objects s3://photos s3://videos
./s3ar --list-objects s3://photos/2026
./s3ar --list-objects s3://photos/2026 s3://videos/2026

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
