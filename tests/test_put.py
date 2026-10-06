import os
import signal
import subprocess
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import threading
import time
import urllib.parse

import pytest

from fault_server import FaultServer, ResponseStep


EXECUTABLE = Path(__file__).parents[1] / "s3ar-put"

UPLOAD_ID_XML = (
    b"<InitiateMultipartUploadResult><UploadId>review-upload</UploadId>"
    b"</InitiateMultipartUploadResult>"
)
COMPLETE_XML = b"<CompleteMultipartUploadResult/>"
UPLOAD_PATH = "/bucket/key?uploadId=review-upload"
DISCONNECT = (("Connection", "close"),)
MULTIPART_DATA = b"x" * (16 * 1024 * 1024) + b"last part"


def multipart_steps(completion_steps, abort=False):
    steps = [
        ResponseStep("POST", "/bucket/key?uploads", 200, UPLOAD_ID_XML,
                     DISCONNECT),
        ResponseStep("PUT", "/bucket/key?partNumber=1&uploadId=review-upload",
                     200, headers=DISCONNECT + (("ETag", '"part"'),)),
        ResponseStep("PUT", "/bucket/key?partNumber=2&uploadId=review-upload",
                     200, headers=DISCONNECT + (("ETag", '"part"'),)),
        *completion_steps,
    ]
    if abort:
        steps.append(ResponseStep("DELETE", UPLOAD_PATH, 204,
                                  headers=DISCONNECT))
    return steps


