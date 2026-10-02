from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
import json
import re
import subprocess
import threading
import urllib.parse

import pytest


EXECUTABLE = Path(__file__).resolve().parents[1] / "s3ar-list"


def run(*args, env=None):
    return subprocess.run(
        [str(EXECUTABLE), *args],
        capture_output=True,
        text=True,
        env=env,
        timeout=30,
    )


def test_list_objects_writes_only_to_stdout(s3_server, s3_environment):
    _, client = s3_server
    client.create_bucket(Bucket="list-tool-objects")
    client.put_object(Bucket="list-tool-objects", Key="item", Body=b"data")

    result = run("s3://list-tool-objects", env=s3_environment)

    assert result.returncode == 0, result.stderr
    assert result.stderr == ""
    assert result.stdout.splitlines() == [
        "s3://list-tool-objects",
        "s3://list-tool-objects/item",
    ]


def test_list_buckets_writes_only_to_stdout(s3_server, s3_environment):
    _, client = s3_server
    client.create_bucket(Bucket="list-tool-buckets")

    result = run("-b", "s3://", env=s3_environment)

    assert result.returncode == 0, result.stderr
    assert result.stderr == ""
    assert "s3://list-tool-buckets" in result.stdout.splitlines()


def test_list_bucket_mode_requires_all_buckets_operand():
    result = run("-b", "s3://some-bucket")

    assert result.returncode == 2
    assert result.stdout == ""
    assert "-b requires exactly s3://" in result.stderr


def test_list_rejects_local_archive_operand():
    result = run("archive.tar")

    assert result.returncode == 2
    assert result.stdout == ""
    assert "invalid S3 operand: archive.tar" in result.stderr


@pytest.fixture
def executable():
    return EXECUTABLE


def run_tool(executable, *arguments, env=None):
    return subprocess.run(
        [str(executable), *arguments],
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )


