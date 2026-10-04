import hashlib
import io
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import signal
import shutil
import stat
import subprocess
import tarfile
import threading
import time
import urllib.parse

import pytest

from fault_server import FaultServer, ResponseStep


def run(executable, *arguments, cwd=None, env=None, umask=-1):
    return subprocess.run(
        [str(executable), *arguments],
        cwd=cwd,
        env=env,
        umask=umask,
        text=True,
        capture_output=True,
        check=False,
    )


def decompress_zstd(path):
    if shutil.which("zstd") is None:
        pytest.skip("zstd command is required for interoperability tests")
    result = subprocess.run(
        ["zstd", "-q", "-d", "-c", str(path)],
        capture_output=True,
        check=False,
    )
    assert result.returncode == 0, result.stderr.decode()
    return result.stdout


def members_by_name(path):
    with tarfile.open(path, "r:") as archive:
        return {member.name: member for member in archive.getmembers()}


def object_member(bucket, key):
    if key.endswith("/"):
        key = f"{key[:-1]}%2F"
    return f"{bucket}/{key}"


def raw_tar_entries(path):
    data = path.read_bytes()
    entries = []
    offset = 0
    while data[offset : offset + 512] != bytes(512):
        header = data[offset : offset + 512]
        assert len(header) == 512
        stored_checksum = int(header[148:156].rstrip(b"\0 ") or b"0", 8)
        checksum_header = bytearray(header)
        checksum_header[148:156] = b" " * 8
        assert sum(checksum_header) == stored_checksum
        size = int(header[124:136].rstrip(b"\0 ") or b"0", 8)
        name = header[:100].split(b"\0", 1)[0]
        prefix = header[345:500].split(b"\0", 1)[0]
        if prefix:
            name = prefix + b"/" + name
        payload = data[offset + 512 : offset + 512 + size]
        entries.append((name, header[156:157], payload))
        offset += 512 + ((size + 511) // 512) * 512
    assert data[offset : offset + 1024] == bytes(1024)
    return entries


def test_create_exact_object_avoids_redundant_head_requests(
    executable, tmp_path
):
    acl = (
        b"<AccessControlPolicy><Owner><ID>owner</ID></Owner>"
        b"<AccessControlList/></AccessControlPolicy>"
    )

    class RequestCountingHandler(BaseHTTPRequestHandler):
        requests = []

        def log_message(self, _format, *_arguments):
            pass

        def reply(self, status, body=b"", headers=()):
            self.send_response(status)
            self.send_header("Content-Length", str(len(body)))
            for name, value in headers:
                self.send_header(name, value)
            self.end_headers()
            if body:
                self.wfile.write(body)

        def do_HEAD(self):
            type(self).requests.append(("HEAD", self.path))
            self.reply(500)

        def do_GET(self):
            type(self).requests.append(("GET", self.path))
            parsed = urllib.parse.urlsplit(self.path)
            query = urllib.parse.parse_qs(parsed.query)
            if parsed.path == "/request-create" and parsed.query == "acl":
                self.reply(200, acl)
            elif parsed.path == "/request-create/object":
                self.reply(200, b"data", (("ETag", '"object"'),))
            elif (
                parsed.path == "/request-create"
                and query.get("list-type") == ["2"]
                and query.get("prefix") == ["object"]
            ):
                self.reply(
                    200,
                    b"<ListBucketResult><EncodingType>url</EncodingType>"
                    b"<IsTruncated>false</IsTruncated>"
                    b"<Contents><Key>object</Key><Size>4</Size>"
                    b"<LastModified>2026-09-03T12:00:00Z</LastModified>"
                    b"<ETag>\"object\"</ETag></Contents></ListBucketResult>",
                )
            else:
                self.reply(404)

    server = ThreadingHTTPServer(("127.0.0.1", 0), RequestCountingHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    target = tmp_path / "request-count.tar"
    environment = os.environ.copy()
    environment.update(
        {
            "S3AR_ENDPOINT": f"http://127.0.0.1:{server.server_port}",
            "S3AR_URI_STYLE": "path",
            "S3AR_REGION": "us-east-1",
            "S3AR_ACCESS_KEY": "test-access",
            "S3AR_SECRET_KEY": "test-secret",
        }
    )
    try:
        result = run(
            executable,
            "-cf",
            str(target),
            "s3://request-create/object",
            env=environment,
        )
    finally:
        server.shutdown()
        server.server_close()
        thread.join()

    assert result.returncode == 0, result.stderr
    assert [method for method, _path in RequestCountingHandler.requests] == [
        "GET",
        "GET",
        "GET",
    ]
    with tarfile.open(target, "r:") as archive:
        assert archive.extractfile(object_member("request-create", "object")).read() == b"data"


def test_create_skips_object_deleted_between_list_and_get(executable, tmp_path):
    acl = (
        b"<AccessControlPolicy><Owner><ID>owner</ID></Owner>"
        b"<AccessControlList/></AccessControlPolicy>"
    )
    listing = (
        b"<ListBucketResult><EncodingType>url</EncodingType>"
        b"<IsTruncated>false</IsTruncated>"
        b"<Contents><Key>deleted</Key><LastModified>2026-09-03T12:00:00Z"
        b"</LastModified><ETag>\"deleted\"</ETag><Size>7</Size></Contents>"
        b"<Contents><Key>retained</Key><LastModified>2026-09-03T12:00:01Z"
        b"</LastModified><ETag>\"retained\"</ETag><Size>4</Size></Contents>"
        b"</ListBucketResult>"
    )

    class ConcurrentDeleteHandler(BaseHTTPRequestHandler):
        def log_message(self, _format, *_arguments):
            pass

        def reply(self, status, body=b"", headers=()):
            self.send_response(status)
            self.send_header("Content-Length", str(len(body)))
            for name, value in headers:
                self.send_header(name, value)
            self.end_headers()
            if body:
                self.wfile.write(body)

        def do_GET(self):
            parsed = urllib.parse.urlsplit(self.path)
            query = urllib.parse.parse_qs(parsed.query)
            if parsed.path == "/concurrent-delete" and parsed.query == "acl":
                self.reply(200, acl)
            elif (
                parsed.path == "/concurrent-delete"
                and query.get("list-type") == ["2"]
            ):
                self.reply(200, listing)
            elif parsed.path == "/concurrent-delete/deleted":
                self.reply(404)
            elif parsed.path == "/concurrent-delete/retained":
                self.reply(200, b"data", (("ETag", '"retained"'),))
            else:
                self.reply(404)

    server = ThreadingHTTPServer(("127.0.0.1", 0), ConcurrentDeleteHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    target = tmp_path / "concurrent-delete.tar"
    environment = os.environ.copy()
    environment.update(
        {
            "S3AR_ENDPOINT": f"http://127.0.0.1:{server.server_port}",
            "S3AR_URI_STYLE": "path",
            "S3AR_REGION": "us-east-1",
            "S3AR_ACCESS_KEY": "test-access",
            "S3AR_SECRET_KEY": "test-secret",
        }
    )
    try:
        result = run(
            executable,
            "-cf",
            str(target),
            "s3://concurrent-delete/",
            env=environment,
        )
    finally:
        server.shutdown()
        server.server_close()
        thread.join()

    assert result.returncode == 0, result.stderr
    assert (
        "s3ar: warning: object disappeared during backup "
        "concurrent-delete/deleted\n"
    ) in result.stderr
    with tarfile.open(target, "r:") as archive:
        assert archive.getnames() == [
            "concurrent-delete",
            object_member("concurrent-delete", "retained"),
        ]
        assert archive.extractfile(
            object_member("concurrent-delete", "retained")
        ).read() == b"data"


def test_create_archives_object_after_interrupted_get(
    executable, s3_environment, tmp_path
):
    bucket = "retry-create"
    key = "object"
    data = bytes(range(239)) * 307
    cutoff = 19873
    etag = '"retry-create-etag"'
    modified = "Thu, 03 Sep 2026 12:00:00 GMT"
    object_headers = (
        ("ETag", etag),
        ("Last-Modified", modified),
        ("Content-Type", "application/octet-stream"),
        ("Cache-Control", "no-cache"),
        ("x-amz-meta-source", "retry-test"),
    )
    resumed_request_headers = (
        ("Range", f"bytes={cutoff}-"),
        ("If-Match", etag),
    )
    acl = (
        b"<AccessControlPolicy><Owner><ID>owner</ID></Owner>"
        b"<AccessControlList/></AccessControlPolicy>"
    )
    listing = (
        b"<ListBucketResult><EncodingType>url</EncodingType>"
        b"<IsTruncated>false</IsTruncated><Contents><Key>object</Key>"
        b"<LastModified>2026-09-03T12:00:00Z</LastModified>"
        b"<ETag>\"retry-create-etag\"</ETag><Size>73373</Size>"
        b"</Contents></ListBucketResult>"
    )
    slow_down = b"<Error><Code>SlowDown</Code><Message>Retry later.</Message></Error>"
    steps = [
        ResponseStep("GET", f"/{bucket}?acl", 200, acl),
        ResponseStep(
            "GET",
            f"/{bucket}?list-type=2&max-keys=1000&encoding-type=url",
            200,
            listing,
        ),
        ResponseStep(
            "GET",
            f"/{bucket}/{key}",
            200,
            data,
            headers=object_headers,
            absent_headers=("Range", "If-Match"),
            disconnect_after=cutoff,
        ),
        ResponseStep(
            "GET",
            f"/{bucket}/{key}",
            503,
            slow_down,
            headers=(("Retry-After", "0"),),
            expected_headers=resumed_request_headers,
        ),
        ResponseStep(
            "GET",
            f"/{bucket}/{key}",
            206,
            data[cutoff:],
            headers=object_headers
            + (("Content-Range", f"bytes {cutoff}-{len(data) - 1}/{len(data)}"),),
            expected_headers=resumed_request_headers,
        ),
    ]
    target = tmp_path / "retry-create.tar"

    with FaultServer(steps) as server:
        environment = s3_environment.copy()
        environment["S3AR_ENDPOINT"] = server.endpoint
        result = run(
            executable,
        "--hash",
            "-cf",
            str(target),
            f"s3://{bucket}/",
            env=environment,
        )

    assert result.returncode == 0, result.stderr
    assert result.stdout == ""
    assert result.stderr == ""
    members = members_by_name(target)
    item_name = object_member(bucket, key)
    assert set(members) == {bucket, item_name}
    item = members[item_name]
    assert item.size == len(data)
    assert item.pax_headers["SCHILY.xattr.user.s3ar.hash"] == (
        "sha512:" + hashlib.sha512(data).hexdigest()
    )
    assert item.pax_headers["SCHILY.xattr.user.s3ar.etag"] == etag
    assert (
        item.pax_headers["SCHILY.xattr.user.s3ar.metadata.source"]
        == "retry-test"
    )
    with tarfile.open(target, "r:") as archive:
        assert archive.extractfile(item_name).read() == data
    assert list(tmp_path.glob("retry-create.tar.tmp.*")) == []


def test_failed_create_after_interrupted_get_preserves_archive(
    executable, s3_environment, tmp_path
):
    bucket = "changed-create"
    key = "object"
    data = bytes(range(197)) * 181
    cutoff = 9137
    etag = '"original-version"'
    acl = (
        b"<AccessControlPolicy><Owner><ID>owner</ID></Owner>"
        b"<AccessControlList/></AccessControlPolicy>"
    )
    listing = (
        "<ListBucketResult><EncodingType>url</EncodingType>"
        "<IsTruncated>false</IsTruncated><Contents><Key>object</Key>"
        "<LastModified>2026-09-03T12:00:00Z</LastModified>"
        f"<ETag>{etag}</ETag><Size>{len(data)}</Size>"
        "</Contents></ListBucketResult>"
    ).encode()
    changed = (
        b"<Error><Code>PreconditionFailed</Code>"
        b"<Message>The object changed.</Message></Error>"
    )
    steps = [
        ResponseStep("GET", f"/{bucket}?acl", 200, acl),
        ResponseStep(
            "GET",
            f"/{bucket}?list-type=2&max-keys=1000&encoding-type=url",
            200,
            listing,
        ),
        ResponseStep(
            "GET",
            f"/{bucket}/{key}",
            200,
            data,
            headers=(("ETag", etag),),
            absent_headers=("Range", "If-Match"),
            disconnect_after=cutoff,
        ),
        ResponseStep(
            "GET",
            f"/{bucket}/{key}",
            412,
            changed,
            expected_headers=(
                ("Range", f"bytes={cutoff}-"),
                ("If-Match", etag),
            ),
        ),
    ]
    target = tmp_path / "changed-create.tar"
    original = b"previous valid archive"
    target.write_bytes(original)

    with FaultServer(steps) as server:
        environment = s3_environment.copy()
        environment["S3AR_ENDPOINT"] = server.endpoint
        result = run(
            executable,
            "-cf",
            str(target),
            f"s3://{bucket}/",
            env=environment,
        )

    assert result.returncode == 2
    assert result.stdout == ""
    assert "object changed during download" in result.stderr
    assert "(S3 PreconditionFailed)" in result.stderr
    assert "after 2 attempts" in result.stderr
    assert target.read_bytes() == original
    assert list(tmp_path.glob("changed-create.tar.tmp.*")) == []


def test_create_single_bucket_with_full_path_and_metadata(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="create-single")
    put = client.put_object(
        Bucket="create-single",
        Key="folder/object.txt",
        Body=b"object data",
        Metadata={"source": "create-test"},
    )
    target = tmp_path / "single.tar"

    result = run(
        executable,
        "-c",
        "-f",
        str(target),
        "s3://create-single",
        env=s3_environment,
    )

    assert result.returncode == 0, result.stderr
    members = members_by_name(target)
    object_name = object_member("create-single", "folder/object.txt")
    assert set(members) == {"create-single", object_name}
    assert not any(
        name.startswith("LIBARCHIVE.xattr.")
        for member in members.values()
        for name in member.pax_headers
    )
    item = members[object_name]
    bucket = members["create-single"]
    assert bucket.isdir()
    assert (
        bucket.pax_headers["SCHILY.xattr.user.s3ar.bucket-acl"]
        == "private"
    )
    assert bucket.pax_headers["SCHILY.xattr.user.s3ar.format"] == "1"
    assert (
        bucket.pax_headers["SCHILY.xattr.user.s3ar.bucket"]
        == "create-single"
    )
    assert "SCHILY.xattr.user.s3ar.key" not in bucket.pax_headers
    assert item.size == len(b"object data")
    assert item.pax_headers["SCHILY.xattr.user.s3ar.bucket"] == "create-single"
    assert item.pax_headers["SCHILY.xattr.user.s3ar.key"] == "folder/object.txt"
    assert item.pax_headers["SCHILY.xattr.user.s3ar.etag"] == put["ETag"]
    assert "SCHILY.xattr.bucket" not in item.pax_headers
    assert (
        item.pax_headers["SCHILY.xattr.user.s3ar.metadata.source"]
        == "create-test"
    )
    assert item.pax_headers["SCHILY.xattr.user.s3ar.format"] == "1"
    assert "SCHILY.xattr.s3ar.bucket-acl" not in bucket.pax_headers
    assert "SCHILY.xattr.user.source" not in item.pax_headers

    raw = raw_tar_entries(target)
    object_pax = next(
        payload for _name, kind, payload in raw
        if kind == b"x"
        and b"SCHILY.xattr.user.s3ar.metadata.source=" in payload
    )
    assert b"LIBARCHIVE.xattr." not in object_pax
    assert b"SCHILY.xattr.bucket=" not in object_pax
    assert b"SCHILY.xattr.user.s3ar.format=1\n" in object_pax
    assert b"SCHILY.xattr.user.s3ar.bucket=create-single\n" in object_pax
    assert b"SCHILY.xattr.user.s3ar.key=folder/object.txt\n" in object_pax
    assert b"SCHILY.xattr.user.s3ar.etag=" in object_pax
    assert b"SCHILY.xattr.user.s3ar.metadata.source=create-test\n" in object_pax
    object_header = next(
        name
        for name, kind, _payload in raw
        if kind == b"0" and name == object_name.encode()
    )
    assert object_header == object_name.encode()


def test_create_bucket_with_control_character_in_key(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    bucket = "create-control-key"
    control_key = "ctrl\x01name.txt"
    client.create_bucket(Bucket=bucket)
    client.put_object(Bucket=bucket, Key="plain.txt", Body=b"plain")
    client.put_object(Bucket=bucket, Key=control_key, Body=b"control")
    target = tmp_path / "control-key.tar"

    result = run(
        executable,
        "-c",
        "-f",
        str(target),
        f"s3://{bucket}",
        env=s3_environment,
    )

    assert result.returncode == 0, result.stderr
    with tarfile.open(target, "r:") as archive:
        assert set(archive.getnames()) == {
            bucket,
            object_member(bucket, "plain.txt"),
            object_member(bucket, control_key),
        }
        assert archive.extractfile(object_member(bucket, "plain.txt")).read() == b"plain"
        assert (
            archive.extractfile(object_member(bucket, control_key)).read()
            == b"control"
        )


def test_create_keeps_raw_pathname_and_stores_authoritative_identity(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    bucket = "create-path-encoding"
    objects = {
        "a": b"plain",
        "a/b": b"child",
        "a/": b"marker",
        "/a": b"leading",
        "a//b": b"empty",
        "a/./b": b"dot",
        "a/../b": b"dotdot",
        "a%2F": b"literal-marker",
        "a%2Fb": b"percent-slash",
        "a%00": b"literal-terminator",
    }
    client.create_bucket(Bucket=bucket)
    for key, body in objects.items():
        client.put_object(Bucket=bucket, Key=key, Body=body)
    target = tmp_path / "path-encoding.tar"

    created = run(
        executable, "-cf", str(target), f"s3://{bucket}", env=s3_environment
    )
    assert created.returncode == 0, created.stderr

    expected_names = [object_member(bucket, key) for key in objects]
    with tarfile.open(target, "r:") as archive:
        members = archive.getmembers()
        assert members[0].name == bucket
        assert sorted(member.name for member in members[1:]) == sorted(
            expected_names
        )
        assert object_member(bucket, "a") == f"{bucket}/a"
        assert object_member(bucket, "a/b") == f"{bucket}/a/b"
        assert object_member(bucket, "a/") == f"{bucket}/a%2F"
        assert object_member(bucket, "a//b") == f"{bucket}/a//b"
        identities = {}
        for member in members[1:]:
            key = urllib.parse.unquote(
                member.pax_headers["SCHILY.xattr.user.s3ar.key"]
            )
            identities[key] = member
            assert member.name == object_member(bucket, key)
            assert member.pax_headers[
                "SCHILY.xattr.user.s3ar.bucket"
            ] == urllib.parse.quote(bucket, safe="/-._~")
        assert set(identities) == set(objects)

    for key in objects:
        client.delete_object(Bucket=bucket, Key=key)
    extracted = run(
        executable, "-xf", str(target), f"s3://{bucket}", env=s3_environment
    )
    assert extracted.returncode == 0, extracted.stderr
    restored = client.list_objects_v2(Bucket=bucket).get("Contents", [])
    assert {item["Key"] for item in restored} == set(objects)
    for key, body in objects.items():
        assert client.get_object(Bucket=bucket, Key=key)["Body"].read() == body


def test_created_metadata_can_be_restored_as_filesystem_xattrs(
    executable, s3_server, s3_environment, tmp_path
):
    if not hasattr(os, "getxattr") or shutil.which("tar") is None:
        pytest.skip("filesystem xattrs and GNU tar are required")
    _endpoint, client = s3_server
    client.create_bucket(Bucket="create-filesystem-xattrs")
    put = client.put_object(
        Bucket="create-filesystem-xattrs",
        Key="object",
        Body=b"data",
        Metadata={"source": "filesystem-test"},
    )
    target = tmp_path / "filesystem-xattrs.tar"

    created = run(
        executable,
        "-c",
        "-f",
        str(target),
        "s3://create-filesystem-xattrs",
        env=s3_environment,
    )
    assert created.returncode == 0, created.stderr

    destination = tmp_path / "extracted"
    destination.mkdir()
    extracted = subprocess.run(
        [
            "tar",
            "--xattrs",
            "--xattrs-include=user.s3ar.*",
            "-xf",
            str(target),
            "-C",
            str(destination),
        ],
        text=True,
        capture_output=True,
        check=False,
    )
    assert extracted.returncode == 0, extracted.stderr
    bucket = destination / "create-filesystem-xattrs"
    assert os.getxattr(bucket, "user.s3ar.bucket-acl") == b"private"
    assert os.getxattr(bucket, "user.s3ar.format") == b"1"
    assert (
        os.getxattr(bucket, "user.s3ar.bucket")
        == b"create-filesystem-xattrs"
    )
    assert (
        os.getxattr(bucket / "object", "user.s3ar.metadata.source")
        == b"filesystem-test"
    )
    assert os.getxattr(bucket / "object", "user.s3ar.format") == b"1"
    assert (
        os.getxattr(bucket / "object", "user.s3ar.bucket")
        == b"create-filesystem-xattrs"
    )
    assert os.getxattr(bucket / "object", "user.s3ar.key") == b"object"

    steps = [
        ResponseStep(
            "GET", "/create-filesystem-xattrs/object", 304,
            expected_headers=(("If-None-Match", put["ETag"]),),
        )
    ]
    with FaultServer(steps) as server:
        refresh_environment = s3_environment.copy()
        refresh_environment["S3AR_ENDPOINT"] = server.endpoint
        result = subprocess.run(
            [str(executable.parent / "s3ar-get"), "-f", str(bucket / "object"),
             "s3://create-filesystem-xattrs/object"],
            capture_output=True, env=refresh_environment, timeout=10,
        )
    assert result.returncode == 0, result.stderr.decode()
    assert (bucket / "object").read_bytes() == b"data"


def test_archive_and_individual_get_have_identical_bodies(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    bucket = "compare-archive-and-get"
    objects = {
        "first": b"first body\n",
        "folder/second": bytes(range(256)) * 17,
        "empty": b"",
    }
    client.create_bucket(Bucket=bucket)
    try:
        for key, body in objects.items():
            client.put_object(Bucket=bucket, Key=key, Body=body)

        archive_path = tmp_path / "bucket.tar"
        created = run(
            executable, "-c", "-f", str(archive_path), f"s3://{bucket}",
            env=s3_environment,
        )
        assert created.returncode == 0, created.stderr

        with tarfile.open(archive_path, "r:") as archive:
            for key in objects:
                member = archive.extractfile(f"{bucket}/{key}")
                assert member is not None
                archived_body = member.read()
                destination = tmp_path / "individual" / key
                destination.parent.mkdir(parents=True, exist_ok=True)
                result = subprocess.run(
                    [str(executable.parent / "s3ar-get"), "-f", str(destination),
                     f"s3://{bucket}/{key}"],
                    capture_output=True, env=s3_environment, timeout=30,
                )
                assert result.returncode == 0, result.stderr.decode()
                assert destination.read_bytes() == archived_body == objects[key]
    finally:
        for key in objects:
            client.delete_object(Bucket=bucket, Key=key)
        client.delete_bucket(Bucket=bucket)


def test_create_zstd_archive(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="create-zstd")
    client.put_object(Bucket="create-zstd", Key="object", Body=b"zstd data")
    target = tmp_path / "backup.tar.zst"

    result = run(
        executable,
        "-c",
        "--zstd",
        "-f",
        str(target),
        "s3://create-zstd/",
        env=s3_environment,
    )

    assert result.returncode == 0, result.stderr
    assert target.read_bytes().startswith(b"\x28\xb5\x2f\xfd")
    with tarfile.open(
        fileobj=io.BytesIO(decompress_zstd(target)), mode="r:"
    ) as archive:
        assert archive.getnames() == [
            "create-zstd",
            object_member("create-zstd", "object"),
        ]


def test_create_zstd_archive_on_stdout(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="create-zstd-stdout")
    client.put_object(
        Bucket="create-zstd-stdout", Key="object", Body=b"zstd stdout data"
    )

    result = subprocess.run(
        [str(executable), "-c", "--zstd", "s3://create-zstd-stdout/"],
        env=s3_environment,
        capture_output=True,
        check=False,
    )

    assert result.returncode == 0, result.stderr.decode()
    assert result.stderr == b""
    target = tmp_path / "stdout.tar.zst"
    target.write_bytes(result.stdout)
    with tarfile.open(
        fileobj=io.BytesIO(decompress_zstd(target)), mode="r:"
    ) as archive:
        assert archive.extractfile(
            object_member("create-zstd-stdout", "object")
        ).read() == b"zstd stdout data"


def test_create_all_buckets_uses_full_paths_and_includes_empty_buckets(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="create-empty")
    client.create_bucket(Bucket="create-full")
    client.put_object(Bucket="create-full", Key="item", Body=b"x")
    target = tmp_path / "all.tar"

    result = run(
        executable, "-c", "-f", str(target), "s3://", env=s3_environment
    )

    assert result.returncode == 0, result.stderr
    members = members_by_name(target)
    assert members["create-empty"].isdir()
    item_name = object_member("create-full", "item")
    assert item_name in members
    item = members[item_name]
    assert "SCHILY.xattr.bucket" not in item.pax_headers


@pytest.mark.parametrize(
    ("process_umask", "expected_mode"),
    [(0o000, 0o666), (0o022, 0o644), (0o077, 0o600)],
    ids=("umask-000", "umask-022", "umask-077"),
)
def test_create_new_archive_respects_umask(
    executable,
    s3_server,
    s3_environment,
    tmp_path,
    process_umask,
    expected_mode,
):
    _endpoint, client = s3_server
    bucket = f"create-mode-{process_umask:03o}"
    client.create_bucket(Bucket=bucket)
    target = tmp_path / "mode.tar"

    result = run(
        executable,
        "-c",
        "-f",
        str(target),
        f"s3://{bucket}",
        env=s3_environment,
        umask=process_umask,
    )

    assert result.returncode == 0, result.stderr
    assert stat.S_IMODE(target.stat().st_mode) == expected_mode


def test_create_to_device_does_not_require_fsync(
    executable, s3_server, s3_environment
):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="create-device")
    client.put_object(Bucket="create-device", Key="object", Body=b"data")

    result = run(
        executable,
        "-c",
        "-f",
        os.devnull,
        "s3://create-device/",
        env=s3_environment,
    )

    assert result.returncode == 0, result.stderr


def test_create_to_fifo_does_not_require_fsync(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="create-fifo")
    client.put_object(Bucket="create-fifo", Key="object", Body=b"data")
    target = tmp_path / "archive.fifo"
    os.mkfifo(target)
    received = bytearray()

    def drain_fifo():
        with target.open("rb") as stream:
            while data := stream.read(65536):
                received.extend(data)

    reader = threading.Thread(target=drain_fifo)
    reader.start()
    result = run(
        executable,
        "-c",
        "-f",
        str(target),
        "s3://create-fifo/",
        env=s3_environment,
    )
    reader.join(timeout=5)

    assert result.returncode == 0, result.stderr
    assert not reader.is_alive()
    assert received


def test_create_bucket_prefix_keeps_full_key(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="create-prefix")
    client.put_object(Bucket="create-prefix", Key="album/photos/a.jpg", Body=b"a")
    client.put_object(Bucket="create-prefix", Key="album/photos/b.jpg", Body=b"b")
    client.put_object(Bucket="create-prefix", Key="album/photos-old/c.jpg", Body=b"c")
    client.put_object(Bucket="create-prefix", Key="other/d.jpg", Body=b"d")
    target = tmp_path / "prefix.tar"

    result = run(
        executable,
        "-c",
        "-f",
        str(target),
        "s3://create-prefix/album/photos/",
        env=s3_environment,
    )

    assert result.returncode == 0, result.stderr
    members = members_by_name(target)
    assert set(members) == {
        "create-prefix",
        object_member("create-prefix", "album/photos/a.jpg"),
        object_member("create-prefix", "album/photos/b.jpg"),
    }


def test_create_multiple_sources_preserves_overlapping_prefixes(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="create-multiple-first")
    client.create_bucket(Bucket="create-multiple-second")
    client.put_object(
        Bucket="create-multiple-first", Key="shared/a", Body=b"a"
    )
    client.put_object(
        Bucket="create-multiple-first", Key="shared/deeper/b", Body=b"b"
    )
    client.put_object(
        Bucket="create-multiple-first", Key="outside", Body=b"outside"
    )
    client.put_object(Bucket="create-multiple-second", Key="c", Body=b"c")
    target = tmp_path / "multiple.tar"

    result = run(
        executable,
        "-cf",
        str(target),
        "s3://create-multiple-first/shared/",
        "s3://create-multiple-first/shared/deeper/",
        "s3://create-multiple-second/",
        env=s3_environment,
    )

    assert result.returncode == 0, result.stderr
    with tarfile.open(target, "r:") as archive:
        names = archive.getnames()
        assert names.count("create-multiple-first") == 2
        assert names.count("create-multiple-second") == 1
        assert names.count(object_member("create-multiple-first", "shared/a")) == 1
        # As with tar, an entry selected by multiple operands is archived
        # once for each matching operand.
        assert names.count(object_member("create-multiple-first", "shared/deeper/b")) == 2
        assert names.count(object_member("create-multiple-second", "c")) == 1
        assert object_member("create-multiple-first", "outside") not in names


def test_create_overwrites_existing_tarfile(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="create-overwrite")
    client.put_object(
        Bucket="create-overwrite", Key="new-object", Body=b"new archive data"
    )
    target = tmp_path / "existing.tar"
    target.write_bytes(b"old contents that must be truncated")
    target.chmod(0o640)

    result = run(
        executable,
        "-c",
        "-f",
        str(target),
        "s3://create-overwrite/",
        env=s3_environment,
    )

    assert result.returncode == 0, result.stderr
    assert stat.S_IMODE(target.stat().st_mode) == 0o640
    with tarfile.open(target, "r:") as archive:
        assert archive.getnames() == [
            "create-overwrite",
            object_member("create-overwrite", "new-object"),
        ]
        assert (
            archive.extractfile(object_member("create-overwrite", "new-object")).read()
            == b"new archive data"
        )


def test_failed_create_preserves_existing_archive(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    target = tmp_path / "partial.tar"
    target.write_bytes(b"previous valid archive")
    target.chmod(0o640)

    result = run(
        executable,
        "-c",
        "-f",
        str(target),
        "s3://create-partial-missing/",
        env=s3_environment,
    )

    assert result.returncode != 0
    assert "bucket does not exist create-partial-missing" in result.stderr
    assert target.read_bytes() == b"previous valid archive"
    assert stat.S_IMODE(target.stat().st_mode) == 0o640
    assert list(tmp_path.glob("partial.tar.tmp.*")) == []


def test_failed_create_does_not_install_new_archive(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    target = tmp_path / "partial.tar"

    result = run(
        executable,
        "-c",
        "-f",
        str(target),
        "s3://create-partial-missing/",
        env=s3_environment,
    )

    assert result.returncode != 0
    assert not target.exists()
    assert list(tmp_path.glob("partial.tar.tmp.*")) == []


@pytest.mark.parametrize("interrupt_signal", [signal.SIGINT, signal.SIGTERM])
def test_interrupted_create_preserves_existing_archive(
    executable, s3_environment, tmp_path, interrupt_signal
):
    acl = (
        b"<AccessControlPolicy><Owner><ID>owner</ID></Owner>"
        b"<AccessControlList/></AccessControlPolicy>"
    )
    object_started = threading.Event()

    class SlowObjectHandler(BaseHTTPRequestHandler):
        def log_message(self, _format, *_arguments):
            pass

        def do_GET(self):
            parsed = urllib.parse.urlsplit(self.path)
            if parsed.path == "/interrupt-create":
                body = acl if parsed.query == "acl" else (
                    b"<ListBucketResult><EncodingType>url</EncodingType>"
                    b"<IsTruncated>false</IsTruncated>"
                    b"<Contents><Key>object</Key><Size>0</Size>"
                    b"<LastModified>2026-09-03T12:00:00Z</LastModified>"
                    b"<ETag>\"object\"</ETag></Contents></ListBucketResult>"
                )
                self.send_response(200)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            if parsed.path != "/interrupt-create/object":
                self.send_response(404)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            chunk = b"x" * 65536
            self.send_response(200)
            self.send_header("Content-Length", str(len(chunk) * 1024))
            self.send_header("ETag", '"object"')
            self.end_headers()
            object_started.set()
            try:
                for _ in range(1024):
                    self.wfile.write(chunk)
                    self.wfile.flush()
                    time.sleep(0.01)
            except (BrokenPipeError, ConnectionResetError):
                pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), SlowObjectHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    environment = s3_environment.copy()
    environment["S3AR_ENDPOINT"] = f"http://127.0.0.1:{server.server_port}"
    target = tmp_path / "interrupted.tar"
    target.write_bytes(b"previous valid archive")
    process = subprocess.Popen(
        [
            str(executable),
            "-cf",
            str(target),
            "s3://interrupt-create/object",
        ],
        env=environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    try:
        assert object_started.wait(timeout=5)
        process.send_signal(interrupt_signal)
        _stdout, stderr = process.communicate(timeout=10)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        server.shutdown()
        server.server_close()
        thread.join()

    assert process.returncode == 2
    assert "interrupted" in stderr
    assert target.read_bytes() == b"previous valid archive"
    assert list(tmp_path.glob("interrupted.tar.tmp.*")) == []


@pytest.mark.parametrize("interrupt_signal", [signal.SIGINT, signal.SIGTERM])
def test_interrupted_create_during_get_retry_delay_preserves_archive(
    executable, s3_environment, tmp_path, interrupt_signal
):
    acl = (
        b"<AccessControlPolicy><Owner><ID>owner</ID></Owner>"
        b"<AccessControlList/></AccessControlPolicy>"
    )
    retry_started = threading.Event()

    class RetryDelayHandler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, _format, *_arguments):
            pass

        def do_GET(self):
            parsed = urllib.parse.urlsplit(self.path)
            if parsed.path == "/interrupt-retry":
                body = acl if parsed.query == "acl" else (
                    b"<ListBucketResult><EncodingType>url</EncodingType>"
                    b"<IsTruncated>false</IsTruncated>"
                    b"<Contents><Key>object</Key><Size>0</Size>"
                    b"<LastModified>2026-09-03T12:00:00Z</LastModified>"
                    b"<ETag>\"object\"</ETag></Contents></ListBucketResult>"
                )
                self.send_response(200)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            if parsed.path != "/interrupt-retry/object":
                self.send_response(404)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            if not retry_started.is_set():
                body = b"<Error><Code>SlowDown</Code></Error>"
                self.send_response(503)
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Retry-After", "30")
                self.end_headers()
                self.wfile.write(body)
                self.wfile.flush()
                retry_started.set()
                return
            self.send_response(200)
            self.send_header("Content-Length", "0")
            self.send_header("ETag", '"object"')
            self.end_headers()

    server = ThreadingHTTPServer(("127.0.0.1", 0), RetryDelayHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    environment = s3_environment.copy()
    environment["S3AR_ENDPOINT"] = f"http://127.0.0.1:{server.server_port}"
    target = tmp_path / "interrupted-retry.tar"
    target.write_bytes(b"previous valid archive")
    process = subprocess.Popen(
        [
            str(executable),
            "-cf",
            str(target),
            "s3://interrupt-retry/object",
        ],
        env=environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    try:
        assert retry_started.wait(timeout=5)
        time.sleep(0.1)
        process.send_signal(interrupt_signal)
        _stdout, stderr = process.communicate(timeout=5)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        server.shutdown()
        server.server_close()
        thread.join()

    assert process.returncode == 2
    assert "interrupted" in stderr
    assert target.read_bytes() == b"previous valid archive"
    assert list(tmp_path.glob("interrupted-retry.tar.tmp.*")) == []


def test_create_accepts_explicit_dot_component_key(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="create-unsafe-explicit")
    client.put_object(
        Bucket="create-unsafe-explicit", Key="folder/../unsafe", Body=b"unsafe"
    )

    result = run(
        executable,
        "-c",
        "-f",
        str(tmp_path / "unsafe.tar"),
        "s3://create-unsafe-explicit/folder/../unsafe",
        env=s3_environment,
    )

    assert result.returncode == 0, result.stderr
    with tarfile.open(tmp_path / "unsafe.tar", "r:") as archive:
        assert object_member(
            "create-unsafe-explicit", "folder/../unsafe"
        ) in archive.getnames()


def test_create_without_f_writes_tar_to_stdout(
    executable, s3_server, s3_environment
):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="create-stdout")
    client.put_object(Bucket="create-stdout", Key="item", Body=b"stdout data")

    result = subprocess.run(
        [str(executable), "-c", "s3://create-stdout/"],
        env=s3_environment,
        capture_output=True,
        check=False,
    )

    assert result.returncode == 0, result.stderr.decode()
    assert result.stderr == b""
    with tarfile.open(fileobj=io.BytesIO(result.stdout), mode="r:") as archive:
        members = {member.name: member for member in archive.getmembers()}
        item_name = object_member("create-stdout", "item")
        assert set(members) == {"create-stdout", item_name}
        assert "SCHILY.xattr.bucket" not in members[item_name].pax_headers


def test_create_with_f_dash_writes_tar_to_stdout(
    executable, s3_server, s3_environment
):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="create-f-dash")
    client.put_object(Bucket="create-f-dash", Key="item", Body=b"f dash data")

    result = subprocess.run(
        [str(executable), "-cf", "-", "s3://create-f-dash/"],
        env=s3_environment,
        capture_output=True,
        check=False,
    )

    assert result.returncode == 0, result.stderr.decode()
    assert result.stderr == b""
    with tarfile.open(fileobj=io.BytesIO(result.stdout), mode="r:") as archive:
        assert archive.extractfile(
            object_member("create-f-dash", "item")
        ).read() == b"f dash data"


def test_verbose_create_lists_archived_objects(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="verbose-create")
    first_put = client.put_object(
        Bucket="verbose-create", Key="first", Body=b"1"
    )
    second_put = client.put_object(
        Bucket="verbose-create", Key="folder/second", Body=b"2"
    )
    listed = {
        item["Key"]: int(item["LastModified"].timestamp())
        for item in client.list_objects_v2(Bucket="verbose-create")["Contents"]
    }
    target = tmp_path / "verbose.tar"

    result = run(
        executable,
        "--hash",
        "-c",
        "-v",
        "-f",
        str(target),
        "s3://verbose-create/",
        env=s3_environment,
    )

    assert result.returncode == 0, result.stderr
    assert result.stdout == ""
    assert result.stderr == (
        "s3ar: info: verbose-create\n"
        f"s3ar: info: verbose-create/first 1 {listed['first']} {first_put['ETag']} sha512:{hashlib.sha512(b'1').hexdigest()}\n"
        "s3ar: info: verbose-create/folder/second 1 "
        f"{listed['folder/second']} {second_put['ETag']} sha512:{hashlib.sha512(b'2').hexdigest()}\n"
    )


def test_verbose_create_to_stdout_lists_objects_on_stderr(
    executable, s3_server, s3_environment
):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="verbose-create-stdout")
    put = client.put_object(
        Bucket="verbose-create-stdout", Key="item", Body=b"data"
    )
    listed = client.list_objects_v2(Bucket="verbose-create-stdout")["Contents"][0]

    result = subprocess.run(
        [str(executable), "--hash", "-cvf", "-", "s3://verbose-create-stdout/"],
        env=s3_environment,
        capture_output=True,
        check=False,
    )

    assert result.returncode == 0, result.stderr.decode()
    assert result.stderr.decode() == (
        "s3ar: info: verbose-create-stdout\n"
        "s3ar: info: verbose-create-stdout/item 4 "
        f"{int(listed['LastModified'].timestamp())} {put['ETag']} sha512:{hashlib.sha512(b'data').hexdigest()}\n"
    )
    with tarfile.open(fileobj=io.BytesIO(result.stdout), mode="r:") as archive:
        assert archive.getnames() == [
            "verbose-create-stdout",
            object_member("verbose-create-stdout", "item"),
        ]


def test_repeated_operands_create_repeated_listing_and_archive_blocks(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    bucket = "create-repeated"
    client.create_bucket(Bucket=bucket)
    put = client.put_object(Bucket=bucket, Key="item", Body=b"data")
    listed = client.list_objects_v2(Bucket=bucket)["Contents"][0]
    archive_path = tmp_path / "repeated.tar"
    operand = f"s3://{bucket}"

    created = run(
        executable,
        "--hash",
        "-vcf",
        str(archive_path),
        operand,
        operand,
        operand,
        env=s3_environment,
    )

    block = (
        f"s3ar: info: {bucket}\n"
        f"s3ar: info: {bucket}/item 4 {int(listed['LastModified'].timestamp())} "
        f"{put['ETag']} sha512:{hashlib.sha512(b'data').hexdigest()}\n"
    )
    assert created.returncode == 0, created.stderr
    assert created.stderr == block * 3
    with tarfile.open(archive_path, "r:") as archive:
        assert archive.getnames() == [bucket, f"{bucket}/item"] * 3

    archived = run(executable, "-vtf", str(archive_path), env={})
    assert archived.returncode == 0, archived.stderr
    assert archived.stdout == (
        f"{bucket}\n"
        f"{bucket}/item 4 {int(listed['LastModified'].timestamp())} "
        f"{put['ETag']} sha512:{hashlib.sha512(b'data').hexdigest()}\n"
    ) * 3

    client.delete_object(Bucket=bucket, Key="item")
    extracted = run(
        executable, "-xf", str(archive_path), operand, env=s3_environment
    )
    assert extracted.returncode == 0, extracted.stderr
    assert client.get_object(Bucket=bucket, Key="item")["Body"].read() == b"data"


def test_create_rejects_s3_tarfile(executable):
    result = run(
        executable,
        "-cf",
        "s3://archives/backup.tar",
        "s3://source/",
    )

    assert result.returncode == 2
    assert "TARFILE must be a local filesystem path or '-'" in result.stderr


def test_unicode_keys_round_trip_is_locale_independent(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    source_bucket = "unicode-source"
    objects = {
        "čeština/žluťoučký-kůň.txt": "český obsah".encode(),
        "日本語/写真.jpg": "日本語の内容".encode(),
        "العربية/ملف.txt": "محتوى عربي".encode(),
        "emoji/🌍-🚀.bin": b"emoji",
        "special/a b+%?#.txt": b"special characters",
    }
    client.create_bucket(Bucket=source_bucket)
    for key, body in objects.items():
        client.put_object(Bucket=source_bucket, Key=key, Body=body)

    locale_path = tmp_path / "locale"
    locale_path.mkdir()
    generated = subprocess.run(
        [
            "localedef",
            "--no-archive",
            "-i",
            "tr_TR",
            "-f",
            "ISO-8859-9",
            str(locale_path / "tr_TR.ISO-8859-9"),
        ],
        capture_output=True,
        check=False,
    )
    locales = [
        {"LC_ALL": "C"},
        {"LC_ALL": "C.UTF-8"},
        {"LC_ALL": "s3ar-test-invalid-locale"},
    ]
    if generated.returncode == 0:
        locales.append(
            {
                "LC_ALL": "tr_TR.ISO-8859-9",
                "LOCPATH": str(locale_path),
            }
        )

    archives = []
    for index, locale in enumerate(locales):
        archive_path = tmp_path / f"unicode-{index}.tar"
        created = run(
            executable,
            "-c",
            "-f",
            str(archive_path),
            f"s3://{source_bucket}/",
            env={**s3_environment, **locale},
        )
        assert created.returncode == 0, created.stderr
        assert created.stdout == ""
        assert created.stderr == ""
        archives.append(archive_path)

    expected_archive = archives[0].read_bytes()
    assert all(path.read_bytes() == expected_archive for path in archives[1:])

    with tarfile.open(archives[0], "r:") as archive:
        members = {member.name: member for member in archive.getmembers()}
    bucket_member = members[source_bucket]
    assert bucket_member.pax_headers[
        "SCHILY.xattr.user.s3ar.bucket"
    ] == source_bucket
    assert "SCHILY.xattr.user.s3ar.key" not in bucket_member.pax_headers
    for key in objects:
        member = members[object_member(source_bucket, key)]
        assert member.pax_headers[
            "SCHILY.xattr.user.s3ar.bucket"
        ] == source_bucket
        assert member.pax_headers[
            "SCHILY.xattr.user.s3ar.key"
        ] == urllib.parse.quote(key, safe="/-._~")

    for archive_path, locale in zip(archives, locales):
        for key in objects:
            client.delete_object(Bucket=source_bucket, Key=key)

        extracted = run(
            executable,
            "-x",
            "-f",
            str(archive_path),
            f"s3://{source_bucket}/",
            env={**s3_environment, **locale},
        )
        assert extracted.returncode == 0, extracted.stderr
        assert extracted.stdout == ""
        assert extracted.stderr == ""
        restored = client.list_objects_v2(Bucket=source_bucket).get(
            "Contents", []
        )
        assert {item["Key"] for item in restored} == set(objects)
        for key, body in objects.items():
            response = client.get_object(Bucket=source_bucket, Key=key)
            assert response["Body"].read() == body


def test_create_continues_after_first_list_page(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    bucket = "create-pagination"
    keys = [f"item-{index:04d}" for index in range(513)]
    client.create_bucket(Bucket=bucket)
    for key in keys:
        client.put_object(Bucket=bucket, Key=key, Body=b"")
    archive_path = tmp_path / "pagination.tar"

    result = run(
        executable,
        "-c",
        "-f",
        str(archive_path),
        f"s3://{bucket}/",
        env=s3_environment,
    )

    assert result.returncode == 0, result.stderr
    with tarfile.open(archive_path, "r:") as archive:
        assert archive.getnames() == [
            bucket,
            *(object_member(bucket, key) for key in keys),
        ]


def test_long_key_round_trip(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    source_bucket = "long-key-source"
    key = "segment/" + "a" * 992
    assert len(key.encode()) == 1000
    body = b"long key data"
    client.create_bucket(Bucket=source_bucket)
    client.put_object(Bucket=source_bucket, Key=key, Body=body)
    archive_path = tmp_path / "long-key.tar"

    created = run(
        executable,
        "-c",
        "-f",
        str(archive_path),
        f"s3://{source_bucket}/",
        env=s3_environment,
    )
    assert created.returncode == 0, created.stderr

    with tarfile.open(archive_path, "r:") as archive:
        assert object_member(source_bucket, key) in archive.getnames()

    raw = raw_tar_entries(archive_path)
    object_pax = next(
        payload
        for _name, kind, payload in raw
        if kind == b"x"
        and f"path={object_member(source_bucket, key)}\n".encode() in payload
    )
    assert b"SCHILY.xattr.bucket=" not in object_pax
    assert b"LIBARCHIVE.xattr." not in object_pax

    extracted = run(
        executable,
        "-x",
        "-f",
        str(archive_path),
        f"s3://{source_bucket}/",
        env=s3_environment,
    )
    assert extracted.returncode == 0, extracted.stderr
    response = client.get_object(Bucket=source_bucket, Key=key)
    assert response["Body"].read() == body


@pytest.mark.parametrize(
    "body", [b"", bytes(range(256)) * 5000], ids=["empty", "binary"]
)
@pytest.mark.parametrize("zstd", [False, True])
def test_create_hash_payload(
    executable, s3_server, s3_environment, tmp_path, body, zstd
):
    _, client = s3_server
    bucket = "hash-payload"
    client.create_bucket(Bucket=bucket)
    client.put_object(Bucket=bucket, Key="key", Body=body)
    path = tmp_path / "hash.tar"
    result = run(
        executable, "--hash", "-cvf", str(path), *(["--zstd"] if zstd else []),
        f"s3://{bucket}/key", env=s3_environment,
    )
    assert result.returncode == 0, result.stderr
    payload = decompress_zstd(path) if zstd else path.read_bytes()
    expected = "sha512:" + hashlib.sha512(body).hexdigest()
    with tarfile.open(fileobj=io.BytesIO(payload), mode="r:") as archive:
        assert "SCHILY.xattr.user.s3ar.hash" not in archive.getmember(bucket).pax_headers
        entry = archive.getmember(f"{bucket}/key")
        assert entry.pax_headers["SCHILY.xattr.user.s3ar.hash"] == expected
        assert archive.extractfile(entry).read() == body
    assert result.stderr.rstrip().endswith(expected)
