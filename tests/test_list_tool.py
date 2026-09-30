from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
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


def test_list_bucket_avoids_redundant_head_request(executable):
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
                b"</ListBucketResult>"
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
            "s3://request-list",
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


def test_verbose_list_buckets_includes_acl(
    executable, s3_server, s3_environment
):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="list-buckets-verbose")

    result = run_tool(executable, "-b", "s3://", "-v", env=s3_environment)

    assert result.returncode == 0, result.stderr
    assert "s3://list-buckets-verbose acl=private" in result.stdout.splitlines()


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
        def log_message(self, _format, *_arguments):
            pass

        def do_GET(self):
            parsed = urllib.parse.urlsplit(self.path)
            if parsed.path == "/" and "max-buckets=" in parsed.query:
                body = buckets
                status = 200
            elif parsed.path == "/acl-uri" and parsed.query == "acl":
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
        result = run_tool(executable, "-b", "s3://", "-v", env=environment)
    finally:
        server.shutdown()
        server.server_close()
        thread.join()

    assert result.returncode == 0, result.stderr
    assert result.stdout == "s3://acl-uri acl=public-read,custom\n"


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


def test_verbose_list_s3(executable, s3_server, s3_environment):
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
        "-v",
        "s3://list-verbose/",
        env={**s3_environment, "TZ": "Europe/Prague"},
    )

    assert result.returncode == 0, result.stderr
    assert result.stdout == (
        "s3://list-verbose\n"
        "s3://list-verbose/folder/a%20b%2B%25%C5%BE"
        f" size=4 mtime={int(listed['LastModified'].timestamp())}"
        f" etag={put['ETag']}\n"
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