def test_list_reports_closed_output_pipe(executable, s3_environment, s3_server):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="closed-output-pipe")
    read_fd, write_fd = os.pipe()
    os.close(read_fd)
    try:
        result = subprocess.run(
            [str(executable), "-b", "s3://"],
            env=s3_environment,
            stdout=write_fd,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
    finally:
        os.close(write_fd)

    assert result.returncode == 1
    assert "standard output" in result.stderr


@pytest.mark.parametrize("key", ["", "/photo"])
def test_list_selection_uses_only_listing_requests(executable, key):
    class RequestCountingHandler(BaseHTTPRequestHandler):
        requests = []

        def log_message(self, _format, *_arguments):
            pass

        def do_HEAD(self):
            type(self).requests.append(("HEAD", self.path))
            self.send_response(500)
            self.send_header("Content-Length", "0")
            self.end_headers()

        def do_GET(self):
            type(self).requests.append(("GET", self.path))
            body = (
                b"<ListBucketResult><EncodingType>url</EncodingType>"
                b"<IsTruncated>false</IsTruncated>"
                b"<Contents><Key>photo</Key><Size>4</Size>"
                b"<LastModified>2026-09-03T12:00:00Z</LastModified>"
                b"<ETag>\"photo\"</ETag></Contents>"
                b"<Contents><Key>photo-old</Key><Size>5</Size>"
                b"<LastModified>2026-09-03T12:00:00Z</LastModified>"
                b"<ETag>\"neighbor\"</ETag></Contents></ListBucketResult>"
            )
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server = ThreadingHTTPServer(("127.0.0.1", 0), RequestCountingHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
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
        result = run_tool(
            executable,
            "-o", "--object-size", "--object-mtime",
            f"s3://request-list{key}",
            env=environment,
        )
    finally:
        server.shutdown()
        server.server_close()
        thread.join()

    assert result.returncode == 0, result.stderr
    assert len(RequestCountingHandler.requests) == 1
    method, path = RequestCountingHandler.requests[0]
    assert method == "GET"
    assert path.startswith("/request-list?list-type=2&max-keys=1000")
    assert "encoding-type=url" in path
    if key:
        assert "prefix=photo" in path
        assert result.stdout.splitlines() == [
            "s3://request-list/photo 4 1788436800"
        ]


def test_list_retries_temporary_redirect(executable):
    class RedirectHandler(BaseHTTPRequestHandler):
        requests = 0

        def log_message(self, _format, *_arguments):
            pass

        def do_GET(self):
            type(self).requests += 1
            if type(self).requests == 1:
                self.send_response(307)
                self.send_header("Content-Length", "0")
                self.send_header("Retry-After", "0")
                self.end_headers()
                return
            body = (
                b"<ListBucketResult><EncodingType>url</EncodingType>"
                b"<IsTruncated>false</IsTruncated></ListBucketResult>"
            )
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server = ThreadingHTTPServer(("127.0.0.1", 0), RedirectHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
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
        result = run_tool(
            executable,
            "s3://redirect-test",
            env=environment,
        )
    finally:
        server.shutdown()
        server.server_close()
        thread.join()

    assert result.returncode == 0, result.stderr
    assert RedirectHandler.requests == 2


def test_failed_request_reports_attempt_count(executable):
    class RetryHandler(BaseHTTPRequestHandler):
        requests = 0

        def log_message(self, _format, *_arguments):
            pass

        def do_GET(self):
            type(self).requests += 1
            status = 503 if type(self).requests == 1 else 400
            body = b"<Error><Code>InvalidRequest</Code></Error>"
            self.send_response(status)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Retry-After", "0")
            self.end_headers()
            self.wfile.write(body)

    server = ThreadingHTTPServer(("127.0.0.1", 0), RetryHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
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
        result = run_tool(
            executable,
            "s3://retry-failure",
            env=environment,
        )
    finally:
        server.shutdown()
        server.server_close()
        thread.join()

    assert result.returncode == 2
    assert RetryHandler.requests == 2
    assert "after 2 attempts" in result.stderr


def test_list_reports_bucket_region_on_permanent_redirect(executable):
    class RedirectHandler(BaseHTTPRequestHandler):
        def log_message(self, _format, *_arguments):
            pass

        def do_GET(self):
            self.send_response(301)
            self.send_header("Content-Length", "0")
            self.send_header("x-amz-bucket-region", "eu-central-1")
            self.end_headers()

    server = ThreadingHTTPServer(("127.0.0.1", 0), RedirectHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
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
        result = run_tool(
            executable,
            "s3://redirect-test",
            env=environment,
        )
    finally:
        server.shutdown()
        server.server_close()
        thread.join()

    assert result.returncode == 2
    assert "bucket is in region eu-central-1" in result.stderr
    assert "S3AR_REGION" in result.stderr


def test_list_decodes_url_encoded_keys_but_not_continuation_token(executable):
    class EncodedListHandler(BaseHTTPRequestHandler):
        requests = []

        def log_message(self, _format, *_arguments):
            pass

        def do_GET(self):
            parsed = urllib.parse.urlsplit(self.path)
            query = urllib.parse.parse_qs(parsed.query)
            type(self).requests.append(query)
            assert query["list-type"] == ["2"]
            assert query["encoding-type"] == ["url"]
            if "continuation-token" not in query:
                body = (
                    b"<ListBucketResult><EncodingType>url</EncodingType>"
                    b"<IsTruncated>true</IsTruncated>"
                    b"<Contents><Key>ctrl%01name.txt</Key>"
                    b"<LastModified>2026-09-02T10:00:00Z</LastModified>"
                    b"<ETag>&quot;first&quot;</ETag><Size>1</Size></Contents>"
                    b"<NextContinuationToken>next%2F+token</NextContinuationToken>"
                    b"</ListBucketResult>"
                )
            else:
                assert query["continuation-token"] == ["next%2F+token"]
                body = (
                    b"<ListBucketResult><EncodingType>url</EncodingType>"
                    b"<IsTruncated>false</IsTruncated>"
                    b"<Contents><Key>literal%252F%C5%BE</Key>"
                    b"<LastModified>2026-09-02T10:00:01Z</LastModified>"
                    b"<ETag>&quot;second&quot;</ETag><Size>2</Size></Contents>"
                    b"</ListBucketResult>"
                )
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server = ThreadingHTTPServer(("127.0.0.1", 0), EncodedListHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
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
        result = run_tool(
            executable,
            "s3://encoded-list",
            env=environment,
        )
    finally:
        server.shutdown()
        server.server_close()
        thread.join()

    assert result.returncode == 0, result.stderr
    assert result.stdout == (
        "s3://encoded-list\n"
        "s3://encoded-list/ctrl%01name.txt\n"
        "s3://encoded-list/literal%252F%C5%BE\n"
    )
    assert len(EncodedListHandler.requests) == 2


def test_list_reports_configuration_error_without_duplicate_context(
    executable,
):
    environment = os.environ.copy()
    environment.pop("S3AR_ENDPOINT", None)

    result = run_tool(executable, "s3://", env=environment)

    assert result.returncode == 2
    assert result.stderr == (
        "s3ar-list: fatal: invalid configuration: $S3AR_ENDPOINT not set\n"
    )


def test_list_buckets_writes_uris(
    executable, s3_server, s3_environment
):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="list-buckets-first")
    client.create_bucket(Bucket="list-buckets-second")

    result = run_tool(executable, "-b", "s3://", env=s3_environment)

    assert result.returncode == 0, result.stderr
    assert result.stderr == ""
    names = result.stdout.splitlines()
    assert names == sorted(names)
    assert "s3://list-buckets-first" in names
    assert "s3://list-buckets-second" in names
    assert all(name.startswith("s3://") and "/" not in name[5:] for name in names)


def test_list_buckets_follows_continuation_token(executable):
    class PaginatedHandler(BaseHTTPRequestHandler):
        requests = []

        def log_message(self, _format, *_arguments):
            pass

        def do_GET(self):
            query = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query)
            type(self).requests.append(query)
            if "continuation-token" not in query:
                body = (
                    b"<ListAllMyBucketsResult><Buckets>"
                    b"<Bucket><Name>zeta-bucket</Name></Bucket>"
                    b"</Buckets><ContinuationToken>next/+ token"
                    b"</ContinuationToken></ListAllMyBucketsResult>"
                )
            elif query["continuation-token"] == ["next/+ token"]:
                body = (
                    b"<ListAllMyBucketsResult><Buckets>"
                    b"<Bucket><Name>alpha-bucket</Name></Bucket>"
                    b"</Buckets></ListAllMyBucketsResult>"
                )
            else:
                self.send_response(400)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            self.send_response(200)
            self.send_header("Content-Type", "application/xml")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server = ThreadingHTTPServer(("127.0.0.1", 0), PaginatedHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    environment = os.environ.copy()
    environment.update(
        {
            "S3AR_ENDPOINT": f"http://127.0.0.1:{server.server_port}",
            "S3AR_REGION": "us-east-1",
            "S3AR_ACCESS_KEY": "test-access",
            "S3AR_SECRET_KEY": "test-secret",
        }
    )
    try:
        result = run_tool(executable, "-b", "s3://", env=environment)
    finally:
        server.shutdown()
        server.server_close()
        thread.join()

    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == ["s3://alpha-bucket", "s3://zeta-bucket"]
    assert PaginatedHandler.requests == [
        {"max-buckets": ["10000"]},
        {
            "continuation-token": ["next/+ token"],
            "max-buckets": ["10000"],
        },
    ]


def test_list_buckets_uses_sigv4_date_and_session_token(executable, tmp_path):
    locale_path = tmp_path / "locale"
    locale_path.mkdir()
    generated = subprocess.run(
        [
            "localedef",
            "--no-archive",
            "-i",
            "cs_CZ",
            "-f",
            "UTF-8",
            str(locale_path / "cs_CZ.UTF-8"),
        ],
        capture_output=True,
        check=False,
    )
    if generated.returncode != 0:
        pytest.skip("cs_CZ locale sources are unavailable")

    class DateCheckingS3Handler(BaseHTTPRequestHandler):
        date = None

        def log_message(self, _format, *_arguments):
            pass

        def do_GET(self):
            type(self).date = self.headers.get("x-amz-date")
            signed = self.headers.get("Authorization", "").startswith(
                "AWS4-HMAC-SHA256 "
            )
            sigv4_date = re.fullmatch(
                r"[0-9]{8}T[0-9]{6}Z", type(self).date or ""
            )
            session_token = (
                self.headers.get("x-amz-security-token") == "test-token"
            )
            if signed and sigv4_date and session_token:
                status = 200
                body = b"<ListAllMyBucketsResult><Buckets/>"
                body += b"</ListAllMyBucketsResult>"
            else:
                status = 403
                body = b"<Error><Code>AccessDenied</Code></Error>"
            self.send_response(status)
            self.send_header("Content-Type", "application/xml")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server = ThreadingHTTPServer(("127.0.0.1", 0), DateCheckingS3Handler)
    thread = threading.Thread(target=server.serve_forever)
    thread.start()
    try:
        environment = os.environ.copy()
        environment.update(
            {
                "LANG": "C.UTF-8",
                "LC_TIME": "cs_CZ.UTF-8",
                "LOCPATH": str(locale_path),
                "S3AR_ENDPOINT": f"http://127.0.0.1:{server.server_port}",
                "S3AR_ACCESS_KEY": "test-access",
                "S3AR_SECRET_KEY": "test-secret",
                "S3AR_SESSION_TOKEN": "test-token",
            }
        )
        result = run_tool(executable, "-b", "s3://", env=environment)
    finally:
        server.shutdown()
        thread.join()
        server.server_close()

    assert result.returncode == 0, result.stderr
    assert DateCheckingS3Handler.date is not None


def test_list_buckets_includes_requested_acl(
    executable, s3_server, s3_environment
):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="list-buckets-verbose")

    result = run_tool(executable, "-b", "s3://", "--bucket-acl", env=s3_environment)

    assert result.returncode == 0, result.stderr
    assert "s3://list-buckets-verbose private" in result.stdout.splitlines()


def test_bucket_acl_group_uris_require_exact_match(executable):
    buckets = (
        b"<ListAllMyBucketsResult><Buckets><Bucket><Name>acl-uri</Name>"
        b"</Bucket></Buckets></ListAllMyBucketsResult>"
    )
    acl = (
        b"<AccessControlPolicy><Owner><ID>owner</ID></Owner>"
        b"<AccessControlList>"
        b"<Grant><Grantee><URI>"
        b"http://acs.amazonaws.com/groups/global/AllUsers"
        b"</URI></Grantee><Permission>READ</Permission></Grant>"
        b"<Grant><Grantee><URI>https://example.test/AuthenticatedUsers"
        b"</URI></Grantee><Permission>WRITE</Permission></Grant>"
        b"</AccessControlList></AccessControlPolicy>"
    )

    class AclHandler(BaseHTTPRequestHandler):
        acl_requests = 0

        def log_message(self, _format, *_arguments):
            pass

        def do_GET(self):
            parsed = urllib.parse.urlsplit(self.path)
            if parsed.path == "/" and "max-buckets=" in parsed.query:
                body = buckets
                status = 200
            elif parsed.path == "/acl-uri" and parsed.query == "acl":
                type(self).acl_requests += 1
                body = acl
                status = 200
            else:
                body = b""
                status = 404
            self.send_response(status)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server = ThreadingHTTPServer(("127.0.0.1", 0), AclHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
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
        plain = run_tool(executable, "-bv", "s3://", env=environment)
        assert plain.returncode == 0, plain.stderr
        assert plain.stdout == "s3://acl-uri\n"
        assert AclHandler.acl_requests == 0
        result = run_tool(executable, "-b", "s3://", "--bucket-acl", env=environment)
    finally:
        server.shutdown()
        server.server_close()
        thread.join()

    assert result.returncode == 0, result.stderr
    assert result.stdout == "s3://acl-uri public-read,custom\n"
    assert AclHandler.acl_requests == 1


def test_list_multiple_live_s3_operands(
    executable, s3_server, s3_environment
):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="list-multiple")
    client.put_object(Bucket="list-multiple", Key="first/a", Body=b"a")
    client.put_object(Bucket="list-multiple", Key="second/b", Body=b"b")
    client.put_object(Bucket="list-multiple", Key="outside", Body=b"x")

    result = run_tool(
        executable,
        "s3://list-multiple/first/",
        "s3://list-multiple/second/",
        env=s3_environment,
    )

    assert result.returncode == 0, result.stderr
    assert result.stdout == (
        "s3://list-multiple\n"
        "s3://list-multiple/first/a\n"
        "s3://list-multiple\n"
        "s3://list-multiple/second/b\n"
    )


def test_list_objects_in_one_bucket_including_empty_bucket(
    executable, s3_server, s3_environment
):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="list-one")
    client.put_object(Bucket="list-one", Key="z.txt", Body=b"z")
    client.put_object(Bucket="list-one", Key="folder/a.txt", Body=b"a")

    result = run_tool(executable, "s3://list-one", env=s3_environment)

    assert result.returncode == 0, result.stderr
    assert result.stdout == (
        "s3://list-one\n"
        "s3://list-one/folder/a.txt\n"
        "s3://list-one/z.txt\n"
    )

    client.create_bucket(Bucket="list-empty")
    empty = run_tool(
        executable,
        "s3://list-empty/",
        env=s3_environment,
    )
    assert empty.returncode == 0, empty.stderr
    assert empty.stdout == "s3://list-empty\n"


def test_list_requested_object_fields(executable, s3_server, s3_environment):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="list-verbose")
    put = client.put_object(
        Bucket="list-verbose",
        Key="folder/a b+%ž",
        Body=b"1234",
        Metadata={"source": "pytest", "sha256": "3472a7"},
    )
    listed = client.list_objects_v2(Bucket="list-verbose")["Contents"][0]

    result = run_tool(
        executable,
        "--object-etag",
        "--object-mtime",
        "--object-size",
        "s3://list-verbose/",
        env={**s3_environment, "TZ": "Europe/Prague"},
    )

    assert result.returncode == 0, result.stderr
    assert result.stdout == (
        "s3://list-verbose\n"
        "s3://list-verbose/folder/a%20b%2B%25%C5%BE"
        f" {put['ETag']} {int(listed['LastModified'].timestamp())} 4\n"
    )


def test_list_objects_in_all_s3_buckets(
    executable, s3_server, s3_environment
):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="aaa-list-all")
    client.create_bucket(Bucket="zzz-list-all")
    client.put_object(Bucket="aaa-list-all", Key="object", Body=b"x")

    result = run_tool(executable, "s3://", env=s3_environment)

    assert result.returncode == 0, result.stderr
    lines = result.stdout.splitlines()
    assert "s3://aaa-list-all/object" in lines
    assert "s3://aaa-list-all" in lines
    assert "s3://zzz-list-all" in lines


def test_list_live_s3_prefix(executable, s3_server, s3_environment):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="list-prefix")
    client.put_object(Bucket="list-prefix", Key="folder/first", Body=b"1")
    client.put_object(Bucket="list-prefix", Key="folder/second", Body=b"22")
    client.put_object(Bucket="list-prefix", Key="outside", Body=b"outside")

    result = run_tool(
        executable,
        "s3://list-prefix/folder/",
        env=s3_environment,
    )

    assert result.returncode == 0, result.stderr
    assert result.stdout == (
        "s3://list-prefix\n"
        "s3://list-prefix/folder/first\n"
        "s3://list-prefix/folder/second\n"
    )


