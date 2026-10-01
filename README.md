# S3 Archiver

`s3ar` creates tar archives directly from S3-compatible storage and restores
them back to S3. It preserves S3 user metadata and records informational
bucket ACL summaries in PAX headers.

Its command line follows the familiar `tar` style, including `-c`, `-x`, `-t`,
`-f` and `-v`. It is not a general replacement for `tar`.

If S3 keys use safe relative paths without file/directory conflicts, archives
can also be extracted to a local filesystem using GNU tar. With --xattrs
enabled, the S3 metadata stored in PAX headers is restored as extended
attributes on filesystems that support them.

## Installation

Install the build dependencies, then build and install into `/usr/local`.
libcurl 8.19.0 or newer is required; use `make install PREFIX=/path` to
choose another installation directory.

```sh
sudo apt install build-essential libarchive-dev libcurl4-openssl-dev librandombytes-dev libssl-dev libxml2-dev
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

Copy one object between buckets without downloading its data:

```sh
./s3ar-copy s3://photos/original.jpg s3://backup/original.jpg
```

The command uses multipart server-side copy for nonempty objects. Set
`--multipart-size 64M` for large objects when the default 16M would exceed
10,000 parts.

Back up the `photos` bucket and restore it later:

```sh
./s3ar -c -f photos.tar s3://photos
./s3ar -x -f photos.tar s3://photos
```

## Command line

```text
s3ar (-c | --create) [-v | --verbose] [--zstd] [-f TARFILE] S3...
s3ar (-x | --extract) [-v | --verbose] [--zstd] [--transform EXPR] [-f TARFILE] S3...
s3ar (-t | --list) [-v | --verbose] [--zstd] [--transform EXPR] [-f TARFILE] [S3...]
s3ar (-h | --help)
```

The options are:

- `-c`, `--create`: create a tar archive from the selected S3 resources.
- `-x`, `--extract`, `--get`: extract a tar archive into S3.
- `-t`, `--list`: list archived objects without connecting to S3.
- `-f`, `--file TARFILE`: read or write the archive named `TARFILE`.
- `--zstd`: create a zstd-compressed archive or require zstd when extracting
  or listing an archive.
- `--transform EXPR`: rename archived `BUCKET/KEY` identities during extraction
  or archive listing; repeat to apply substitutions in command-line order.
- `-v`, `--verbose`: increase log verbosity; repeat for debug and trace.
- `-h`, `--help`: display command-line help on standard error and exit successfully.

For `s3ar`, `-v` enables info messages, `-vv` debug, and `-vvv` trace. During
execution, `SIGUSR1` raises the log level by one step and `SIGUSR2` lowers it.
The archive listing option `-v` also adds object size, modification time as a
Unix timestamp, ETag, and a `-` placeholder for the hash to its stdout rows.

`--create` and `--extract` need at least one S3 URI. Use `s3://` to select
everything explicitly. `--list` processes the whole archive without a URI.
Archive creation processes URIs in command-line order. Extraction and archive
listing process entries in archive order, using the URIs as selection filters.

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

These selection rules apply to `s3ar -c`, `s3ar -x`, and `s3ar -t`.

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

GNU tar can restore these attributes on filesystems that support them, but
xattr handling must be enabled explicitly:

```sh
mkdir restored
tar --xattrs --xattrs-include='user.s3ar.*' -xf backup.tar -C restored
getfattr -d -m 'user.s3ar.*' restored/BUCKET
getfattr -d -m 'user.s3ar.*' restored/BUCKET/KEY
```

## Extracting archives

`--extract` always needs an S3 URI. To restore the whole archive, use `s3://`:

```sh
./s3ar -x -f media.tar s3://
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

Standard HTTP properties such as `Content-Type`, `Content-Encoding`, and
`Cache-Control` are not stored in the archive and are not restored.

### Transforming destination names

`--transform` applies to the decoded `BUCKET/KEY` identity from the PAX
attributes, without a `s3://` prefix. Selection arguments match the resulting
identity after all substitutions. Bucket members are transformed as
`BUCKET/`; the first component of the result determines the destination
bucket. Object transforms can also introduce another destination bucket,
which is initialized before upload.

Restore the `photos` bucket from `media.tar` into `photos2`, keeping object keys
unchanged:

```sh
./s3ar -xvf media.tar s3://photos2 --transform='s|^photos/|photos2/|'
```

For example, `photos/2026/photo1.jpg` becomes `photos2/2026/photo1.jpg`.
The selection URI names the destination after transformation: `s3://photos2`,
not the original `s3://photos`.

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
buckets remain visible.

## Additional utilities

`s3ar` also includes utilities for working directly with S3:

- `s3ar-get`: download an object to a file or standard output.
- `s3ar-put`: upload a file or standard input.
- `s3ar-copy`: copy one object between buckets on the same S3 endpoint.
- `s3ar-delete`: remove objects or buckets.
- `s3ar-list`: list live buckets and objects.

All use the same S3 configuration as `s3ar`.

For example, back up the `photos` bucket as a tarball stored in S3 without
writing an intermediate file to the local filesystem:

```bash
set -o pipefail
export S3AR_ACCESS_KEY='access-key2'
export S3AR_SECRET_KEY='secret-key2'
export S3AR_ENDPOINT='https://backup.example.net'

(
  export S3AR_ACCESS_KEY='access-key1'
  export S3AR_SECRET_KEY='secret-key1'
  export S3AR_ENDPOINT='https://source.example.net'
  s3ar -c s3://photos
) | s3ar-put s3://backups/photos.tar.tmp &&
s3ar-copy s3://backups/photos.tar.tmp s3://backups/photos.tar &&
s3ar-delete s3://backups/photos.tar.tmp
```

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

./s3ar -c -f media.tar s3://
./s3ar -c -f media-buckets.tar s3://photos s3://videos
./s3ar -c -f photos-2026.tar s3://photos/2026
./s3ar -c -f media-2026.tar s3://photos/2026 s3://videos/2026

./s3ar -x -f media.tar s3://
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
