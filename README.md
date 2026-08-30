# S3 Archiver

`s3ar` (S3 Archiver) is a command-line utility for creating tar archives from
an S3-compatible object store and extracting those archives back to S3. In
addition to object data, the archives store S3 user metadata and bucket ACL
summaries in PAX `SCHILY` extended-attribute headers.

The command-line interface is intentionally designed to resemble the standard
`tar` utility. Archive creation and extraction use the familiar `-c`, `-x`,
`-f`, and `-v` options, and selection operands are processed in command-line
order. `s3ar` is not a full replacement for `tar`; it applies the same style
of operation to live S3 resources and supports only the options documented
below.

## Building

Install the build dependencies:

```sh
apt install build-essential libs3-dev libarchive-dev
```

Build the executable:

```sh
make
```

The resulting executable is `./s3ar`.

## S3 configuration

S3 access requires these environment variables:

```sh
export S3AR_ACCESS_KEY='access-key'
export S3AR_SECRET_KEY='secret-key'
export S3AR_ENDPOINT='https://s3.example.net'
export S3AR_URI_STYLE='path'
export S3AR_REGION='us-east-1'
```

`S3AR_ENDPOINT` contains a host and optional port, but no URL path. Its scheme
may be `http://` or `https://`; HTTPS is used when the scheme is omitted. One
trailing slash is accepted and removed.

`S3AR_URI_STYLE` accepts `path` or `virtual` and defaults to `path`.

`S3AR_REGION` selects the S3 region and defaults to `us-east-1` when it is
not set.

The bundled `libs3` interface does not support temporary session credentials,
so `S3AR_SESSION_TOKEN` is not currently supported.

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

Create and `--list-objects` require at least one S3 operand. Extract accepts
zero or more selection operands. Multiple operands are processed in
command-line order.
Without `-f`, or with `-f -`, create writes to standard output while extract
reads from standard input.

Extract automatically detects uncompressed and zstd-compressed tar streams.
Explicit `--zstd` rejects an uncompressed input. The option is not valid with
`--list-buckets` or `--list-objects`.

Names in listings, verbose output, and diagnostics use GNU tar's default
`escape` quoting style. Control characters are written as backslash escapes
such as `\n` and `\t`, a backslash is written as `\\`, and other non-printable
bytes use three-digit octal escapes. Printable characters, including spaces
and printable characters in the current locale, are written unchanged.

For example, list the `photos` and `videos` buckets in one invocation:

```sh
./s3ar --list-objects s3://photos s3://videos
```

## Selecting S3 resources

A trailing slash does not change the selection:

| Operand | Selection |
| --- | --- |
| `s3://` | All buckets and all their objects |
| `s3://BUCKET` | The named bucket and all its objects |
| `s3://BUCKET/` | The same as `s3://BUCKET` |
| `s3://BUCKET/NAME` | The exact `NAME` key and objects below `NAME/` |
| `s3://BUCKET/NAME/` | The same as `s3://BUCKET/NAME` |

Matching occurs only at a path boundary. Selecting `photo` can match the exact
key `photo` and keys such as `photo/first.jpg`, but never `photo1.jpg` or
`photo-old.jpg`. The exact key does not need to exist when matching descendants
exist.

An object selection that matches neither an exact key nor any descendant is an
error. A bucket selection succeeds for an empty bucket and still writes the
bucket URI.

For example, suppose S3 contains these objects:

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

Create always requires at least one S3 selection operand. To select everything,
pass `s3://` explicitly; omitting the operand is an error.

The following examples use the S3 objects shown above. Back up everything in S3:

```sh
./s3ar -c -f media.tar s3://
```

For this example, explicitly selecting both buckets archives the same objects:

```sh
./s3ar -c -f media.tar s3://photos s3://videos
```

Back up exactly one bucket and all its objects:

```sh
./s3ar -c -f photos.tar s3://photos
```

Back up only the photos from 2026:

```sh
./s3ar -c -f photos-2026.tar s3://photos/2026
```

Back up the photos and videos from 2026:

```sh
./s3ar -c -f media-2026.tar s3://photos/2026 s3://videos/2026
```

Without `-f`, the archive is streamed to standard output:

```sh
./s3ar -c s3://photos/2026 >photos-2026.tar
```