def test_list_live_s3_continues_after_first_page(
    executable, s3_server, s3_environment
):
    _endpoint, client = s3_server
    bucket = "list-pagination"
    keys = [f"item-{index:04d}" for index in range(1001)]
    client.create_bucket(Bucket=bucket)
    for key in keys:
        client.put_object(Bucket=bucket, Key=key, Body=b"")

    result = run_tool(
        executable,
        f"s3://{bucket}",
        env=s3_environment,
    )

    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == [
        f"s3://{bucket}",
        *(f"s3://{bucket}/{key}" for key in keys),
    ]


@pytest.mark.parametrize("buckets", [False, True])
@pytest.mark.parametrize("response", ["retry", "denied", "invalid_xml", "too_large"])
def test_listing_http_responses(s3_environment, buckets, response):
    from fault_server import FaultServer, ResponseStep

    path = (
        "/?max-buckets=10000" if buckets
        else "/bucket?list-type=2&max-keys=1000&encoding-type=url"
    )
    valid = (
        b"<ListAllMyBucketsResult><Buckets/></ListAllMyBucketsResult>" if buckets
        else b"<ListBucketResult><EncodingType>url</EncodingType>"
             b"<IsTruncated>false</IsTruncated></ListBucketResult>"
    )
    if response == "retry":
        steps = [
            ResponseStep("GET", path, 503, b"<Error><Code>SlowDown</Code></Error>"),
            ResponseStep("GET", path, 200, valid),
        ]
        expected = None
    else:
        status, body, expected = {
            "denied": (403, b"<Error><Code>AccessDenied</Code></Error>", "AccessDenied"),
            "invalid_xml": (200, b"<WrongRoot/>", "invalid ListBuckets XML" if buckets
                            else "invalid ListObjectsV2 XML"),
            "too_large": (200, b"x" * (16 * 1024 * 1024 + 1),
                          "S3 XML response is too large"),
        }[response]
        steps = [ResponseStep("GET", path, status, body)]
    with FaultServer(steps) as server:
        environment = {**s3_environment, "S3AR_ENDPOINT": server.endpoint}
        arguments = ["-b", "s3://"] if buckets else ["s3://bucket"]
        result = run(*arguments, env=environment)
    assert result.returncode == (0 if expected is None else 2), result.stderr
    if expected is not None:
        assert expected in result.stderr
    else:
        assert result.stdout == ("" if buckets else "s3://bucket\n")
    assert len(server.requests) == len(steps)


