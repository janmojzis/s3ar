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
sudo apt install build-essential libarchive-dev nettle-dev libcurl4-openssl-dev librandombytes-dev libssl-dev libxml2-dev
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
./s3ar -x -f photos.tar s3://photos
```

## Streaming a backup between endpoints

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

This example uses a fixed temporary object key for simplicity. For real use,
choose a unique temporary key for each run so overlapping backups cannot
publish or delete each other's temporary archive.

## Commands

All commands use the S3 configuration above.

| Command | Purpose and documentation |
| --- | --- |
| [s3ar](s3ar.md) | Create tar archives from S3, restore them to S3, or list archived entries. |
| [s3ar-copy](s3ar-copy.md) | Copy objects, prefix contents, or whole buckets on the same S3 endpoint. |
| [s3ar-delete](s3ar-delete.md) | Delete objects, versions, multipart uploads, or whole buckets. |
| [s3ar-get](s3ar-get.md) | Download an object to a file or standard output, optionally computing SHA-512. |
| [s3ar-list](s3ar-list.md) | List live buckets and objects, optionally adding metadata and other fields. |
| [s3ar-put](s3ar-put.md) | Upload a file or standard input to an object. |

The command manuals document options, selection rules, examples, metadata,
output, and exit status. In particular, prefix selection in `s3ar-copy`
differs from the selection rules used by archive commands, listing, and deletion.
