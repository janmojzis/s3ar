import io
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import shutil
import subprocess
import tarfile
import threading
import urllib.parse

import botocore.exceptions
import pytest

from fault_server import FaultServer, ResponseStep


def run(executable, *arguments, env=None):
    return subprocess.run(
        [str(executable), *arguments],
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )


def zstd_bytes(data):
    if shutil.which("zstd") is None:
        pytest.skip("zstd command is required for interoperability tests")
    result = subprocess.run(
        ["zstd", "-q", "-c"],
        input=data,
        capture_output=True,
        check=False,
    )
    assert result.returncode == 0, result.stderr.decode()
    return result.stdout


def archive_bytes(buckets):
    output = io.BytesIO()
    with tarfile.open(fileobj=output, mode="w", format=tarfile.PAX_FORMAT) as archive:
        for bucket, objects in buckets.items():
            entry = tarfile.TarInfo(bucket)
            entry.type = tarfile.DIRTYPE
            entry.pax_headers = {
                "SCHILY.xattr.user.s3ar.format": "1",
                "SCHILY.xattr.user.s3ar.bucket": urllib.parse.quote(
                    bucket, safe="/-._~"
                ),
                "SCHILY.xattr.user.s3ar.bucket-acl": "private",
            }
            archive.addfile(entry)
            for key, data in objects.items():
                entry = tarfile.TarInfo(f"{bucket}/{key}")
                entry.size = len(data)
                entry.pax_headers = {
                    "SCHILY.xattr.user.s3ar.format": "1",
                    "SCHILY.xattr.user.s3ar.bucket": urllib.parse.quote(
                        bucket, safe="/-._~"
                    ),
                    "SCHILY.xattr.user.s3ar.key": urllib.parse.quote(
                        key, safe="/-._~"
                    ),
                    "SCHILY.xattr.user.s3ar.metadata.origin": "archive",
                }
                archive.addfile(entry, io.BytesIO(data))
    return output.getvalue()


def test_extract_retries_multipart_completion(
    executable, s3_environment, tmp_path
):
    archive = tmp_path / "retry-complete.tar"
    archive.write_bytes(archive_bytes({"bucket": {"key": b"x" * (16 * 1024 * 1024)}}))
    close = (("Connection", "close"),)
    upload_path = "/bucket/key?uploadId=restore-upload"
    steps = [
        ResponseStep("HEAD", "/bucket", 200, headers=close),
        ResponseStep("POST", "/bucket/key?uploads", 200,
                     b"<InitiateMultipartUploadResult><UploadId>restore-upload"
                     b"</UploadId></InitiateMultipartUploadResult>", close),
        ResponseStep("PUT", "/bucket/key?partNumber=1&uploadId=restore-upload",
                     200, headers=close + (("ETag", '"part"'),)),
        ResponseStep("POST", upload_path, 503,
                     b"<Error><Code>SlowDown</Code></Error>", close),
        ResponseStep("POST", upload_path, 200,
                     b"<CompleteMultipartUploadResult/>", close),
    ]
    with FaultServer(steps) as server:
        environment = {**s3_environment, "S3AR_ENDPOINT": server.endpoint}
        result = run(executable, "-xf", str(archive), env=environment)

    assert result.returncode == 0, result.stderr


def test_create_extract_round_trip_into_original_bucket(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="roundtrip-source")
    client.put_object(
        Bucket="roundtrip-source",
        Key="folder/item",
        Body=b"round trip data",
        Metadata={"owner": "s3ar"},
    )
    archive = tmp_path / "roundtrip.tar"
    created = run(
        executable,
        "-c",
        "-f",
        str(archive),
        "s3://roundtrip-source/",
        env=s3_environment,
    )
    assert created.returncode == 0, created.stderr
    client.delete_object(Bucket="roundtrip-source", Key="folder/item")

    extracted = run(
        executable,
        "-x",
        "-f",
        str(archive),
        "s3://roundtrip-source",
        env=s3_environment,
    )

    assert extracted.returncode == 0, extracted.stderr
    assert extracted.stderr == ""
    restored = client.get_object(Bucket="roundtrip-source", Key="folder/item")
    assert restored["Body"].read() == b"round trip data"
    assert restored["Metadata"] == {"owner": "s3ar"}