@pytest.mark.parametrize("mode", [[], ["-o"], ["--objects"], ["-bo"], ["-ob"]])
@pytest.mark.parametrize("fields", [[], ["--object-size"], ["--object-mtime"],
                                  ["--object-mtime", "--object-size"]])
def test_list_output_selection(mode, fields, s3_server, s3_environment):
    _, client = s3_server
    bucket = "list-output-selection"
    client.create_bucket(Bucket=bucket)
    client.put_object(Bucket=bucket, Key="item", Body=b"data")
    listed = client.list_objects_v2(Bucket=bucket)["Contents"][0]
    expected = f"s3://{bucket}/item"
    for field in fields:
        expected += (" 4" if field == "--object-size"
                     else f" {int(listed['LastModified'].timestamp())}")
    expected += "\n"
    if mode not in (["-o"], ["--objects"]):
        expected = f"s3://{bucket}\n" + expected
    for verbosity in ([], ["-v"], ["-vvv"]):
        result = run(*mode, *fields, *verbosity, f"s3://{bucket}",
                     env=s3_environment)
        assert result.returncode == 0, result.stderr
        assert result.stdout == expected


@pytest.mark.parametrize("arguments, message", [
    (["-b", "--object-size"], "object fields require object output"),
    (["-b", "--object-mtime"], "object fields require object output"),
    (["-b", "--object-etag"], "object fields require object output"),
    (["-o", "--bucket-acl"], "--bucket-acl requires bucket output"),
    (["-bb"], "bucket mode specified twice"),
    (["-oo"], "object mode specified twice"),
])
def test_list_invalid_output_selection(arguments, message):
    result = run(*arguments, "s3://")
    assert result.returncode == 2
    assert result.stdout == ""
    assert message in result.stderr