@pytest.mark.parametrize("multipart_started", [False, True])
@pytest.mark.parametrize("interrupt_signal", [signal.SIGINT, signal.SIGTERM])
def test_interrupted_put_waiting_for_stdin_aborts_upload(
    s3_environment, interrupt_signal, multipart_started
):
    steps = [
        ResponseStep("POST", "/bucket/key?uploads", 200, UPLOAD_ID_XML,
                     DISCONNECT),
        ResponseStep("PUT", "/bucket/key?partNumber=1&uploadId=review-upload",
                     200, headers=DISCONNECT + (("ETag", '"part"'),)),
        ResponseStep("DELETE", UPLOAD_PATH, 204, headers=DISCONNECT),
    ] if multipart_started else []
    with FaultServer(steps) as server:
        environment = {**s3_environment, "S3AR_ENDPOINT": server.endpoint}
        process = subprocess.Popen(
            [str(EXECUTABLE), "s3://bucket/key"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, env=environment,
        )
        try:
            if multipart_started:
                process.stdin.write(b"x" * (16 * 1024 * 1024 + 1))
                process.stdin.flush()
                deadline = time.monotonic() + 5
                while len(server.requests) < 2 and time.monotonic() < deadline:
                    time.sleep(0.01)
                assert len(server.requests) == 2
            time.sleep(0.2)
            process.send_signal(interrupt_signal)
            process.wait(timeout=10)
            stderr = process.stderr.read()
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            process.stdin.close()
            process.stdout.close()
            process.stderr.close()

    assert process.returncode == 2
    assert b"interrupted" in stderr
    assert [request.method for request in server.requests] == (
        ["POST", "PUT", "DELETE"] if multipart_started else []
    )


@pytest.mark.parametrize("first_response", [
    ResponseStep("POST", UPLOAD_PATH, 503,
                 b"<Error><Code>SlowDown</Code></Error>", DISCONNECT),
    ResponseStep("POST", UPLOAD_PATH, 200,
                 b"<Error><Code>SlowDown</Code></Error>", DISCONNECT),
])
def test_put_retries_multipart_completion(s3_environment, first_response):
    steps = multipart_steps([
        first_response,
        ResponseStep("POST", UPLOAD_PATH, 200, COMPLETE_XML, DISCONNECT),
    ])
    with FaultServer(steps) as server:
        environment = {**s3_environment, "S3AR_ENDPOINT": server.endpoint}
        result = run_put(EXECUTABLE, environment, "s3://bucket/key", data=MULTIPART_DATA)

    assert result.returncode == 0, result.stderr.decode()


def test_put_reports_uncertain_completion_without_abort(s3_environment):
    steps = multipart_steps([
        ResponseStep("POST", UPLOAD_PATH, 200, COMPLETE_XML, DISCONNECT,
                     disconnect_after=0),
        ResponseStep("POST", UPLOAD_PATH, 404,
                     b"<Error><Code>NoSuchUpload</Code></Error>", DISCONNECT),
    ])
    with FaultServer(steps) as server:
        environment = {**s3_environment, "S3AR_ENDPOINT": server.endpoint}
        result = run_put(EXECUTABLE, environment, "s3://bucket/key", data=MULTIPART_DATA)

    assert result.returncode == 2
    assert b"completion outcome uncertain" in result.stderr


def test_put_aborts_definitive_completion_failure(s3_environment):
    steps = multipart_steps([
        ResponseStep("POST", UPLOAD_PATH, 200,
                     b"<Error><Code>AccessDenied</Code></Error>", DISCONNECT),
    ], abort=True)
    with FaultServer(steps) as server:
        environment = {**s3_environment, "S3AR_ENDPOINT": server.endpoint}
        result = run_put(EXECUTABLE, environment, "s3://bucket/key", data=MULTIPART_DATA)

    assert result.returncode == 2
    assert b"AccessDenied" in result.stderr


def test_put_aborts_when_part_response_lacks_etag(s3_environment):
    steps = [
        ResponseStep("POST", "/bucket/key?uploads", 200, UPLOAD_ID_XML,
                     DISCONNECT),
        ResponseStep("PUT", "/bucket/key?partNumber=1&uploadId=review-upload",
                     200, headers=DISCONNECT),
        ResponseStep("DELETE", UPLOAD_PATH, 204, headers=DISCONNECT),
    ]
    with FaultServer(steps) as server:
        result = run_put(
            EXECUTABLE, {**s3_environment, "S3AR_ENDPOINT": server.endpoint},
            "s3://bucket/key", data=MULTIPART_DATA,
        )

    assert result.returncode == 2
    assert b"UploadPart response lacks ETag" in result.stderr


def run_put(
    executable, environment, destination, data=None, path=None,
    multipart_size=None, verbosity=0, create_bucket=False,
):
    command = [str(executable)]
    command += ["-v"] * verbosity
    if create_bucket:
        command += ["--create-bucket"]
    if path is not None:
        command += ["-f", str(path)]
    if multipart_size is not None:
        command += ["--multipart-size", multipart_size]
    command.append(destination)
    return subprocess.run(
        command,
        input=data,
        capture_output=True,
        env=environment,
        timeout=30,
    )


def test_put_trace_reports_content_length_without_carriage_return(s3_environment):
    steps = [ResponseStep("PUT", "/bucket/key", 200, headers=DISCONNECT)]
    with FaultServer(steps) as server:
        environment = {**s3_environment, "S3AR_ENDPOINT": server.endpoint}
        result = run_put(
            EXECUTABLE, environment, "s3://bucket/key", data=b"data",
            verbosity=3,
        )

    assert result.returncode == 0, result.stderr.decode()
    assert server.requests[0].headers["content-length"] == "4"
    traces = [line for line in result.stderr.splitlines() if b": trace:" in line]
    part_trace = next(line for line in traces if b" PUT " in line)
    assert b"content_length=4" in part_trace.split()


@pytest.mark.parametrize("region", ["us-east-1", "eu-central-1"])
def test_put_creates_missing_bucket(s3_server, s3_environment, region):
    _endpoint, client = s3_server
    bucket = f"put-create-{region}"
    environment = {**s3_environment, "S3AR_REGION": region}
    result = run_put(
        EXECUTABLE, environment, f"s3://{bucket}/key", data=b"created",
        create_bucket=True, verbosity=2,
    )

    assert result.returncode == 0, result.stderr.decode()
    assert b"(option --create-bucket) create-bucket = 'true'" in result.stderr
    assert client.get_object(Bucket=bucket, Key="key")["Body"].read() == b"created"
    location = client.get_bucket_location(Bucket=bucket)["LocationConstraint"]
    assert location == (None if region == "us-east-1" else region)


def test_put_uses_existing_bucket_without_creating(s3_environment):
    steps = [
        ResponseStep("HEAD", "/bucket", 200, headers=DISCONNECT),
        ResponseStep("PUT", "/bucket/key", 200, headers=DISCONNECT),
    ]
    with FaultServer(steps) as server:
        environment = {**s3_environment, "S3AR_ENDPOINT": server.endpoint}
        result = run_put(
            EXECUTABLE, environment, "s3://bucket/key", data=b"data",
            create_bucket=True,
        )

    assert result.returncode == 0, result.stderr.decode()


def test_put_missing_bucket_without_create_option(s3_environment):
    steps = [
        ResponseStep("PUT", "/bucket/key", 404,
                     b"<Error><Code>NoSuchBucket</Code></Error>", DISCONNECT),
    ]
    with FaultServer(steps) as server:
        environment = {**s3_environment, "S3AR_ENDPOINT": server.endpoint}
        result = run_put(EXECUTABLE, environment, "s3://bucket/key", data=b"data")

    assert result.returncode == 2
    assert b"upload failed for s3://bucket/key:" in result.stderr
    assert b"NoSuchBucket" in result.stderr


@pytest.mark.parametrize("steps", [
    [ResponseStep("HEAD", "/bucket", 403, headers=DISCONNECT)],
    [ResponseStep("HEAD", "/bucket", 404, headers=DISCONNECT),
     ResponseStep("PUT", "/bucket", 403,
                  b"<Error><Code>AccessDenied</Code></Error>", DISCONNECT)],
    [ResponseStep("HEAD", "/bucket", 404, headers=DISCONNECT),
     ResponseStep("PUT", "/bucket", 409,
                  b"<Error><Code>BucketAlreadyExists</Code></Error>", DISCONNECT)],
])
def test_put_stops_on_bucket_error(s3_environment, steps):
    with FaultServer(steps) as server:
        environment = {**s3_environment, "S3AR_ENDPOINT": server.endpoint}
        result = run_put(
            EXECUTABLE, environment, "s3://bucket/key", data=b"data",
            create_bucket=True,
        )

    assert result.returncode == 2
    assert b"cannot ensure destination bucket" in result.stderr
    assert b"upload failed" not in result.stderr


def test_put_does_not_create_bucket_if_input_cannot_open(s3_environment):
    with FaultServer([]) as server:
        environment = {**s3_environment, "S3AR_ENDPOINT": server.endpoint}
        result = run_put(
            EXECUTABLE, environment, "s3://bucket/key",
            path="/nonexistent/s3ar-put-input", create_bucket=True,
        )

    assert result.returncode == 2
    assert b"cannot open" in result.stderr


def test_put_distinguishes_client_initialization_failure(s3_environment):
    environment = s3_environment.copy()
    environment["S3AR_ENDPOINT"] = "invalid-endpoint"

    result = run_put(
        EXECUTABLE, environment, "s3://bucket/key", data=b""
    )

    assert result.returncode == 2
    assert b"unable to initialize S3 client" in result.stderr
    assert b"upload failed" not in result.stderr


def test_put_validates_configuration_before_input(s3_environment):
    cases = [
        ({}, b"invalid configuration"),
        (
            {**s3_environment, "S3AR_ENDPOINT": "invalid-endpoint"},
            b"unable to initialize S3 client",
        ),
    ]
    for environment, diagnostic in cases:
        result = subprocess.run(
            [str(EXECUTABLE), "-f", "/nonexistent/s3ar-put-input",
             "s3://bucket/key"],
            capture_output=True,
            env=environment,
            timeout=30,
        )
        assert result.returncode == 2
        assert diagnostic in result.stderr
        assert b"cannot open" not in result.stderr


@pytest.mark.parametrize(
    ("uri", "uri_style"),
    [
        (b"s3://bucket/invalid\xff", "path"),
        (b"s3://bucket\xff/key", "virtual"),
        (b"s3://Upper_under/key", "virtual"),
    ],
)
def test_put_validates_object_name_before_input(
    s3_environment, uri, uri_style
):
    environment = {**s3_environment, "S3AR_URI_STYLE": uri_style}
    result = subprocess.run(
        [str(EXECUTABLE), "-f", "/nonexistent/s3ar-put-input", uri],
        capture_output=True,
        env=environment,
        timeout=30,
    )

    assert result.returncode == 2
    assert b"invalid destination: invalid S3 bucket or object key" in result.stderr
    assert b"cannot open" not in result.stderr


@pytest.mark.parametrize(
    ("bucket", "data"),
    [
        ("put-stdin", b"from standard input\x00with binary data"),
        ("put-empty", b""),
    ],
)
@pytest.mark.parametrize("explicit_dash", [False, True])
def test_put_reads_stdin(s3_server, s3_environment, bucket, data, explicit_dash):
    _endpoint, client = s3_server
    if explicit_dash:
        bucket += "-dash"
    client.create_bucket(Bucket=bucket)

    result = run_put(
        EXECUTABLE, s3_environment, f"s3://{bucket}/path/to/object", data=data,
        path="-" if explicit_dash else None,
    )

    assert result.returncode == 0, result.stderr.decode()
    assert result.stderr == (
        f"s3ar-put: success: stdin to s3://{bucket}/path/to/object\n".encode()
    )
    response = client.get_object(Bucket=bucket, Key="path/to/object")
    assert response["Body"].read() == data


def test_put_encodes_destination_name_in_diagnostics(s3_server, s3_environment):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="put-encoded-name")
    key = "folder/key name%"
    encoded_uri = b"s3://put-encoded-name/folder/key%20name%25"

    result = run_put(
        EXECUTABLE, s3_environment, f"s3://put-encoded-name/{key}",
        data=b"data", verbosity=2,
    )

    assert result.returncode == 0, result.stderr.decode()
    assert result.stderr.count(encoded_uri) == 3
    assert b"key name%" not in result.stderr
    assert client.get_object(Bucket="put-encoded-name", Key=key)["Body"].read() == b"data"