@pytest.mark.parametrize("body_size", [5, 17 * 1024 * 1024])
def test_empty_metadata_survives_round_trip(
    executable, s3_server, s3_environment, tmp_path, body_size
):
    _endpoint, client = s3_server
    bucket = f"empty-metadata-{body_size}"
    body = b"x" * body_size
    metadata = {"empty": "", "nonempty": "value"}
    client.create_bucket(Bucket=bucket)
    client.put_object(Bucket=bucket, Key="object", Body=body, Metadata=metadata)
    archive = tmp_path / "empty-metadata.tar"
    created = run(executable, "-c", "-f", str(archive), f"s3://{bucket}",
                  env=s3_environment)
    assert created.returncode == 0, created.stderr
    with tarfile.open(archive) as tar:
        assert tar.getmember(f"{bucket}/object").pax_headers[
            "SCHILY.xattr.user.s3ar.metadata.empty"
        ] == ""
    client.delete_object(Bucket=bucket, Key="object")
    restored = run(executable, "-x", "-f", str(archive), env=s3_environment)
    assert restored.returncode == 0, restored.stderr
    result = client.get_object(Bucket=bucket, Key="object")
    assert result["Body"].read() == body
    assert result["Metadata"] == metadata


def test_truncated_restore_aborts_multipart_upload(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    bucket = "truncated-multipart-restore"
    full_archive = archive_bytes(
        {bucket: {"large-object": b"x" * (20 * 1024 * 1024)}}
    )
    archive = tmp_path / "truncated-multipart.tar"
    archive.write_bytes(full_archive[: -(3 * 1024 * 1024)])

    result = run(
        executable,
        "-x",
        "-f",
        str(archive),
        f"s3://{bucket}",
        env=s3_environment,
    )

    assert result.returncode == 2
    assert "truncated" in result.stderr.lower()
    uploads = client.list_multipart_uploads(Bucket=bucket)
    assert uploads.get("Uploads", []) == []


def test_extract_does_not_retry_initiate_multipart_upload(
    executable, s3_environment, tmp_path
):
    class InitiateHandler(BaseHTTPRequestHandler):
        initiates = 0

        def log_message(self, _format, *_arguments):
            pass

        def reply(self, status, body=b"", **headers):
            self.send_response(status)
            self.send_header("Content-Length", str(len(body)))
            for name, value in headers.items():
                self.send_header(name.replace("_", "-"), value)
            self.end_headers()
            if body:
                self.wfile.write(body)

        def do_HEAD(self):
            self.reply(200)

        def do_POST(self):
            if urllib.parse.urlsplit(self.path).query == "uploads":
                type(self).initiates += 1
                if type(self).initiates == 1:
                    self.reply(503)
                else:
                    self.reply(
                        200,
                        b"<InitiateMultipartUploadResult>"
                        b"<UploadId>second-upload</UploadId>"
                        b"</InitiateMultipartUploadResult>",
                        Content_Type="application/xml",
                    )

    server = ThreadingHTTPServer(("127.0.0.1", 0), InitiateHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    environment = s3_environment.copy()
    environment["S3AR_ENDPOINT"] = f"http://127.0.0.1:{server.server_port}"
    bucket = "no-retry-initiate"
    archive = tmp_path / "no-retry-initiate.tar"
    archive.write_bytes(
        archive_bytes({bucket: {"large-object": b"x" * (16 * 1024 * 1024)}})
    )
    try:
        result = run(
            executable,
            "-xf",
            str(archive),
            f"s3://{bucket}",
            env=environment,
        )
    finally:
        server.shutdown()
        server.server_close()
        thread.join()

    assert result.returncode == 2
    assert InitiateHandler.initiates == 1


def test_failed_multipart_abort_is_retried_and_reported(
    executable, s3_environment, tmp_path
):
    class AbortHandler(BaseHTTPRequestHandler):
        aborts = 0

        def log_message(self, _format, *_arguments):
            pass

        def reply(self, status, body=b"", **headers):
            self.send_response(status)
            self.send_header("Content-Length", str(len(body)))
            for name, value in headers.items():
                self.send_header(name.replace("_", "-"), value)
            self.end_headers()
            if body:
                self.wfile.write(body)

        def do_HEAD(self):
            self.reply(200)

        def do_POST(self):
            if urllib.parse.urlsplit(self.path).query == "uploads":
                self.reply(
                    200,
                    b"<InitiateMultipartUploadResult>"
                    b"<UploadId>test-upload</UploadId>"
                    b"</InitiateMultipartUploadResult>",
                    Content_Type="application/xml",
                )
            else:
                self.reply(500)

        def do_PUT(self):
            length = int(self.headers.get("Content-Length", "0"))
            self.rfile.read(length)
            self.reply(200, ETag='"part-etag"')

        def do_DELETE(self):
            type(self).aborts += 1
            self.reply(503 if type(self).aborts == 1 else 400)

    server = ThreadingHTTPServer(("127.0.0.1", 0), AbortHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    environment = s3_environment.copy()
    environment["S3AR_ENDPOINT"] = f"http://127.0.0.1:{server.server_port}"
    bucket = "failed-abort-restore"
    full_archive = archive_bytes(
        {bucket: {"large-object": b"x" * (20 * 1024 * 1024)}}
    )
    archive = tmp_path / "failed-abort.tar"
    archive.write_bytes(full_archive[: -(3 * 1024 * 1024)])
    try:
        result = run(
            executable,
            "-x",
            "-f",
            str(archive),
            f"s3://{bucket}",
            env=environment,
        )
    finally:
        server.shutdown()
        server.server_close()
        thread.join()

    assert result.returncode == 2
    assert AbortHandler.aborts == 2
    assert "multipart abort failed" in result.stderr


def test_extract_prefers_url_encoded_identity_headers_over_path(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    archive = tmp_path / "identity-headers.tar"
    bucket_name = "identity-header-target"
    key = "folder/a b+%?#-ž"
    data = io.BytesIO()
    with tarfile.open(fileobj=data, mode="w", format=tarfile.PAX_FORMAT) as tar:
        bucket = tarfile.TarInfo("path-placeholder")
        bucket.type = tarfile.DIRTYPE
        bucket.pax_headers = {
            "SCHILY.xattr.user.s3ar.format": "1",
            "SCHILY.xattr.user.s3ar.bucket": bucket_name,
        }
        tar.addfile(bucket)
        entry = tarfile.TarInfo("path-placeholder/wrong-key")
        entry.size = 4
        entry.pax_headers = {
            "SCHILY.xattr.user.s3ar.format": "1",
            "SCHILY.xattr.user.s3ar.bucket": bucket_name,
            "SCHILY.xattr.user.s3ar.key": urllib.parse.quote(
                key, safe="/-._~"
            ),
        }
        tar.addfile(entry, io.BytesIO(b"data"))
    archive.write_bytes(data.getvalue())

    result = run(
        executable,
        "-x",
        "-f",
        str(archive),
        f"s3://{bucket_name}/",
        env=s3_environment,
    )

    assert result.returncode == 0, result.stderr
    restored = client.get_object(Bucket=bucket_name, Key=key)
    assert restored["Body"].read() == b"data"
    with pytest.raises(botocore.exceptions.ClientError):
        client.head_bucket(Bucket="path-placeholder")


def test_extract_rejects_path_without_identity_headers(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    archive = tmp_path / "path-only.tar"
    data = io.BytesIO()
    with tarfile.open(fileobj=data, mode="w", format=tarfile.PAX_FORMAT) as tar:
        bucket = tarfile.TarInfo("path-only-bucket")
        bucket.type = tarfile.DIRTYPE
        tar.addfile(bucket)
        entry = tarfile.TarInfo("path-only-bucket/object")
        entry.size = 1
        tar.addfile(entry, io.BytesIO(b"x"))
    archive.write_bytes(data.getvalue())

    result = run(
        executable, "-x", "-f", str(archive), "s3://", env=s3_environment
    )

    assert result.returncode != 0
    assert "S3 identity PAX headers" in result.stderr
    with pytest.raises(botocore.exceptions.ClientError):
        client.head_bucket(Bucket="path-only-bucket")


def test_extract_accepts_legacy_user_metadata_namespace(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    archive = tmp_path / "legacy-metadata.tar"
    data = io.BytesIO()
    with tarfile.open(fileobj=data, mode="w", format=tarfile.PAX_FORMAT) as tar:
        bucket = tarfile.TarInfo("legacy-metadata")
        bucket.type = tarfile.DIRTYPE
        bucket.pax_headers = {
            "SCHILY.xattr.user.s3ar.bucket": "legacy-metadata"
        }
        tar.addfile(bucket)
        entry = tarfile.TarInfo("legacy-metadata/object")
        entry.size = 1
        entry.pax_headers = {
            "SCHILY.xattr.user.s3ar.bucket": "legacy-metadata",
            "SCHILY.xattr.user.s3ar.key": "object",
            "SCHILY.xattr.user.origin": "legacy-archive",
            "SCHILY.xattr.user.s3ar.metadata.original": "legacy-prefixed",
        }
        tar.addfile(entry, io.BytesIO(b"x"))
    archive.write_bytes(data.getvalue())

    result = run(
        executable, "-x", "-f", str(archive), "s3://", env=s3_environment
    )

    assert result.returncode == 0, result.stderr
    restored = client.get_object(Bucket="legacy-metadata", Key="object")
    assert restored["Metadata"] == {
        "origin": "legacy-archive",
        "s3ar.metadata.original": "legacy-prefixed",
    }


@pytest.mark.parametrize("format_value", ["2", "3"])
def test_extract_rejects_unsupported_metadata_format(
    executable, s3_server, s3_environment, tmp_path, format_value
):
    _endpoint, client = s3_server
    archive = tmp_path / "unknown-metadata-format.tar"
    data = io.BytesIO()
    with tarfile.open(fileobj=data, mode="w", format=tarfile.PAX_FORMAT) as tar:
        bucket = tarfile.TarInfo("unknown-metadata-format")
        bucket.type = tarfile.DIRTYPE
        bucket.pax_headers = {
            "SCHILY.xattr.user.s3ar.bucket": "unknown-metadata-format"
        }
        tar.addfile(bucket)
        entry = tarfile.TarInfo("unknown-metadata-format/object")
        entry.size = 1
        entry.pax_headers = {
            "SCHILY.xattr.user.s3ar.format": format_value,
            "SCHILY.xattr.user.s3ar.bucket": "unknown-metadata-format",
            "SCHILY.xattr.user.s3ar.key": "object",
        }
        tar.addfile(entry, io.BytesIO(b"x"))
    archive.write_bytes(data.getvalue())

    result = run(
        executable, "-x", "-f", str(archive), "s3://", env=s3_environment
    )

    assert result.returncode != 0
    assert "unsupported archive metadata format" in result.stderr
    client.head_bucket(Bucket="unknown-metadata-format")


def test_extract_explicit_zstd_archive(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="extract-zstd")
    archive = tmp_path / "restore.tar.zst"
    archive.write_bytes(
        zstd_bytes(archive_bytes({"extract-zstd": {"object": b"zstd data"}}))
    )

    result = run(
        executable,
        "-x",
        "--zstd",
        "-f",
        str(archive),
        "s3://extract-zstd/",
        env=s3_environment,
    )

    assert result.returncode == 0, result.stderr
    restored = client.get_object(Bucket="extract-zstd", Key="object")
    assert restored["Body"].read() == b"zstd data"


def test_extract_zstd_option_rejects_plain_archive(
    executable, s3_environment, tmp_path
):
    archive = tmp_path / "plain.tar"
    archive.write_bytes(archive_bytes({"plain-zstd": {"object": b"data"}}))

    result = run(
        executable,
        "-x",
        "--zstd",
        "-f",
        str(archive),
        env=s3_environment,
    )

    assert result.returncode == 2
    assert result.stderr == "s3ar: fatal: archive is not zstd-compressed\n"


def test_extract_all_buckets(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    archive = tmp_path / "all-restore.tar"
    archive.write_bytes(
        archive_bytes(
            {
                "restore-first": {"one": b"first"},
                "restore-full": {"path/file": b"restored"},
            }
        )
    )

    result = run(
        executable, "-x", "-f", str(archive), "s3://", env=s3_environment
    )

    assert result.returncode == 0, result.stderr
    first = client.get_object(Bucket="restore-first", Key="one")
    assert first["Body"].read() == b"first"
    restored = client.get_object(Bucket="restore-full", Key="path/file")
    assert restored["Body"].read() == b"restored"
    assert restored["Metadata"] == {"origin": "archive"}


def test_extract_bucket_filter_skips_other_buckets(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    archive = tmp_path / "multiple.tar"
    archive.write_bytes(
        archive_bytes({"filter-skip-first": {"same": b"1"}, "filter-skip-second": {"same": b"2"}})
    )

    result = run(
        executable,
        "-x",
        "-f",
        str(archive),
        "s3://filter-skip-second/",
        env=s3_environment,
    )

    assert result.returncode == 0, result.stderr
    restored = client.get_object(Bucket="filter-skip-second", Key="same")
    assert restored["Body"].read() == b"2"
    with pytest.raises(botocore.exceptions.ClientError):
        client.head_bucket(Bucket="filter-skip-first")


def test_invalid_archive_is_rejected_before_target_bucket_is_created(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    archive = tmp_path / "invalid.tar"
    data = io.BytesIO()
    with tarfile.open(fileobj=data, mode="w", format=tarfile.PAX_FORMAT) as tar:
        entry = tarfile.TarInfo("missing-key")
        entry.size = 1
        tar.addfile(entry, io.BytesIO(b"x"))
    archive.write_bytes(data.getvalue())

    result = run(
        executable,
        "-x",
        "-f",
        str(archive),
        "s3://",
        env=s3_environment,
    )

    assert result.returncode != 0
    try:
        client.head_bucket(Bucket="missing-key")
    except botocore.exceptions.ClientError as error:
        assert error.response["ResponseMetadata"]["HTTPStatusCode"] == 404
    else:
        raise AssertionError("bucket was created for an invalid archive")


def test_extract_rejects_object_before_bucket_member(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    archive = tmp_path / "object-before-bucket.tar"
    data = io.BytesIO()
    with tarfile.open(fileobj=data, mode="w", format=tarfile.PAX_FORMAT) as tar:
        entry = tarfile.TarInfo("object-before-bucket/item")
        entry.size = 1
        entry.pax_headers = {
            "SCHILY.xattr.user.s3ar.bucket": "object-before-bucket",
            "SCHILY.xattr.user.s3ar.key": "item",
        }
        tar.addfile(entry, io.BytesIO(b"x"))
        bucket = tarfile.TarInfo("object-before-bucket")
        bucket.type = tarfile.DIRTYPE
        bucket.pax_headers = {
            "SCHILY.xattr.user.s3ar.bucket": "object-before-bucket"
        }
        tar.addfile(bucket)
    archive.write_bytes(data.getvalue())

    result = run(
        executable, "-x", "-f", str(archive), "s3://", env=s3_environment
    )

    assert result.returncode != 0
    assert "object precedes bucket archive member" in result.stderr
    with pytest.raises(botocore.exceptions.ClientError):
        client.head_bucket(Bucket="object-before-bucket")


def test_extract_prefix_initializes_bucket_from_bucket_member(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    archive = tmp_path / "prefix-bucket-member.tar"
    archive.write_bytes(
        archive_bytes(
            {
                "prefix-bucket-member": {
                    "selected/item": b"selected",
                    "outside": b"outside",
                },
                "prefix-bucket-skipped": {"selected/item": b"skipped"},
            }
        )
    )

    result = run(
        executable,
        "-xv",
        "-f",
        str(archive),
        "s3://prefix-bucket-member/selected/",
        env=s3_environment,
    )

    assert result.returncode == 0, result.stderr
    assert result.stderr.splitlines() == [
        "s3ar: info: prefix-bucket-member",
        "s3ar: info: prefix-bucket-member/selected/item -",
    ]
    restored = client.get_object(
        Bucket="prefix-bucket-member", Key="selected/item"
    )
    assert restored["Body"].read() == b"selected"
    with pytest.raises(botocore.exceptions.ClientError):
        client.head_bucket(Bucket="prefix-bucket-skipped")


@pytest.mark.parametrize(
    ("pax_key", "namespaced"),
    [
        ("SCHILY.xattr.user.s3ar.metadata.source", True),
        ("SCHILY.xattr.user.source", False),
    ],
    ids=("current", "legacy"),
)
def test_extract_rejects_metadata_with_http_line_breaks(
    pax_key,
    namespaced,
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    archive = tmp_path / "header-injection.tar"
    data = io.BytesIO()
    with tarfile.open(fileobj=data, mode="w", format=tarfile.PAX_FORMAT) as tar:
        bucket = tarfile.TarInfo("header-injection")
        bucket.type = tarfile.DIRTYPE
        bucket.pax_headers = {
            "SCHILY.xattr.user.s3ar.bucket": "header-injection"
        }
        tar.addfile(bucket)
        entry = tarfile.TarInfo("header-injection/object")
        entry.size = 1
        entry.pax_headers = {
            "SCHILY.xattr.user.s3ar.bucket": "header-injection",
            "SCHILY.xattr.user.s3ar.key": "object",
            pax_key: "archive\r\nx-amz-acl: public-read",
        }
        if namespaced:
            entry.pax_headers["SCHILY.xattr.user.s3ar.format"] = "1"
        tar.addfile(entry, io.BytesIO(b"x"))
    archive.write_bytes(data.getvalue())

    result = run(
        executable,
        "-x",
        "-f",
        str(archive),
        "s3://",
        env=s3_environment,
    )

    assert result.returncode != 0
    assert "invalid object metadata" in result.stderr
    client.head_bucket(Bucket="header-injection")


def test_extract_rejects_s3_tarfile(executable):
    result = run(
        executable,
        "-x",
        "-f",
        "s3://extract-archives/source.tar",
        "s3://remote-original/",
    )

    assert result.returncode == 2
    assert "TARFILE must be a local filesystem path or '-'" in result.stderr


def test_extract_without_f_reads_tar_from_stdin(
    executable, s3_server, s3_environment
):
    _endpoint, client = s3_server
    data = archive_bytes({"stdin-original": {"path/item": b"stdin data"}})

    result = subprocess.run(
        [str(executable), "-x", "s3://stdin-original/"],
        env=s3_environment,
        input=data,
        capture_output=True,
        check=False,
    )

    assert result.returncode == 0, result.stderr.decode()
    assert result.stderr == b""
    restored = client.get_object(Bucket="stdin-original", Key="path/item")
    assert restored["Body"].read() == b"stdin data"


def test_extract_with_f_dash_reads_tar_from_stdin(
    executable, s3_server, s3_environment
):
    _endpoint, client = s3_server
    data = archive_bytes({"stdin-f-dash": {"path/item": b"explicit stdin"}})

    result = subprocess.run(
        [str(executable), "-xf", "-", "s3://stdin-f-dash/"],
        env=s3_environment,
        input=data,
        capture_output=True,
        check=False,
    )

    assert result.returncode == 0, result.stderr.decode()
    assert result.stderr == b""
    restored = client.get_object(Bucket="stdin-f-dash", Key="path/item")
    assert restored["Body"].read() == b"explicit stdin"


def test_verbose_extract_lists_restored_objects(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    archive = tmp_path / "verbose-objects.tar"
    archive.write_bytes(
        archive_bytes(
            {
                "verbose-objects": {
                    "first": b"1",
                    "folder/second": b"2",
                }
            }
        )
    )

    result = run(
        executable,
        "-x",
        "-v",
        "-f",
        str(archive),
        "s3://verbose-objects/",
        env=s3_environment,
    )

    assert result.returncode == 0, result.stderr
    assert result.stdout == ""
    assert result.stderr == (
        "s3ar: info: verbose-objects\n"
        "s3ar: info: verbose-objects/first -\n"
        "s3ar: info: verbose-objects/folder/second -\n"
    )


def test_extract_prefix_restores_only_matching_keys(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    archive = tmp_path / "prefix.tar"
    archive.write_bytes(
        archive_bytes(
            {
                "prefix-restore": {
                    "album/photos/a.jpg": b"a",
                    "album/photos/b.jpg": b"b",
                    "album/photos-old/c.jpg": b"c",
                    "other/d.jpg": b"d",
                }
            }
        )
    )

    result = run(
        executable,
        "-x",
        "-f",
        str(archive),
        "s3://prefix-restore/album/photos/",
        env=s3_environment,
    )

    assert result.returncode == 0, result.stderr
    contents = client.list_objects_v2(Bucket="prefix-restore")["Contents"]
    assert {item["Key"] for item in contents} == {
        "album/photos/a.jpg",
        "album/photos/b.jpg",
    }


def test_extract_multiple_filters_deduplicate_overlaps(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    archive = tmp_path / "multiple-filters.tar"
    archive.write_bytes(
        archive_bytes(
            {
                "extract-multiple-first": {
                    "selected/a": b"a",
                    "selected/deeper/b": b"b",
                    "outside": b"outside",
                },
                "extract-multiple-second": {"selected/c": b"c"},
            }
        )
    )

    result = run(
        executable,
        "-xvf",
        str(archive),
        "s3://extract-multiple-first/selected/",
        "s3://extract-multiple-first/selected/deeper/",
        "s3://extract-multiple-second/selected/",
        env=s3_environment,
    )

    assert result.returncode == 0, result.stderr
    assert result.stdout == ""
    assert result.stderr.splitlines() == [
        "s3ar: info: extract-multiple-first",
        "s3ar: info: extract-multiple-first/selected/a -",
        "s3ar: info: extract-multiple-first/selected/deeper/b -",
        "s3ar: info: extract-multiple-second",
        "s3ar: info: extract-multiple-second/selected/c -",
    ]
    first = client.list_objects_v2(Bucket="extract-multiple-first")[
        "Contents"
    ]
    second = client.list_objects_v2(Bucket="extract-multiple-second")[
        "Contents"
    ]
    assert {item["Key"] for item in first} == {
        "selected/a",
        "selected/deeper/b",
    }
    assert {item["Key"] for item in second} == {"selected/c"}


def test_extract_without_filters_restores_all_members(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    archive = tmp_path / "all-members.tar"
    archive.write_bytes(
        archive_bytes(
            {
                "extract-all-first": {"a": b"a"},
                "extract-all-second": {"b": b"b"},
            }
        )
    )

    result = run(
        executable,
        "-xf",
        str(archive),
        env=s3_environment,
    )

    assert result.returncode == 0, result.stderr
    first = client.get_object(Bucket="extract-all-first", Key="a")
    second = client.get_object(Bucket="extract-all-second", Key="b")
    assert first["Body"].read() == b"a"
    assert second["Body"].read() == b"b"