def test_list_objects_only_empty_and_all_buckets(s3_server, s3_environment):
    _, client = s3_server
    client.create_bucket(Bucket="list-only-empty")
    result = run("-o", "s3://list-only-empty", env=s3_environment)
    assert result.returncode == 0, result.stderr
    assert result.stdout == ""
    client.create_bucket(Bucket="list-only-full")
    client.put_object(Bucket="list-only-full", Key="item", Body=b"")
    result = run("-o", "s3://", env=s3_environment)
    assert result.returncode == 0, result.stderr
    assert "s3://list-only-full/item" in result.stdout.splitlines()
    assert all("/" in line[5:] for line in result.stdout.splitlines())


@pytest.mark.parametrize("mode", [[], ["-bo"]])
def test_list_combined_bucket_acl(mode, s3_server, s3_environment):
    _, client = s3_server
    client.create_bucket(Bucket="list-combined-acl")
    client.put_object(Bucket="list-combined-acl", Key="item", Body=b"x")
    result = run(*mode, "--bucket-acl", "--object-size",
                 "s3://list-combined-acl", env=s3_environment)
    assert result.returncode == 0, result.stderr
    assert result.stdout == ("s3://list-combined-acl private\n"
                             "s3://list-combined-acl/item 1\n")


@pytest.mark.parametrize("options, expected", [
    (["--object-metadata"], ' {"empty":"","source":"upload","z":"last"}'),
    (["--object-meta", "SOURCE"], ' upload'),
    (["--object-meta", "missing", "--object-meta", "empty"], '  '),
    (["--object-meta", "source", "--object-meta", "SOURCE"], ' upload'),
    (["--object-metadata", "--object-meta", "source", "--object-meta", "missing"],
     ' {"empty":"","source":"upload","z":"last"} upload '),
])
def test_list_object_metadata(options, expected, s3_server, s3_environment):
    _, client = s3_server
    client.create_bucket(Bucket="list-metadata")
    client.put_object(Bucket="list-metadata", Key="item", Body=b"data",
                      Metadata={"z": "last", "source": "upload", "empty": ""})
    result = run("-o", "--object-size", *options, "s3://list-metadata",
                 env=s3_environment)
    assert result.returncode == 0, result.stderr
    assert result.stdout == "s3://list-metadata/item 4" + expected + "\n"


