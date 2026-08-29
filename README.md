# s3ar

`s3ar` is a command-line utility for listing buckets and objects in an
S3-compatible object store, creating tar archives from them, and extracting
those archives back to S3. Its member selection follows tar semantics: an
operand selects an exact member and all descendants below `MEMBER/`.

This README is the authoritative contract for the command-line interface,
configuration, and observable behavior.

## Building on Debian

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
unset or empty.

The bundled `libs3` interface does not support temporary session credentials,
so `S3AR_SESSION_TOKEN` is not supported.

## Command line

```text
s3ar (-c | --create) [-v | --verbose] [-f TARFILE] S3...
s3ar (-x | --extract) [-v | --verbose] [-f TARFILE] [S3...]
s3ar (-t | --list) [-v | --verbose] S3...
s3ar (-h | --help)
```

The options are:

- `-c`, `--create`: create a tar archive from the selected S3 resources.
- `-x`, `--extract`: extract a tar archive into S3.
- `-t`, `--list`: list the selected live S3 resource.
- `-f`, `--file TARFILE`: read or write the archive named `TARFILE`.
- `-v`, `--verbose`: enable verbose output.
- `-h`, `--help`: display command-line help and exit successfully.

Create and list require at least one S3 operand. Extract accepts zero or more
operands. Multiple operands are processed in command-line order. `-f` is valid
with create and extract. Without `-f`, or with `-f -`, create writes to
standard output and extract reads from standard input.

For example, list two buckets and all their objects in one invocation:

```sh
./s3ar -t s3://incoming/ s3://processed/
```

## S3 selection contract

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

## Creating archives

Create one bucket in a local tar file:

```sh
./s3ar -c -f album.tar s3://album
```

Without `-f`, the archive is streamed to standard output:

```sh
./s3ar -c s3://album/photo/ >photos.tar
```

Each selected bucket is stored once as a `BUCKET/` directory entry. Objects
are regular entries named `BUCKET/KEY`; their bodies are streamed from S3
directly into libarchive. Empty buckets therefore still produce an archive
member.

The restricted PAX archive stores S3 metadata as SCHILY extended attributes:

```text
SCHILY.xattr.s3ar.bucket-acl=public-read,custom
SCHILY.xattr.etag=ETAG
SCHILY.xattr.user.NAME=VALUE
```

Unsafe object keys containing empty, `.` or `..` path components are rejected.
Create is a weak snapshot: an object can change between LIST and GET. With
`-v`, created S3 URIs are written to standard error so a tar stream on standard
output is not corrupted.

## Extracting archives

Extract an archive into S3:

```sh
./s3ar -x -f album.tar
```

Without `-f`, the archive is read from standard input. Operands optionally
filter archive members using the same exact-member-and-descendants selection
as create and list:

```sh
./s3ar -x -f album.tar s3://album/photo
cat album.tar | ./s3ar -x s3://album/photo
```

Bucket directory entries create missing buckets. Object entries are streamed
directly from libarchive into S3 PUT requests and overwrite objects with the
same keys. Missing buckets needed by selected objects are created even when
the archive contains no bucket directory entry.

Only tar archives containing top-level bucket directories and regular
`BUCKET/KEY` object members are accepted. Absolute paths, empty, `.` and `..`
components, links, and other member types are rejected. Compressed archives
are not accepted. A requested operand that matches no archive member is an
error.

`SCHILY.xattr.user.NAME` values are restored as S3 user metadata. Stored ETags
and bucket ACL summaries are informational and are not restored; new buckets
and uploaded objects use private ACLs. With `-v`, extracted S3 URIs are written
to standard error.

## Listing output

Every selected bucket or object is written to standard output as a reusable S3
URI, one entry per line:

```text
s3://photos
s3://photos/2026/first.jpg
s3://photos/2026/second.jpg
```

Listing `s3://` writes each bucket followed by its objects. Bucket names are
sorted. Object listings use S3 ordering and continue across pages of up to 512
objects.

Bucket selections write the bucket URI followed by all its object URIs. Object
selections write the exact object first, when it exists, followed by matching
descendants in S3 order.

With `-v` or `--verbose`, buckets include their ACL summary. Objects include
their byte size followed by sorted S3 user metadata:

```text
s3://photos    acl=public-read,custom
s3://photos/first.jpg    1234    sha256=3472a7,source=camera
```

An object without user metadata uses `-`. An ACL unavailable from the S3
endpoint is written as `acl=unavailable`. Verbose object listings perform one
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
mkdir -p "$test_data/photos/2026" "$test_data/empty"
printf 'first\n' >"$test_data/photos/2026/first.jpg"
printf 'second\n' >"$test_data/photos/second.jpg"

python3 ../s3testserver.py "$test_data" --host 127.0.0.1 --port 9000
```

In another shell, configure and run `s3ar`:

```sh
export S3AR_ENDPOINT='http://127.0.0.1:9000'
export S3AR_URI_STYLE='path'
export S3AR_REGION='us-east-1'
export S3AR_ACCESS_KEY='test-access'
export S3AR_SECRET_KEY='test-secret'

./s3ar -t s3://
./s3ar -t s3://photos
./s3ar -t s3://photos/
./s3ar -t s3://photos/second.jpg
./s3ar -t s3://photos/2026/
./s3ar -c -f photos.tar s3://photos
./s3ar -x -f photos.tar
```

The test server imports the filesystem tree at startup. Restart it after
changing files directly in the test-data directory.