def test_put_encodes_control_character_in_debug_name():
    result = run_put(
        EXECUTABLE, {}, "s3://bucket/line\n name%", data=b"", verbosity=2,
    )

    assert result.returncode == 2
    assert b"debug: (argument) destination = 's3://bucket/line%0A%20name%25'\n" in result.stderr
    assert b"line\n name" not in result.stderr


def test_put_reads_file_multipart(s3_server, s3_environment, tmp_path):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="put-file")
    data = b"a" * (16 * 1024 * 1024) + b"last part"
    source = tmp_path / "input.bin"
    source.write_bytes(data)

    result = run_put(
        EXECUTABLE, s3_environment, "s3://put-file/object", path=source,
        verbosity=1,
    )

    assert result.returncode == 0, result.stderr.decode()
    assert b"info: s3://put-file/object uploaded" in result.stderr
    assert f"success: {source} to s3://put-file/object".encode() in result.stderr
    response = client.get_object(Bucket="put-file", Key="object")
    assert response["Body"].read() == data
    assert response["ETag"].rstrip('"').endswith("-2")


def test_put_configures_multipart_size(s3_server, s3_environment):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="put-part-size")
    data = b"a" * (5 * 1024 * 1024) + b"last part"

    result = run_put(
        EXECUTABLE,
        s3_environment,
        "s3://put-part-size/object",
        data=data,
        multipart_size="5M",
    )

    assert result.returncode == 0, result.stderr.decode()
    response = client.get_object(Bucket="put-part-size", Key="object")
    assert response["Body"].read() == data
    assert response["ETag"].rstrip('"').endswith("-2")