@pytest.mark.parametrize("options", [[], ["--object-metadata"],
                                     ["--object-meta", "source"],
                                     ["--object-metadata", "--object-meta", "source"]])
def test_list_metadata_requests_and_pagination(options, s3_environment):
    from fault_server import FaultServer, ResponseStep

    def page(key, continuation=""):
        return (
            '<ListBucketResult><EncodingType>url</EncodingType>'
            f'<IsTruncated>{"true" if continuation else "false"}</IsTruncated>'
            f'<Contents><Key>{key}</Key><Size>4</Size>'
            '<LastModified>2026-09-03T12:00:00Z</LastModified></Contents>'
            + (f'<NextContinuationToken>{continuation}</NextContinuationToken>'
               if continuation else '') + '</ListBucketResult>'
        ).encode()

    path = "/bucket?list-type=2&max-keys=1000&encoding-type=url"
    steps = [ResponseStep("GET", path, 200, page("a", "next"))]
    if options:
        steps.append(ResponseStep("HEAD", "/bucket/a", 200,
                                  headers=(("x-amz-meta-source", 'a "quote" \\ path'),)))
    # Match the query order used by the object listing implementation.
    steps.append(ResponseStep("GET", path + "&continuation-token=next",
                              200, page("b")))
    if options:
        steps.append(ResponseStep("HEAD", "/bucket/b", 200))
    with FaultServer(steps) as server:
        result = run("-o", *options, "s3://bucket",
                     env={**s3_environment, "S3AR_ENDPOINT": server.endpoint})
    assert result.returncode == 0, result.stderr
    first = second = ''
    if "--object-metadata" in options:
        first += ' ' + json.dumps({"source": 'a "quote" \\ path'}, separators=(',', ':'))
        second += ' {}'
    if "--object-meta" in options:
        first += ' a "quote" \\ path'
        second += ' '
    assert result.stdout == f"s3://bucket/a{first}\ns3://bucket/b{second}\n"
    assert len(server.requests) == len(steps)