A new archive file is created with mode `0666` modified by the current `umask`.
Truncating an existing archive preserves its permissions.

Each selected bucket is stored once as a `BUCKET/` directory entry. Objects
are regular entries named `BUCKET/KEY`; their bodies are streamed from S3
directly into libarchive. Empty buckets therefore still produce an archive
member.

The restricted PAX archive stores S3 metadata as SCHILY extended attributes:

```text
SCHILY.xattr.s3ar.bucket-acl=public-read,custom
SCHILY.xattr.user.NAME=VALUE
```

Unsafe object keys containing empty, `.` or `..` path components are rejected.
Create is a weak snapshot: an object can change between LIST and GET. With
`-v`, archived member names are written without the `s3://` prefix to standard
error. This is independent of the archive destination and keeps a tar stream
on standard output unmodified.

## Extracting archives

Selection operands are optional with extract. With no S3 operand, every
accepted archive member is restored; one or more operands limit extraction to
matching members.

Assuming `media.tar` contains the full backup created above, extract all of it
back to S3:

```sh
./s3ar -x -f media.tar
```

Extract exactly the `photos` bucket and all its objects:

```sh
./s3ar -x -f media.tar s3://photos
```

Extract only the photos from 2026:

```sh
./s3ar -x -f media.tar s3://photos/2026
```

Multiple operands can select several branches. This extracts the photos and
videos from 2026:

```sh
./s3ar -x -f media.tar s3://photos/2026 s3://videos/2026
```

Without `-f`, the archive is read from standard input. Selection operands work
the same way as with an archive file:

```sh
cat media.tar | ./s3ar -x s3://photos/2026
```

Bucket directory entries create missing buckets. Object entries are streamed
directly from libarchive into S3 PUT requests and overwrite objects with the
same keys. Missing buckets needed by selected objects are created even when
the archive contains no bucket directory entry.

Only tar archives containing top-level bucket directories and regular
`BUCKET/KEY` object members are accepted. Absolute paths, empty, `.` and `..`
components, links, and other member types are rejected. Uncompressed and zstd
archives are accepted. A requested operand that matches no archive member is
an error.

`SCHILY.xattr.user.NAME` values are restored as S3 user metadata. Bucket ACL
summaries are informational and are not restored; new buckets and uploaded
objects use private ACLs. With `-v`, restored member names are written without
the `s3://` prefix to standard error.

## Listing buckets and objects

### Listing buckets

`--list-buckets` lists all bucket names without listing their objects:

```sh
./s3ar --list-buckets
```

The names are written to standard output, one per line:

```text
photos
videos
```

With `-v` or `--verbose`, every name is followed by a tab and its ACL summary:

```text
photos	acl=public-read,custom
videos	acl=private
```

All positional arguments passed to `--list-buckets` are ignored. In
particular, operands such as `s3://asdasd`, `s3://bucket/prefix`, and `s3://`
do not filter the output or cause those resources to be accessed.

### Listing objects

`--list-objects` requires one or more S3 selection operands. It writes every
selected object to standard output as `BUCKET/KEY`, without the `s3://` prefix,
one object per line. For example:

```sh
./s3ar --list-objects s3://photos/2026
```

produces:

```text
photos/2026/photo1.jpg
photos/2026/photo2.jpg
photos/2026/photoN.jpg
```

The operand `s3://` lists objects from every bucket. Buckets are processed in
sorted order. Object listings use S3 ordering and continue across pages of up
to 512 objects. Bucket names are not printed separately, so empty buckets
produce no output.

A bucket operand lists all objects in that bucket. An object operand lists the
exact object first, when it exists, followed by matching descendants in S3
order.

With `-v` or `--verbose`, each object is followed by its byte size and sorted
S3 user metadata, separated by tabs:

```text
photos/2026/photo1.jpg	1234	sha256=3472a7,source=camera
```

An object without user metadata uses `-`. Verbose object listings perform one
HEAD request per object to retrieve its user metadata.

## Exit status

- `0`: the listing or help completed successfully.
- `2`: invalid usage, invalid configuration, allocation failure, S3 failure,
  or output failure.

Fatal errors are written to standard error with an `s3ar:` prefix.

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

The test server imports the filesystem tree at startup. Restart it after
changing files directly in the test-data directory.