def test_put_does_not_retry_initiate_multipart_upload(s3_environment):
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
    try:
        result = run_put(
            EXECUTABLE, environment, "s3://no-retry-initiate/object", data=MULTIPART_DATA
        )
    finally:
        server.shutdown()
        server.server_close()
        thread.join()

    assert result.returncode == 2
    assert InitiateHandler.initiates == 1


@pytest.mark.parametrize("tool", ["s3ar-put", "s3ar-copy"])
@pytest.mark.parametrize(
    "value",
    [
        "",
        "4M",
        "0M",
        "5121M",
        "18446744073709551615G",
        "18446744073709551616M",
        "6G",
        "1024",
        "5K",
        "5MB",
        "5MiB",
        "5m",
        "5g",
        "1.5G",
        "-16M",
        " 5M",
        "5M ",
    ],
)
def test_tools_reject_invalid_multipart_size(value, tool):
    operands = ["s3://unused/object"]
    if tool == "s3ar-copy":
        operands.append("s3://unused/destination")
    result = subprocess.run(
        [str(EXECUTABLE.with_name(tool)), "--multipart-size", value, *operands],
        capture_output=True, timeout=5,
    )

    assert result.returncode == 2
    assert result.stderr == (
        f"{tool}: fatal: --multipart-size must be between 5M and 5G\n".encode()
    )