@pytest.mark.parametrize("status", [403, 404])
def test_list_metadata_head_failure(status, s3_environment):
    from fault_server import FaultServer, ResponseStep

    listing = (b'<ListBucketResult><EncodingType>url</EncodingType>'
               b'<IsTruncated>false</IsTruncated><Contents><Key>item</Key>'
               b'<Size>1</Size><LastModified>2026-09-03T12:00:00Z</LastModified>'
               b'</Contents></ListBucketResult>')
    with FaultServer([
        ResponseStep("GET", "/bucket?list-type=2&max-keys=1000&encoding-type=url",
                     200, listing),
        ResponseStep("HEAD", "/bucket/item", status),
    ]) as server:
        result = run("-o", "--object-metadata", "s3://bucket",
                     env={**s3_environment, "S3AR_ENDPOINT": server.endpoint})
    assert result.returncode == 2
    assert result.stdout == ""
    assert "unable to read object metadata bucket/item" in result.stderr


@pytest.mark.parametrize("options", [["-b", "--object-metadata"],
                                     ["-b", "--object-meta", "source"],
                                     ["-o", "--object-meta", ""],
                                     ["-o", "s3://bucket", "--object-meta"]])
def test_list_metadata_invalid_options(options):
    result = run(*options)
    assert result.returncode == 2
    assert result.stdout == ""


