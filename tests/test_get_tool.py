from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import errno
import os
from pathlib import Path
import signal
import stat
import subprocess
import threading
import urllib.parse

import pytest

from fault_server import FaultServer, ResponseStep


EXECUTABLE = Path(__file__).parents[1] / "s3ar-get"
RETRY_PROBE = Path(__file__).parents[1] / "test-get-retry"
PART_SIZE = 16 * 1024 * 1024
GET_STDOUT_SUCCESS = (
    b"s3ar-get: success: s3://bucket/key to stdout\n"
)


def run_server(environment, data):
    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            assert urllib.parse.urlsplit(self.path).path == "/bucket/key"
            self.send_response(200)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("ETag", '"download"')
            self.end_headers()
            self.wfile.write(data)

        def log_message(self, _format, *_args):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    configured = environment.copy()
    configured["S3AR_ENDPOINT"] = f"http://127.0.0.1:{server.server_port}"
    return server, thread, configured


def stop_server(server, thread):
    server.shutdown()
    server.server_close()
    thread.join()


@pytest.mark.parametrize("interrupt_signal", [signal.SIGINT, signal.SIGTERM])
def test_interrupted_get_removes_temporary_file(
    s3_environment, tmp_path, interrupt_signal
):
    started = threading.Event()
    release = threading.Event()

    class SlowHandler(BaseHTTPRequestHandler):
        def log_message(self, _format, *_arguments):
            pass

        def do_GET(self):
            self.send_response(200)
            self.send_header("Content-Length", "1048576")
            self.send_header("ETag", '"interrupted"')
            self.end_headers()
            started.set()
            release.wait(timeout=10)

    server = ThreadingHTTPServer(("127.0.0.1", 0), SlowHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    environment = s3_environment.copy()
    environment["S3AR_ENDPOINT"] = f"http://127.0.0.1:{server.server_port}"
    destination = tmp_path / "download"
    destination.write_bytes(b"previous content")
    process = subprocess.Popen(
        [str(EXECUTABLE), "-f", str(destination), "s3://bucket/key"],
        env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    try:
        assert started.wait(timeout=5)
        process.send_signal(interrupt_signal)
        _stdout, stderr = process.communicate(timeout=10)
    finally:
        release.set()
        if process.poll() is None:
            process.kill()
            process.wait()
        server.shutdown()
        server.server_close()
        thread.join()

    assert process.returncode == 2
    assert b"interrupted" in stderr
    assert destination.read_bytes() == b"previous content"
    assert list(tmp_path.glob("download.tmp.*")) == []


def test_get_distinguishes_client_initialization_failure(s3_environment):
    environment = s3_environment.copy()
    environment["S3AR_ENDPOINT"] = "invalid-endpoint"

    result = subprocess.run(
        [str(EXECUTABLE), "s3://bucket/key"],
        capture_output=True,
        env=environment,
        timeout=30,
    )

    assert result.returncode == 2
    assert b"unable to initialize S3 client" in result.stderr
    assert b"download failed" not in result.stderr


def test_get_validates_uri_and_configuration_before_output(s3_environment):
    cases = [
        ("invalid", s3_environment, b"invalid source"),
        ("s3://bucket/key", {}, b"invalid configuration"),
        (
            "s3://bucket/key",
            {**s3_environment, "S3AR_ENDPOINT": "invalid-endpoint"},
            b"unable to initialize S3 client",
        ),
    ]
    for uri, environment, diagnostic in cases:
        result = subprocess.run(
            [str(EXECUTABLE), "-f", "/dev/null", uri],
            capture_output=True,
            env=environment,
            timeout=30,
        )
        assert result.returncode == 2
        assert diagnostic in result.stderr
        assert b"output is not a regular file" not in result.stderr


@pytest.mark.parametrize(
    ("uri", "uri_style"),
    [
        (b"s3://bucket/invalid\xff", "path"),
        (b"s3://bucket\xff/key", "virtual"),
        (b"s3://Upper_under/key", "virtual"),
    ],
)
def test_get_validates_object_name_before_output(
    s3_environment, uri, uri_style
):
    environment = {**s3_environment, "S3AR_URI_STYLE": uri_style}
    result = subprocess.run(
        [str(EXECUTABLE), "-f", "/dev/null", uri],
        capture_output=True,
        env=environment,
        timeout=30,
    )

    assert result.returncode == 2
    assert b"invalid source: invalid S3 bucket or object key" in result.stderr
    assert b"output is not a regular file" not in result.stderr


@pytest.mark.parametrize("data", [b"123456789", b""])
def test_get_writes_stdout(s3_environment, data):
    server, thread, environment = run_server(s3_environment, data)
    try:
        result = subprocess.run(
            [str(EXECUTABLE), "s3://bucket/key"],
            capture_output=True,
            env=environment,
            timeout=30,
        )
    finally:
        stop_server(server, thread)

    assert result.returncode == 0, result.stderr.decode()
    assert result.stdout == data
    assert result.stderr == GET_STDOUT_SUCCESS


def test_get_encodes_source_name_in_diagnostics(s3_environment):
    key = "folder/line\n name%#"
    encoded_uri = b"s3://bucket/folder/line%0A%20name%25%23"
    steps = [
        ResponseStep(
            "GET", "/bucket/folder/line%0A%20name%25%23", 200, b"data",
            headers=(("ETag", '"encoded-name"'),),
        )
    ]
    with FaultServer(steps) as server:
        environment = {**s3_environment, "S3AR_ENDPOINT": server.endpoint}
        result = subprocess.run(
            [str(EXECUTABLE), "-vv", f"s3://bucket/{key}"],
            capture_output=True,
            env=environment,
            timeout=10,
        )

    assert result.returncode == 0, result.stderr.decode()
    assert result.stdout == b"data"
    assert result.stderr.count(encoded_uri) == 3
    assert b"line\n name" not in result.stderr


def test_get_writes_redirected_stdout(s3_environment, tmp_path):
    data = b"downloaded to a regular stdout file"
    destination = tmp_path / "redirected.bin"
    server, thread, environment = run_server(s3_environment, data)
    try:
        with destination.open("wb") as output:
            result = subprocess.run(
                [str(EXECUTABLE), "s3://bucket/key"],
                stdout=output,
                stderr=subprocess.PIPE,
                env=environment,
                timeout=30,
            )
    finally:
        stop_server(server, thread)

    assert result.returncode == 0, result.stderr.decode()
    assert destination.read_bytes() == data
    assert result.stderr == GET_STDOUT_SUCCESS


def test_get_writes_with_explicit_temporary_file(s3_environment, tmp_path):
    data = b"downloaded data"
    destination = tmp_path / "output"
    temporary = tmp_path / "staging"
    server, thread, environment = run_server(s3_environment, data)
    try:
        result = subprocess.run(
            [
                str(EXECUTABLE),
                "-v",
                "-f",
                str(destination),
                "-t",
                str(temporary),
                "s3://bucket/key",
            ],
            capture_output=True,
            env=environment,
            timeout=30,
        )
    finally:
        stop_server(server, thread)

    assert result.returncode == 0, result.stderr.decode()
    assert destination.read_bytes() == data
    assert result.stderr == (
        b"s3ar-get: info: s3://bucket/key downloaded\n"
        + f"s3ar-get: success: s3://bucket/key to {destination}\n".encode()
    )
    assert not temporary.exists()


@pytest.mark.parametrize("kind", ["device", "directory", "symlink"])
@pytest.mark.parametrize("explicit_temporary", [False, True])
def test_get_rejects_nonregular_temporary_target(
    s3_environment, tmp_path, kind, explicit_temporary
):
    temporary = tmp_path / "staging"
    if kind == "device":
        destination = Path("/dev/null")
    else:
        destination = tmp_path / "output"
        if kind == "directory":
            destination.mkdir()
        else:
            original = tmp_path / "original"
            original.write_bytes(b"unchanged")
            destination.symlink_to(original)

    command = [str(EXECUTABLE), "-f", str(destination)]
    if explicit_temporary:
        command.extend(["-t", str(temporary)])
    result = subprocess.run(
        [*command, "s3://bucket/key"],
        capture_output=True,
        env=s3_environment,
        timeout=30,
    )

    assert result.returncode == 2
    assert b"output is not a regular file" in result.stderr
    assert not temporary.exists()
    if kind != "device":
        assert list(tmp_path.glob("output.tmp.*")) == []
    if kind == "device":
        assert stat.S_ISCHR(destination.stat().st_mode)
    elif kind == "directory":
        assert destination.is_dir()
    else:
        assert destination.is_symlink()
        assert original.read_bytes() == b"unchanged"


def test_get_default_temporary_preserves_output_on_client_failure(
    s3_environment, tmp_path
):
    destination = tmp_path / "output"
    destination.write_bytes(b"old output")
    environment = s3_environment.copy()
    environment["S3AR_ENDPOINT"] = "invalid-endpoint"

    result = subprocess.run(
        [str(EXECUTABLE), "-f", str(destination), "s3://bucket/key"],
        capture_output=True,
        env=environment,
        timeout=30,
    )

    assert result.returncode == 2
    assert destination.read_bytes() == b"old output"
    assert list(tmp_path.iterdir()) == [destination]


def test_get_preserves_existing_temporary_file(s3_environment, tmp_path):
    destination = tmp_path / "output"
    temporary = tmp_path / "staging"
    destination.write_bytes(b"old output")
    temporary.write_bytes(b"old temporary")

    result = subprocess.run(
        [
            str(EXECUTABLE),
            "-f",
            str(destination),
            "-t",
            str(temporary),
            "s3://bucket/key",
        ],
        capture_output=True,
        env=s3_environment,
        timeout=30,
    )

    assert result.returncode == 2
    assert destination.read_bytes() == b"old output"
    assert temporary.read_bytes() == b"old temporary"


@pytest.mark.parametrize("file_args", [[], ["-f", "-"]])
def test_get_rejects_temporary_file_without_output(tmp_path, file_args):
    temporary = tmp_path / "staging"
    result = subprocess.run(
        [str(EXECUTABLE), *file_args, "-t", str(temporary), "s3://bucket/key"],
        capture_output=True,
        timeout=30,
    )

    assert result.returncode == 2
    assert b"-t requires -f FILE" in result.stderr
    assert not temporary.exists()


def test_get_accepts_connection_close_after_complete_body(s3_environment):
    data = bytes(range(251)) * 101
    steps = [
        ResponseStep(
            "GET",
            "/bucket/key",
            200,
            data,
            headers=(("ETag", '"complete-body"'),),
            absent_headers=("Range", "If-Match"),
            disconnect_after=len(data),
        )
    ]

    with FaultServer(steps) as server:
        environment = s3_environment.copy()
        environment["S3AR_ENDPOINT"] = server.endpoint
        result = subprocess.run(
            [str(EXECUTABLE), "s3://bucket/key"],
            capture_output=True,
            env=environment,
            timeout=10,
        )

    assert result.returncode == 0, result.stderr.decode()
    assert result.stdout == data
    assert result.stderr == GET_STDOUT_SUCCESS


def test_get_uses_archive_xattrs_for_conditional_refresh(s3_environment, tmp_path):
    path = tmp_path / "key"
    path.write_bytes(b"old")
    try:
        os.setxattr(path, "user.s3ar.format", b"1")
        os.setxattr(path, "user.s3ar.bucket", b"bucket")
        os.setxattr(path, "user.s3ar.key", b"key")
        os.setxattr(path, "user.s3ar.etag", b'"old"')
    except (AttributeError, OSError) as exc:
        pytest.skip(f"filesystem xattrs unavailable: {exc}")
    steps = [
        ResponseStep("GET", "/bucket/key", 304,
                     expected_headers=(("If-None-Match", '"old"'),)),
        ResponseStep("GET", "/bucket/key", 200, b"new",
                     headers=(("ETag", '"new"'),),
                     expected_headers=(("If-None-Match", '"old"'),)),
        ResponseStep("GET", "/bucket/key", 304,
                     expected_headers=(("If-None-Match", '"new"'),)),
    ]
    with FaultServer(steps) as server:
        environment = s3_environment.copy()
        environment["S3AR_ENDPOINT"] = server.endpoint
        for attempt, expected in enumerate((b"old", b"new", b"new")):
            before = path.stat()
            result = subprocess.run(
                [str(EXECUTABLE), "-vv", "-f", str(path), "s3://bucket/key"],
                capture_output=True, env=environment, timeout=10,
            )
            assert result.returncode == 0, result.stderr.decode()
            assert path.read_bytes() == expected
            if attempt == 1:
                assert b"info: s3://bucket/key downloaded" in result.stderr
            else:
                assert b"info: s3://bucket/key not modified" in result.stderr
            assert f"success: s3://bucket/key to {path}".encode() in result.stderr
            if attempt != 1:
                assert b"xattr user.s3ar.format = '1' (match)" in result.stderr
                assert b"xattr user.s3ar.bucket = 'bucket' (match)" in result.stderr
                assert b"xattr user.s3ar.key = 'key' (match)" in result.stderr
                cached = b'"old"' if attempt == 0 else b'"new"'
                assert b"xattr user.s3ar.etag = '" + cached + b"' (valid)" in result.stderr
                assert b"S3 object not modified" in result.stderr
                assert path.stat().st_ino == before.st_ino
                assert path.stat().st_mtime_ns == before.st_mtime_ns
            else:
                assert b"xattr user.s3ar.etag = '\"new\"' (saved)" in result.stderr
    assert os.getxattr(path, "user.s3ar.etag") == b'"new"'


@pytest.mark.parametrize("attribute,value", [
    ("user.s3ar.format", b"2"),
    ("user.s3ar.bucket", b"other"),
    ("user.s3ar.key", b"other"),
    ("user.s3ar.etag", b"bad\nvalue"),
])
def test_get_ignores_untrusted_cache_identity(
    s3_environment, tmp_path, attribute, value
):
    path = tmp_path / "key"
    path.write_bytes(b"old")
    try:
        for name, data in (
            ("user.s3ar.format", b"1"),
            ("user.s3ar.bucket", b"bucket"),
            ("user.s3ar.key", b"key"),
            ("user.s3ar.etag", b'"old"'),
        ):
            os.setxattr(path, name, data)
        os.setxattr(path, attribute, value)
    except (AttributeError, OSError) as exc:
        pytest.skip(f"filesystem xattrs unavailable: {exc}")
    steps = [ResponseStep("GET", "/bucket/key", 200, b"new",
                          headers=(("ETag", '"new"'),),
                          absent_headers=("If-None-Match",))]
    with FaultServer(steps) as server:
        environment = s3_environment.copy()
        environment["S3AR_ENDPOINT"] = server.endpoint
        result = subprocess.run(
            [str(EXECUTABLE), "-vv", "-f", str(path), "s3://bucket/key"],
            capture_output=True, env=environment, timeout=10,
        )
    assert result.returncode == 0, result.stderr.decode()
    assert path.read_bytes() == b"new"
    assert b"conditional GET disabled: no valid cached ETag" in result.stderr
    if attribute == "user.s3ar.etag":
        assert b"warning: xattr user.s3ar.etag: invalid" in result.stderr
    else:
        assert b"warning:" not in result.stderr


def test_get_matches_archive_encoded_key(s3_environment, tmp_path):
    path = tmp_path / "key"
    path.write_bytes(b"old")
    try:
        for name, data in (
            ("user.s3ar.format", b"1"),
            ("user.s3ar.bucket", b"bucket"),
            ("user.s3ar.key", b"folder/key%20name"),
            ("user.s3ar.etag", b'"old"'),
        ):
            os.setxattr(path, name, data)
    except (AttributeError, OSError) as exc:
        pytest.skip(f"filesystem xattrs unavailable: {exc}")
    steps = [ResponseStep("GET", "/bucket/folder/key%20name", 304,
                          expected_headers=(("If-None-Match", '"old"'),))]
    with FaultServer(steps) as server:
        environment = s3_environment.copy()
        environment["S3AR_ENDPOINT"] = server.endpoint
        result = subprocess.run(
            [str(EXECUTABLE), "-f", str(path), "s3://bucket/folder/key name"],
            capture_output=True, env=environment, timeout=10,
        )
    assert result.returncode == 0, result.stderr.decode()
    assert path.read_bytes() == b"old"


def test_get_not_modified_removes_explicit_temporary_file(
    s3_environment, tmp_path
):
    path = tmp_path / "key"
    temporary = tmp_path / "staging"
    path.write_bytes(b"old")
    try:
        for name, value in (
            ("user.s3ar.format", b"1"),
            ("user.s3ar.bucket", b"bucket"),
            ("user.s3ar.key", b"key"),
            ("user.s3ar.etag", b'"old"'),
        ):
            os.setxattr(path, name, value)
    except (AttributeError, OSError) as exc:
        pytest.skip(f"filesystem xattrs unavailable: {exc}")
    before = path.stat()
    steps = [ResponseStep("GET", "/bucket/key", 304,
                          expected_headers=(("If-None-Match", '"old"'),))]
    with FaultServer(steps) as server:
        environment = s3_environment.copy()
        environment["S3AR_ENDPOINT"] = server.endpoint
        result = subprocess.run(
            [str(EXECUTABLE), "-f", str(path), "-t", str(temporary),
             "s3://bucket/key"],
            capture_output=True, env=environment, timeout=10,
        )
    assert result.returncode == 0, result.stderr.decode()
    assert path.read_bytes() == b"old"
    assert path.stat().st_ino == before.st_ino
    assert path.stat().st_mtime_ns == before.st_mtime_ns
    assert not temporary.exists()


def test_get_does_not_emit_bytes_beyond_content_length(s3_environment):
    data = bytes(range(251)) * 101
    extra = b"bytes beyond the declared response body"
    wire_body = data + extra
    steps = [
        ResponseStep(
            "GET",
            "/bucket/key",
            200,
            wire_body,
            headers=(
                ("Content-Length", str(len(data))),
                ("ETag", '"bounded-body"'),
            ),
            absent_headers=("Range", "If-Match"),
            disconnect_after=len(wire_body),
        )
    ]

    with FaultServer(steps) as server:
        environment = s3_environment.copy()
        environment["S3AR_ENDPOINT"] = server.endpoint
        result = subprocess.run(
            [str(EXECUTABLE), "s3://bucket/key"],
            capture_output=True,
            env=environment,
            timeout=10,
        )

    assert result.returncode == 0, result.stderr.decode()
    assert result.stdout == data
    assert extra not in result.stdout
    assert result.stderr == GET_STDOUT_SUCCESS


def test_get_resumes_after_truncated_response(s3_environment):
    data = bytes(range(251)) * 301
    cutoff = 12347
    etag = '"download"'
    steps = [
        ResponseStep(
            "GET",
            "/bucket/key",
            200,
            data,
            headers=(("ETag", etag),),
            absent_headers=("Range", "If-Match"),
            disconnect_after=cutoff,
        ),
        ResponseStep(
            "GET",
            "/bucket/key",
            206,
            data[cutoff:],
            headers=(
                ("ETag", etag),
                ("Content-Range", f"bytes {cutoff}-{len(data) - 1}/{len(data)}"),
            ),
            expected_headers=(
                ("Range", f"bytes={cutoff}-"),
                ("If-Match", etag),
            ),
        ),
    ]

    with FaultServer(steps) as server:
        environment = s3_environment.copy()
        environment["S3AR_ENDPOINT"] = server.endpoint
        result = subprocess.run(
            [str(EXECUTABLE), "s3://bucket/key"],
            capture_output=True,
            env=environment,
            timeout=10,
        )

    assert result.returncode == 0, result.stderr.decode()
    assert result.stdout == data
    assert result.stderr == GET_STDOUT_SUCCESS


@pytest.mark.parametrize("header_name", [
    "Content-Type", "Content-Encoding", "Cache-Control",
])
def test_get_resumes_with_long_property_header(s3_environment, header_name):
    data = bytes(range(251)) * 101
    cutoff = 12347
    etag = '"long-property"'
    value = ("application/x-" + "a" * 1786
             if header_name == "Content-Type" else "a" * 1800)
    property_header = (header_name, value)
    steps = [
        ResponseStep(
            "GET", "/bucket/key", 200, data,
            headers=(("ETag", etag), property_header),
            absent_headers=("Range", "If-Match"),
            disconnect_after=cutoff,
        ),
        ResponseStep(
            "GET", "/bucket/key", 206, data[cutoff:],
            headers=(
                ("ETag", etag), property_header,
                ("Content-Range", f"bytes {cutoff}-{len(data) - 1}/{len(data)}"),
            ),
            expected_headers=(
                ("Range", f"bytes={cutoff}-"),
                ("If-Match", etag),
            ),
        ),
    ]

    with FaultServer(steps) as server:
        environment = s3_environment.copy()
        environment["S3AR_ENDPOINT"] = server.endpoint
        result = subprocess.run(
            [str(EXECUTABLE), "s3://bucket/key"],
            capture_output=True, env=environment, timeout=10,
        )

    assert result.returncode == 0, result.stderr.decode()
    assert result.stdout == data
    assert result.stderr == GET_STDOUT_SUCCESS


def test_get_restarts_after_disconnect_before_first_body_byte(s3_environment):
    initial_data = b"object body from the initial response"
    current_data = b"object body from the current response"
    steps = [
        ResponseStep(
            "GET",
            "/bucket/key",
            200,
            initial_data,
            headers=(("ETag", '"initial-object"'),),
            absent_headers=("Range", "If-Match"),
            disconnect_after=0,
        ),
        ResponseStep(
            "GET",
            "/bucket/key",
            200,
            current_data,
            headers=(("ETag", '"current-object"'),),
            absent_headers=("Range", "If-Match"),
        ),
    ]

    with FaultServer(steps) as server:
        environment = s3_environment.copy()
        environment["S3AR_ENDPOINT"] = server.endpoint
        result = subprocess.run(
            [str(EXECUTABLE), "s3://bucket/key"],
            capture_output=True,
            env=environment,
            timeout=10,
        )

    assert result.returncode == 0, result.stderr.decode()
    assert result.stdout == current_data
    assert initial_data not in result.stdout
    assert result.stderr == GET_STDOUT_SUCCESS


def test_get_retries_slow_down_before_body(s3_environment):
    data = bytes(range(199)) * 101
    error_body = (
        b"<Error><Code>SlowDown</Code>"
        b"<Message>Please reduce your request rate.</Message></Error>"
    )
    etag = '"slow-down"'
    steps = [
        ResponseStep(
            "GET",
            "/bucket/key",
            503,
            error_body,
            headers=(("Retry-After", "0"),),
            absent_headers=("Range", "If-Match"),
        ),
        ResponseStep(
            "GET",
            "/bucket/key",
            200,
            data,
            headers=(("ETag", etag),),
            absent_headers=("Range", "If-Match"),
        ),
    ]

    with FaultServer(steps) as server:
        environment = s3_environment.copy()
        environment["S3AR_ENDPOINT"] = server.endpoint
        result = subprocess.run(
            [str(EXECUTABLE), "s3://bucket/key"],
            capture_output=True,
            env=environment,
            timeout=10,
        )

    assert result.returncode == 0, result.stderr.decode()
    assert result.stdout == data
    assert error_body not in result.stdout
    assert result.stderr == GET_STDOUT_SUCCESS


def test_get_trace_records_requests_and_retry_without_payload(s3_environment):
    payload = b"private-body-must-stay-out-of-trace"
    steps = [
        ResponseStep("GET", "/bucket/key", 503,
                     b"<Error><Code>SlowDown</Code></Error>",
                     headers=(("Retry-After", "0"),)),
        ResponseStep("GET", "/bucket/key", 200, payload,
                     headers=(("ETag", '"trace"'),
                              ("x-amz-request-id", "trace-request-123"))),
    ]
    with FaultServer(steps) as server:
        environment = s3_environment.copy()
        environment["S3AR_ENDPOINT"] = server.endpoint
        environment["S3AR_SESSION_TOKEN"] = "private-session-token"
        result = subprocess.run(
            [str(EXECUTABLE), "-vvv", "s3://bucket/key"],
            capture_output=True, env=environment, timeout=10,
        )

    assert result.returncode == 0, result.stderr.decode()
    assert result.stdout == payload
    assert result.stderr.count(b"http id=") == 2
    assert result.stderr.count(b"http result id=") == 2
    assert result.stderr.count(b"retry id=") == 1
    assert b"status=503" in result.stderr
    assert b"status=200" in result.stderr
    assert b"request_id=trace-request-123" in result.stderr
    assert b" received=" in result.stderr
    assert b" request_id=trace-request-123" in result.stderr
    assert payload not in result.stderr
    assert b"private-session-token" not in result.stderr


def test_get_retries_slow_down_before_empty_object(s3_environment):
    error_body = b"<Error><Code>SlowDown</Code><Message>Retry later.</Message></Error>"
    steps = [
        ResponseStep(
            "GET",
            "/bucket/key",
            503,
            error_body,
            headers=(("Retry-After", "0"),),
            absent_headers=("Range", "If-Match"),
        ),
        ResponseStep(
            "GET",
            "/bucket/key",
            200,
            b"",
            headers=(
                ("ETag", '"empty-object"'),
                ("x-amz-meta-source", "empty-retry-test"),
            ),
            absent_headers=("Range", "If-Match"),
        ),
    ]

    with FaultServer(steps) as server:
        environment = s3_environment.copy()
        environment["S3AR_ENDPOINT"] = server.endpoint
        result = subprocess.run(
            [str(EXECUTABLE), "s3://bucket/key"],
            capture_output=True,
            env=environment,
            timeout=10,
        )

    assert result.returncode == 0, result.stderr.decode()
    assert result.stdout == b""
    assert result.stderr == GET_STDOUT_SUCCESS


def test_get_discards_large_retryable_error_body(s3_environment):
    data = b"object data after a large error response"
    error_body = (
        b"<Error><Code>SlowDown</Code><Message>"
        + b"x" * (128 * 1024)
        + b"</Message></Error>"
    )
    steps = [
        ResponseStep(
            "GET",
            "/bucket/key",
            503,
            error_body,
            headers=(("Retry-After", "0"),),
            absent_headers=("Range", "If-Match"),
        ),
        ResponseStep(
            "GET",
            "/bucket/key",
            200,
            data,
            headers=(("ETag", '"after-large-error"'),),
            absent_headers=("Range", "If-Match"),
        ),
    ]

    with FaultServer(steps) as server:
        environment = s3_environment.copy()
        environment["S3AR_ENDPOINT"] = server.endpoint
        result = subprocess.run(
            [str(EXECUTABLE), "s3://bucket/key"],
            capture_output=True,
            env=environment,
            timeout=10,
        )

    assert result.returncode == 0, result.stderr.decode()
    assert result.stdout == data
    assert error_body[: 64 * 1024] not in result.stdout
    assert result.stderr == GET_STDOUT_SUCCESS


def test_resumed_get_retries_slow_down(s3_environment):
    data = bytes(range(241)) * 313
    cutoff = 23459
    error_body = b"<Error><Code>SlowDown</Code><Message>Retry later.</Message></Error>"
    etag = '"resumed-slow-down"'
    resumed_headers = (
        ("Range", f"bytes={cutoff}-"),
        ("If-Match", etag),
    )
    steps = [
        ResponseStep(
            "GET",
            "/bucket/key",
            200,
            data,
            headers=(("ETag", etag),),
            absent_headers=("Range", "If-Match"),
            disconnect_after=cutoff,
        ),
        ResponseStep(
            "GET",
            "/bucket/key",
            503,
            error_body,
            headers=(("Retry-After", "0"),),
            expected_headers=resumed_headers,
        ),
        ResponseStep(
            "GET",
            "/bucket/key",
            206,
            data[cutoff:],
            headers=(
                ("ETag", etag),
                ("Content-Range", f"bytes {cutoff}-{len(data) - 1}/{len(data)}"),
            ),
            expected_headers=resumed_headers,
        ),
    ]

    with FaultServer(steps) as server:
        environment = s3_environment.copy()
        environment["S3AR_ENDPOINT"] = server.endpoint
        result = subprocess.run(
            [str(EXECUTABLE), "s3://bucket/key"],
            capture_output=True,
            env=environment,
            timeout=10,
        )

    assert result.returncode == 0, result.stderr.decode()
    assert result.stdout == data
    assert error_body not in result.stdout
    assert result.stderr == GET_STDOUT_SUCCESS


@pytest.mark.parametrize(
    ("status", "s3_code"),
    [
        pytest.param(408, "TemporaryFailure", id="http-408"),
        pytest.param(429, "TemporaryFailure", id="http-429"),
        pytest.param(500, "TemporaryFailure", id="http-500"),
        pytest.param(502, "TemporaryFailure", id="http-502"),
        pytest.param(504, "TemporaryFailure", id="http-504"),
        pytest.param(400, "InternalError", id="s3-internal-error"),
        pytest.param(400, "RequestTimeout", id="s3-request-timeout"),
        pytest.param(400, "ServiceUnavailable", id="s3-service-unavailable"),
    ],
)
def test_get_retries_temporary_s3_errors(s3_environment, status, s3_code):
    data = b"object data after a temporary S3 error"
    error_body = (
        f"<Error><Code>{s3_code}</Code><Message>Retry later.</Message></Error>"
    ).encode()
    steps = [
        ResponseStep(
            "GET",
            "/bucket/key",
            status,
            error_body,
            headers=(("Retry-After", "0"),),
            absent_headers=("Range", "If-Match"),
        ),
        ResponseStep(
            "GET",
            "/bucket/key",
            200,
            data,
            headers=(("ETag", '"temporary-error"'),),
            absent_headers=("Range", "If-Match"),
        ),
    ]

    with FaultServer(steps) as server:
        environment = s3_environment.copy()
        environment["S3AR_ENDPOINT"] = server.endpoint
        result = subprocess.run(
            [str(EXECUTABLE), "s3://bucket/key"],
            capture_output=True,
            env=environment,
            timeout=10,
        )

    assert result.returncode == 0, result.stderr.decode()
    assert result.stdout == data
    assert error_body not in result.stdout
    assert result.stderr == GET_STDOUT_SUCCESS


def test_get_reports_exhausted_slow_down_retries(s3_environment):
    error_body = (
        b"<Error><Code>SlowDown</Code><Message>Reduce the request rate.</Message>"
        b"<RequestId>slow-request</RequestId></Error>"
    )
    steps = [
        ResponseStep(
            "GET",
            "/bucket/key",
            503,
            error_body,
            headers=(("Retry-After", "0"),),
            absent_headers=("Range", "If-Match"),
        ),
        ResponseStep(
            "GET",
            "/bucket/key",
            503,
            error_body,
            headers=(("Retry-After", "0"),),
            absent_headers=("Range", "If-Match"),
        ),
    ]

    with FaultServer(steps) as server:
        environment = s3_environment.copy()
        environment["S3AR_ENDPOINT"] = server.endpoint
        result = subprocess.run(
            [str(RETRY_PROBE)],
            capture_output=True,
            env=environment,
            timeout=10,
        )

    assert result.returncode == 1
    assert result.stdout == b""
    assert b"result=retry exhausted" in result.stderr
    assert b"attempts=2" in result.stderr
    assert b"http=503" in result.stderr
    assert b"s3=SlowDown" in result.stderr
    assert b"message=S3 GET retry limit exhausted" in result.stderr


def test_get_does_not_retry_output_callback_failure(s3_environment):
    data = bytes(range(211)) * 97
    steps = [
        ResponseStep(
            "GET",
            "/bucket/key",
            200,
            data,
            headers=(("ETag", '"callback-failure"'),),
            absent_headers=("Range", "If-Match"),
        )
    ]

    with FaultServer(steps) as server:
        environment = s3_environment.copy()
        environment["S3AR_ENDPOINT"] = server.endpoint
        result = subprocess.run(
            [str(RETRY_PROBE), "fail-write"],
            capture_output=True,
            env=environment,
            timeout=10,
        )

    assert result.returncode == 1
    assert result.stdout == b""
    assert b"result=callback error" in result.stderr
    assert b"attempts=1" in result.stderr
    assert b"http=200" in result.stderr
    assert f"callback={errno.ENOSPC}".encode() in result.stderr
    assert b"message=output callback failed" in result.stderr


@pytest.mark.parametrize(
    "invalid_field",
    [
        "status",
        "missing-content-range",
        "range-start",
        "range-total",
        "range-end",
        "short-range",
        "content-length",
        "etag",
        "last-modified",
        "content-type",
        "content-encoding",
        "cache-control",
        "metadata-value",
        "metadata-missing",
        "metadata-extra",
    ],
)
def test_get_rejects_inconsistent_resumed_response(
    s3_environment, tmp_path, invalid_field
):
    data = bytes(range(233)) * 211
    cutoff = 15731
    etag = '"consistent-object"'
    modified = "Thu, 03 Sep 2026 12:00:00 GMT"
    initial_headers = (
        ("ETag", etag),
        ("Last-Modified", modified),
        ("Content-Type", "application/octet-stream"),
        ("Content-Encoding", "identity"),
        ("Cache-Control", "no-cache"),
        ("x-amz-meta-source", "original"),
    )
    resumed_headers = dict(initial_headers)
    resumed_headers["Content-Range"] = (
        f"bytes {cutoff}-{len(data) - 1}/{len(data)}"
    )
    status = 206

    if invalid_field == "status":
        status = 200
    elif invalid_field == "missing-content-range":
        del resumed_headers["Content-Range"]
    elif invalid_field == "range-start":
        resumed_headers["Content-Range"] = (
            f"bytes {cutoff + 1}-{len(data) - 1}/{len(data)}"
        )
    elif invalid_field == "range-total":
        resumed_headers["Content-Range"] = (
            f"bytes {cutoff}-{len(data) - 1}/{len(data) + 1}"
        )
    elif invalid_field in ("range-end", "short-range"):
        resumed_headers["Content-Range"] = (
            f"bytes {cutoff}-{len(data) - 2}/{len(data)}"
        )
        if invalid_field == "short-range":
            resumed_headers["Content-Length"] = str(len(data) - cutoff - 1)
    elif invalid_field == "content-length":
        resumed_headers["Content-Length"] = str(len(data) - cutoff + 1)
    elif invalid_field == "etag":
        resumed_headers["ETag"] = '"changed-object"'
    elif invalid_field == "last-modified":
        resumed_headers["Last-Modified"] = "Fri, 04 Sep 2026 12:00:00 GMT"
    elif invalid_field == "content-type":
        resumed_headers["Content-Type"] = "text/plain"
    elif invalid_field == "content-encoding":
        resumed_headers["Content-Encoding"] = "gzip"
    elif invalid_field == "cache-control":
        resumed_headers["Cache-Control"] = "max-age=60"
    elif invalid_field == "metadata-value":
        resumed_headers["x-amz-meta-source"] = "changed"
    elif invalid_field == "metadata-missing":
        del resumed_headers["x-amz-meta-source"]
    elif invalid_field == "metadata-extra":
        resumed_headers["x-amz-meta-extra"] = "added"

    steps = [
        ResponseStep(
            "GET",
            "/bucket/key",
            200,
            data,
            headers=initial_headers,
            absent_headers=("Range", "If-Match"),
            disconnect_after=cutoff,
        ),
        ResponseStep(
            "GET",
            "/bucket/key",
            status,
            data[cutoff:],
            headers=tuple(resumed_headers.items()),
            expected_headers=(
                ("Range", f"bytes={cutoff}-"),
                ("If-Match", etag),
            ),
        ),
    ]
    destination = tmp_path / f"invalid-{invalid_field}.bin"
    temporary = tmp_path / f"{destination.name}.tmp"
    original = b"existing destination"
    destination.write_bytes(original)

    with FaultServer(steps) as server:
        environment = s3_environment.copy()
        environment["S3AR_ENDPOINT"] = server.endpoint
        result = subprocess.run(
            [
                str(EXECUTABLE),
                "-f",
                str(destination),
                "-t",
                str(temporary),
                "s3://bucket/key",
            ],
            capture_output=True,
            env=environment,
            timeout=10,
        )

    assert result.returncode == 2
    assert result.stdout == b""
    assert b"invalid resumed GET" in result.stderr
    assert destination.read_bytes() == original
    assert not temporary.exists()


def test_get_reports_object_change_during_resume(s3_environment, tmp_path):
    data = bytes(range(227)) * 197
    cutoff = 11239
    etag = '"original-object"'
    error_body = (
        b"<Error><Code>PreconditionFailed</Code>"
        b"<Message>The object changed.</Message></Error>"
    )
    steps = [
        ResponseStep(
            "GET",
            "/bucket/key",
            200,
            data,
            headers=(("ETag", etag),),
            absent_headers=("Range", "If-Match"),
            disconnect_after=cutoff,
        ),
        ResponseStep(
            "GET",
            "/bucket/key",
            412,
            error_body,
            expected_headers=(
                ("Range", f"bytes={cutoff}-"),
                ("If-Match", etag),
            ),
        ),
    ]
    destination = tmp_path / "changed-object.bin"
    temporary = tmp_path / f"{destination.name}.tmp"
    original = b"existing destination"
    destination.write_bytes(original)

    with FaultServer(steps) as server:
        environment = s3_environment.copy()
        environment["S3AR_ENDPOINT"] = server.endpoint
        result = subprocess.run(
            [
                str(EXECUTABLE),
                "-f",
                str(destination),
                "-t",
                str(temporary),
                "s3://bucket/key",
            ],
            capture_output=True,
            env=environment,
            timeout=10,
        )

    assert result.returncode == 2
    assert result.stdout == b""
    assert b"object changed during download" in result.stderr
    assert b"S3 PreconditionFailed" in result.stderr
    assert b"HTTP 412" in result.stderr
    assert b"after 2 attempts" in result.stderr
    assert destination.read_bytes() == original
    assert not temporary.exists()


def test_get_advances_range_across_multiple_interruptions(s3_environment):
    data = bytes(range(229)) * 401
    first_cutoff = 14317
    second_cutoff = 37691
    etag = '"multiple-interruptions"'
    first_resumed_headers = (
        ("Range", f"bytes={first_cutoff}-"),
        ("If-Match", etag),
    )
    second_resumed_headers = (
        ("Range", f"bytes={second_cutoff}-"),
        ("If-Match", etag),
    )
    steps = [
        ResponseStep(
            "GET",
            "/bucket/key",
            200,
            data,
            headers=(("ETag", etag),),
            absent_headers=("Range", "If-Match"),
            disconnect_after=first_cutoff,
        ),
        ResponseStep(
            "GET",
            "/bucket/key",
            206,
            data[first_cutoff:],
            headers=(
                ("ETag", etag),
                (
                    "Content-Range",
                    f"bytes {first_cutoff}-{len(data) - 1}/{len(data)}",
                ),
            ),
            expected_headers=first_resumed_headers,
            disconnect_after=second_cutoff - first_cutoff,
        ),
        ResponseStep(
            "GET",
            "/bucket/key",
            206,
            data[second_cutoff:],
            headers=(
                ("ETag", etag),
                (
                    "Content-Range",
                    f"bytes {second_cutoff}-{len(data) - 1}/{len(data)}",
                ),
            ),
            expected_headers=second_resumed_headers,
        ),
    ]

    with FaultServer(steps) as server:
        environment = s3_environment.copy()
        environment["S3AR_ENDPOINT"] = server.endpoint
        result = subprocess.run(
            [str(EXECUTABLE), "s3://bucket/key"],
            capture_output=True,
            env=environment,
            timeout=10,
        )

    assert result.returncode == 0, result.stderr.decode()
    assert result.stdout == data
    assert result.stderr == GET_STDOUT_SUCCESS


def test_get_accepts_reordered_metadata_during_resume(s3_environment):
    data = bytes(range(217)) * 263
    cutoff = 17389
    etag = '"reordered-metadata"'
    steps = [
        ResponseStep(
            "GET",
            "/bucket/key",
            200,
            data,
            headers=(
                ("ETag", etag),
                ("x-amz-meta-zeta", "last"),
                ("x-amz-meta-alpha", "first"),
            ),
            absent_headers=("Range", "If-Match"),
            disconnect_after=cutoff,
        ),
        ResponseStep(
            "GET",
            "/bucket/key",
            206,
            data[cutoff:],
            headers=(
                ("ETag", etag),
                ("Content-Range", f"bytes {cutoff}-{len(data) - 1}/{len(data)}"),
                ("x-amz-meta-alpha", "first"),
                ("x-amz-meta-zeta", "last"),
            ),
            expected_headers=(
                ("Range", f"bytes={cutoff}-"),
                ("If-Match", etag),
            ),
        ),
    ]

    with FaultServer(steps) as server:
        environment = s3_environment.copy()
        environment["S3AR_ENDPOINT"] = server.endpoint
        result = subprocess.run(
            [str(EXECUTABLE), "s3://bucket/key"],
            capture_output=True,
            env=environment,
            timeout=10,
        )

    assert result.returncode == 0, result.stderr.decode()
    assert result.stdout == data
    assert result.stderr == GET_STDOUT_SUCCESS


def test_get_accepts_reordered_duplicate_metadata_during_resume(
    s3_environment,
):
    data = bytes(range(193)) * 271
    cutoff = 16411
    etag = '"duplicate-metadata"'
    steps = [
        ResponseStep(
            "GET",
            "/bucket/key",
            200,
            data,
            headers=(
                ("ETag", etag),
                ("x-amz-meta-label", "beta"),
                ("x-amz-meta-label", "alpha"),
            ),
            absent_headers=("Range", "If-Match"),
            disconnect_after=cutoff,
        ),
        ResponseStep(
            "GET",
            "/bucket/key",
            206,
            data[cutoff:],
            headers=(
                ("ETag", etag),
                ("Content-Range", f"bytes {cutoff}-{len(data) - 1}/{len(data)}"),
                ("x-amz-meta-label", "alpha"),
                ("x-amz-meta-label", "beta"),
            ),
            expected_headers=(
                ("Range", f"bytes={cutoff}-"),
                ("If-Match", etag),
            ),
        ),
    ]

    with FaultServer(steps) as server:
        environment = s3_environment.copy()
        environment["S3AR_ENDPOINT"] = server.endpoint
        result = subprocess.run(
            [str(EXECUTABLE), "s3://bucket/key"],
            capture_output=True,
            env=environment,
            timeout=10,
        )

    assert result.returncode == 0, result.stderr.decode()
    assert result.stdout == data
    assert result.stderr == GET_STDOUT_SUCCESS


def test_get_accepts_metadata_limit_during_resume(s3_environment):
    data = bytes(range(191)) * 277
    cutoff = 18109
    etag = '"metadata-limit"'
    metadata = tuple(
        (f"x-amz-meta-item-{index:03d}", f"value-{index:03d}")
        for index in range(128)
    )
    steps = [
        ResponseStep(
            "GET",
            "/bucket/key",
            200,
            data,
            headers=(("ETag", etag),) + metadata,
            absent_headers=("Range", "If-Match"),
            disconnect_after=cutoff,
        ),
        ResponseStep(
            "GET",
            "/bucket/key",
            206,
            data[cutoff:],
            headers=(
                ("ETag", etag),
                ("Content-Range", f"bytes {cutoff}-{len(data) - 1}/{len(data)}"),
            )
            + tuple(reversed(metadata)),
            expected_headers=(
                ("Range", f"bytes={cutoff}-"),
                ("If-Match", etag),
            ),
        ),
    ]

    with FaultServer(steps) as server:
        environment = s3_environment.copy()
        environment["S3AR_ENDPOINT"] = server.endpoint
        result = subprocess.run(
            [str(EXECUTABLE), "s3://bucket/key"],
            capture_output=True,
            env=environment,
            timeout=10,
        )

    assert result.returncode == 0, result.stderr.decode()
    assert result.stdout == data
    assert result.stderr == GET_STDOUT_SUCCESS


def test_get_rejects_metadata_over_limit(s3_environment, tmp_path):
    data = b"object with too many metadata fields"
    metadata = tuple(
        (f"x-amz-meta-item-{index:03d}", f"value-{index:03d}")
        for index in range(129)
    )
    steps = [
        ResponseStep(
            "GET",
            "/bucket/key",
            200,
            data,
            headers=(("ETag", '"too-many-metadata"'),) + metadata,
            absent_headers=("Range", "If-Match"),
        )
    ]
    destination = tmp_path / "too-many-metadata.bin"
    temporary = tmp_path / f"{destination.name}.tmp"
    original = b"existing destination"
    destination.write_bytes(original)

    with FaultServer(steps) as server:
        environment = s3_environment.copy()
        environment["S3AR_ENDPOINT"] = server.endpoint
        result = subprocess.run(
            [
                str(EXECUTABLE),
                "-f",
                str(destination),
                "-t",
                str(temporary),
                "s3://bucket/key",
            ],
            capture_output=True,
            env=environment,
            timeout=10,
        )

    assert result.returncode == 2
    assert result.stdout == b""
    assert b"invalid or oversized GET response headers" in result.stderr
    assert b"after" not in result.stderr
    assert destination.read_bytes() == original
    assert not temporary.exists()


def test_get_does_not_retry_missing_object_during_resume(
    s3_environment, tmp_path
):
    data = bytes(range(223)) * 191
    cutoff = 10429
    etag = '"deleted-object"'
    error_body = (
        b"<Error><Code>NoSuchKey</Code>"
        b"<Message>The object no longer exists.</Message></Error>"
    )
    steps = [
        ResponseStep(
            "GET",
            "/bucket/key",
            200,
            data,
            headers=(("ETag", etag),),
            absent_headers=("Range", "If-Match"),
            disconnect_after=cutoff,
        ),
        ResponseStep(
            "GET",
            "/bucket/key",
            404,
            error_body,
            expected_headers=(
                ("Range", f"bytes={cutoff}-"),
                ("If-Match", etag),
            ),
        ),
    ]
    destination = tmp_path / "deleted-object.bin"
    temporary = tmp_path / f"{destination.name}.tmp"
    original = b"existing destination"
    destination.write_bytes(original)

    with FaultServer(steps) as server:
        environment = s3_environment.copy()
        environment["S3AR_ENDPOINT"] = server.endpoint
        result = subprocess.run(
            [
                str(EXECUTABLE),
                "-f",
                str(destination),
                "-t",
                str(temporary),
                "s3://bucket/key",
            ],
            capture_output=True,
            env=environment,
            timeout=10,
        )

    assert result.returncode == 2
    assert result.stdout == b""
    assert b"The object no longer exists." in result.stderr
    assert b"S3 NoSuchKey" in result.stderr
    assert b"HTTP 404" in result.stderr
    assert b"after 2 attempts" in result.stderr
    assert destination.read_bytes() == original
    assert not temporary.exists()


@pytest.mark.parametrize(
    ("status", "s3_code"),
    [
        pytest.param(400, "InvalidRequest", id="bad-request"),
        pytest.param(403, "AccessDenied", id="access-denied"),
        pytest.param(404, "NoSuchKey", id="not-found"),
    ],
)
def test_get_does_not_retry_permanent_s3_errors(
    s3_environment, tmp_path, status, s3_code
):
    error_body = (
        f"<Error><Code>{s3_code}</Code><Message>Permanent failure.</Message>"
        f"</Error>"
    ).encode()
    steps = [
        ResponseStep(
            "GET",
            "/bucket/key",
            status,
            error_body,
            absent_headers=("Range", "If-Match"),
        )
    ]
    destination = tmp_path / f"permanent-{status}.bin"
    temporary = tmp_path / f"{destination.name}.tmp"
    original = b"existing destination"
    destination.write_bytes(original)

    with FaultServer(steps) as server:
        environment = s3_environment.copy()
        environment["S3AR_ENDPOINT"] = server.endpoint
        result = subprocess.run(
            [
                str(EXECUTABLE),
                "-f",
                str(destination),
                "-t",
                str(temporary),
                "s3://bucket/key",
            ],
            capture_output=True,
            env=environment,
            timeout=10,
        )

    assert result.returncode == 2
    assert result.stdout == b""
    assert b"Permanent failure." in result.stderr
    assert f"S3 {s3_code}".encode() in result.stderr
    assert f"HTTP {status}".encode() in result.stderr
    assert b"after" not in result.stderr
    assert destination.read_bytes() == original
    assert not temporary.exists()


@pytest.mark.parametrize(
    "invalid_field",
    [
        "missing-content-length",
        "invalid-content-length",
        "missing-etag",
        "initial-partial-response",
    ],
)
def test_get_rejects_invalid_initial_response(
    s3_environment, tmp_path, invalid_field
):
    data = b"body from an invalid initial response"
    headers = {"ETag": '"initial-object"'}
    status = 200
    auto_content_length = True
    disconnect_after = None

    if invalid_field == "missing-content-length":
        auto_content_length = False
        disconnect_after = len(data)
    elif invalid_field == "invalid-content-length":
        headers["Content-Length"] = "invalid"
        disconnect_after = len(data)
    elif invalid_field == "missing-etag":
        del headers["ETag"]
    elif invalid_field == "initial-partial-response":
        status = 206
        headers["Content-Range"] = f"bytes 0-{len(data) - 1}/{len(data)}"

    steps = [
        ResponseStep(
            "GET",
            "/bucket/key",
            status,
            data,
            headers=tuple(headers.items()),
            absent_headers=("Range", "If-Match"),
            disconnect_after=disconnect_after,
            auto_content_length=auto_content_length,
        )
    ]
    destination = tmp_path / f"invalid-initial-{invalid_field}.bin"
    temporary = tmp_path / f"{destination.name}.tmp"
    original = b"existing destination"
    destination.write_bytes(original)

    with FaultServer(steps) as server:
        environment = s3_environment.copy()
        environment["S3AR_ENDPOINT"] = server.endpoint
        result = subprocess.run(
            [
                str(EXECUTABLE),
                "-f",
                str(destination),
                "-t",
                str(temporary),
                "s3://bucket/key",
            ],
            capture_output=True,
            env=environment,
            timeout=10,
        )

    assert result.returncode == 2
    assert result.stdout == b""
    expected_error = (
        b"Invalid Content-Length"
        if invalid_field == "invalid-content-length"
        else b"GET response lacks Content-Length or ETag"
    )
    assert expected_error in result.stderr
    assert b"after" not in result.stderr
    assert destination.read_bytes() == original
    assert not temporary.exists()


def test_get_writes_multipart_file(s3_environment, tmp_path):
    data = b"x" * PART_SIZE + b"last part"
    destination = tmp_path / "download.bin"
    server, thread, environment = run_server(s3_environment, data)
    try:
        result = subprocess.run(
            [str(EXECUTABLE), "-f", str(destination), "s3://bucket/key"],
            capture_output=True,
            env=environment,
            timeout=30,
        )
    finally:
        stop_server(server, thread)

    assert result.returncode == 0, result.stderr.decode()
    assert result.stdout == b""
    assert destination.read_bytes() == data
    assert list(tmp_path.glob("download.bin.tmp.*")) == []


@pytest.mark.parametrize("process_umask", [0o000, 0o022, 0o077])
def test_get_new_file_uses_temporary_mode(
    s3_environment, tmp_path, process_umask
):
    destination = tmp_path / "new.bin"
    server, thread, environment = run_server(s3_environment, b"new data")
    try:
        result = subprocess.run(
            [str(EXECUTABLE), "-f", str(destination), "s3://bucket/key"],
            capture_output=True,
            env=environment,
            timeout=30,
            umask=process_umask,
        )
    finally:
        stop_server(server, thread)

    assert result.returncode == 0, result.stderr.decode()
    assert stat.S_IMODE(destination.stat().st_mode) == 0o600


def test_get_overwrite_uses_temporary_mode(s3_environment, tmp_path):
    destination = tmp_path / "existing.bin"
    destination.write_bytes(b"old data")
    destination.chmod(0o640)
    server, thread, environment = run_server(s3_environment, b"new data")
    try:
        result = subprocess.run(
            [str(EXECUTABLE), "-f", str(destination), "s3://bucket/key"],
            capture_output=True,
            env=environment,
            timeout=30,
        )
    finally:
        stop_server(server, thread)

    assert result.returncode == 0, result.stderr.decode()
    assert destination.read_bytes() == b"new data"
    assert stat.S_IMODE(destination.stat().st_mode) == 0o600