@pytest.mark.parametrize(
    "options,diagnostic",
    [
        (["-f", "first", "-f", "second"], b"input file specified twice"),
        (["--multipart-size", "5M", "--multipart-size", "6M"],
         b"multipart size specified twice"),
    ],
)
def test_put_rejects_duplicate_options(options, diagnostic):
    result = subprocess.run(
        [str(EXECUTABLE), *options, "s3://bucket/key"],
        capture_output=True, env=os.environ.copy(), timeout=10,
    )
    assert result.returncode == 2
    assert b"s3ar-put: fatal: " + diagnostic in result.stderr


def test_put_rejects_long_uri_before_opening_input():
    result = subprocess.run(
        [str(EXECUTABLE), "-f", "/nonexistent/s3ar-put-input",
         "s3://bucket/" + "k" * 1025],
        capture_output=True, env=os.environ.copy(), timeout=10,
    )
    assert result.returncode == 2
    assert b"S3 URI bucket or key is too long" in result.stderr
    assert b"cannot open" not in result.stderr


def test_put_debug_logs_input_and_destination(tmp_path):
    source = tmp_path / "input"
    source.write_bytes(b"data")
    result = subprocess.run(
        [str(EXECUTABLE), "-vv", "-f", str(source), "s3://bucket/key"],
        capture_output=True, env={}, timeout=10,
    )
    assert result.returncode == 2
    assert f"(option -f) input-file = '{source}'".encode() in result.stderr
    assert b"(argument) destination = 's3://bucket/key'" in result.stderr
    assert b"invalid configuration" in result.stderr


@pytest.mark.parametrize("multipart_size,threshold", [(None, 16 * 1024 * 1024),
                                                     ("5M", 5 * 1024 * 1024)])
@pytest.mark.parametrize("offset", [-1, 0, 1])
def test_put_multipart_threshold(s3_server, s3_environment, multipart_size, threshold, offset):
    _, client = s3_server
    client.create_bucket(Bucket="put-threshold")
    body = b"x" * (threshold + offset)
    result = run_put(EXECUTABLE, s3_environment, "s3://put-threshold/key",
                     data=body, multipart_size=multipart_size)
    assert result.returncode == 0, result.stderr.decode()
    response = client.get_object(Bucket="put-threshold", Key="key")
    assert response["Body"].read() == body
    assert response["ETag"].rstrip('"').endswith("-2") == (offset > 0)


@pytest.mark.parametrize("first_response", [
    ResponseStep("PUT", "/bucket/key", 503,
                 b"<Error><Code>SlowDown</Code></Error>", DISCONNECT),
    ResponseStep("PUT", "/bucket/key", 200, b"incomplete response",
                 DISCONNECT, disconnect_after=0),
])
def test_put_retries_small_object(s3_environment, first_response):
    with FaultServer([first_response, ResponseStep("PUT", "/bucket/key", 200,
                                                  headers=DISCONNECT)]) as server:
        result = run_put(EXECUTABLE, {**s3_environment, "S3AR_ENDPOINT": server.endpoint},
                         "s3://bucket/key", data=b"data")
    assert result.returncode == 0, result.stderr.decode()
    assert len(server.requests) == 2
    assert all(request.headers["content-length"] == "4" for request in server.requests)


@pytest.mark.parametrize("size,parts", [(10 * 1024 * 1024, 2),
                                       (10 * 1024 * 1024 + 1, 3)])
def test_put_preserves_lookahead_across_parts(s3_server, s3_environment, size, parts):
    _, client = s3_server
    client.create_bucket(Bucket="put-lookahead")
    body = bytes(range(251)) * (size // 251) + bytes(range(size % 251))
    result = run_put(EXECUTABLE, s3_environment, "s3://put-lookahead/key",
                     data=body, multipart_size="5M")
    assert result.returncode == 0, result.stderr.decode()
    response = client.get_object(Bucket="put-lookahead", Key="key")
    assert response["Body"].read() == body
    assert response["ETag"].rstrip('"').endswith(f"-{parts}")