@pytest.mark.parametrize("value, expected", [(None, ""), ("", ""), ("-", "-")])
def test_list_metadata_missing_empty_and_dash(value, expected, s3_server, s3_environment):
    _, client = s3_server
    client.create_bucket(Bucket="list-metadata-sentinel")
    client.put_object(Bucket="list-metadata-sentinel", Key="item", Body=b"",
                      Metadata={} if value is None else {"source": value})
    for options in (["--object-meta", "source"], ["--object-metadata"],
                    ["--object-metadata", "--object-meta", "source"]):
        result = run("-o", *options, "s3://list-metadata-sentinel", env=s3_environment)
        assert result.returncode == 0, result.stderr
        columns = ["s3://list-metadata-sentinel/item"]
        if "--object-metadata" in options:
            columns.append(json.dumps({} if value is None else {"source": value},
                                      separators=(',', ':')))
        if "--object-meta" in options:
            columns.append(expected)
        assert result.stdout == " ".join(columns) + "\n"


@pytest.mark.parametrize("argument, separator", [
    ("|", b"|"), (r"\t", b"\t"), (r"\n", b"\n"),
    (r"\0", b"\0"), (r"\\", b"\\"), (r"<\0>\t", b"<\0>\t"),
    ("", b""), ("::", b"::"),
])
def test_list_delimiter_and_field_order(argument, separator, s3_server, s3_environment):
    _, client = s3_server
    client.create_bucket(Bucket="list-delimiter")
    client.put_object(Bucket="list-delimiter", Key="item", Body=b"data",
                      Metadata={"source": 'manual "upload" \\ path', "z": "last"})
    arguments = ["-o", "--delimiter", argument, "--object-meta", "z",
                 "--object-size", "--object-meta", "source", "--object-metadata",
                 "--object-meta", "missing", "--object-size", "--object-meta", "Z",
                 "s3://list-delimiter"]
    result = subprocess.run([str(EXECUTABLE), *arguments], capture_output=True,
                            env=s3_environment, timeout=30)
    assert result.returncode == 0, result.stderr
    columns = [b"s3://list-delimiter/item", b"last", b"4",
               b'manual "upload" \\ path',
               b'{"source":"manual \\"upload\\" \\\\ path","z":"last"}', b'']
    assert result.stdout == separator.join(columns) + b"\n"


@pytest.mark.parametrize("delimiter", [r"\x", "trailing\\"])
def test_list_rejects_invalid_delimiter_escape(delimiter):
    result = run("-o", "--delimiter", delimiter, "s3://bucket")
    assert result.returncode == 2
    assert result.stdout == ""
    assert "invalid escape in --delimiter" in result.stderr


def test_list_delimiter_for_bucket_acl(s3_server, s3_environment):
    _, client = s3_server
    client.create_bucket(Bucket="list-delimiter-acl")
    result = run("-b", "--bucket-acl", "--delimiter", "|", "s3://",
                 env=s3_environment)
    assert result.returncode == 0, result.stderr
    assert "s3://list-delimiter-acl|private" in result.stdout.splitlines()
