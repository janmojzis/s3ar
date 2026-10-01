import subprocess
from pathlib import Path

import pytest

from fault_server import FaultServer, ResponseStep


EXECUTABLE = Path(__file__).parents[1] / "s3ar-copy"


def run_copy(environment, *options, source="s3://copy-source/key",
             destination="s3://copy-destination/key"):
    return subprocess.run(
        [str(EXECUTABLE), *options, source, destination],
        capture_output=True, env=environment, timeout=15,
    )


@pytest.mark.parametrize("size", [0, 9, 6 * 1024 * 1024])
def test_copy_object(s3_server, s3_environment, size):
    data = b"x" * size
    _endpoint, client = s3_server
    for bucket in ("copy-source", "copy-destination"):
        client.create_bucket(Bucket=bucket)
    client.put_object(Bucket="copy-source", Key="key", Body=data,
                      ContentType="text/plain", Metadata={"origin": "test"})

    result = run_copy(s3_environment, "--multipart-size", "5M")

    assert result.returncode == 0, result.stderr.decode()
    assert result.stdout == b""
    response = client.get_object(Bucket="copy-destination", Key="key")
    assert response["Body"].read() == data
    assert response["ContentType"] == "text/plain"
    assert response["Metadata"] == {"origin": "test"}


def test_copy_rejects_same_identity_before_network(s3_environment):
    result = run_copy(s3_environment, destination="s3://copy-source/key")
    assert result.returncode == 2
    assert b"identical" in result.stderr


def test_copy_debug_logs_options_and_both_operands():
    result = run_copy(
        {}, "-vv", "--create-bucket", "--multipart-size", "5M",
        source="s3://copy-source/a b%?",
        destination="s3://copy-destination/new key",
    )
    assert result.returncode == 2
    for expected in (
        b"(option -v) verbosity = '2'",
        b"(option -h) help = 'false'",
        b"(option --create-bucket) create-bucket = 'true'",
        b"(option --multipart-size) multipart-size = '5 MiB'",
        b"maximum object size = '48.83 GiB' (10000 parts)",
        b"(argument) source = 's3://copy-source/a%20b%25%3F'",
        b"(argument) destination = 's3://copy-destination/new%20key'",
    ):
        assert expected in result.stderr
    assert b"invalid configuration" in result.stderr


def test_copy_debug_logs_help():
    result = subprocess.run(
        [str(EXECUTABLE), "-vv", "--help"],
        capture_output=True, env={}, timeout=10,
    )
    assert result.returncode == 0
    assert b"(option -v) verbosity = '2'" in result.stderr
    assert b"(option -h) help = 'true'" in result.stderr
    assert b"Usage: s3ar-copy" in result.stdout


def test_copy_aborts_after_embedded_part_error(s3_environment):
    initiate = (b"<InitiateMultipartUploadResult><UploadId>test-upload"
                b"</UploadId></InitiateMultipartUploadResult>")
    steps = [
        ResponseStep("HEAD", "/copy-source/key", 200,
                     headers=(("Content-Length", "1"), ("ETag", '"source"'))),
        ResponseStep("POST", "/copy-destination/key?uploads", 200, initiate),
        ResponseStep("PUT", "/copy-destination/key?partNumber=1&uploadId=test-upload",
                     200, b"<Error><Code>AccessDenied</Code></Error>"),
        ResponseStep("DELETE", "/copy-destination/key?uploadId=test-upload",
                     204),
    ]
    with FaultServer(steps) as server:
        result = run_copy({**s3_environment, "S3AR_ENDPOINT": server.endpoint})
    assert result.returncode == 2
    assert len(server.requests) == 4


def test_copy_uses_ranges_and_source_etag(s3_environment):
    initiate = (b"<InitiateMultipartUploadResult><UploadId>test-upload"
                b"</UploadId></InitiateMultipartUploadResult>")
    part = b'<CopyPartResult><ETag>"part"</ETag></CopyPartResult>'
    base = "/copy-destination/key"
    source = ("x-amz-copy-source", "/copy-source/a%20b%25%3F")
    match = ("x-amz-copy-source-if-match", '"source"')
    steps = [
        ResponseStep("HEAD", "/copy-source/a%20b%25%3F", 200,
                     headers=(("Content-Length", str(6 * 1024 * 1024)),
                              ("ETag", '"source"'))),
        ResponseStep("POST", base + "?uploads", 200, initiate),
        ResponseStep("PUT", base + "?partNumber=1&uploadId=test-upload",
                     200, part, expected_headers=(source, match,
                        ("x-amz-copy-source-range", "bytes=0-5242879"))),
        ResponseStep("PUT", base + "?partNumber=2&uploadId=test-upload",
                     200, part, expected_headers=(source, match,
                        ("x-amz-copy-source-range", "bytes=5242880-6291455"))),
        ResponseStep("POST", base + "?uploadId=test-upload", 200,
                     b"<CompleteMultipartUploadResult/>"),
    ]
    with FaultServer(steps) as server:
        result = run_copy({**s3_environment, "S3AR_ENDPOINT": server.endpoint},
                          "--multipart-size", "5M",
                          source="s3://copy-source/a b%?")
        assert result.returncode == 0, result.stderr.decode()


def test_copy_small_object_omits_range(s3_environment):
    initiate = (b"<InitiateMultipartUploadResult><UploadId>test-upload"
                b"</UploadId></InitiateMultipartUploadResult>")
    steps = [
        ResponseStep("HEAD", "/copy-source/key", 200,
                     headers=(("Content-Length", "3"), ("ETag", '"source"'))),
        ResponseStep("POST", "/copy-destination/key?uploads", 200, initiate),
        ResponseStep("PUT", "/copy-destination/key?partNumber=1&uploadId=test-upload",
                     200, b'<CopyPartResult><ETag>"part"</ETag></CopyPartResult>',
                     expected_headers=(("x-amz-copy-source-if-match", '"source"'),),
                     absent_headers=("x-amz-copy-source-range",)),
        ResponseStep("POST", "/copy-destination/key?uploadId=test-upload", 200,
                     b"<CompleteMultipartUploadResult/>"),
    ]
    with FaultServer(steps) as server:
        result = run_copy({**s3_environment, "S3AR_ENDPOINT": server.endpoint})
        assert result.returncode == 0, result.stderr.decode()


def test_copy_source_change_aborts(s3_environment):
    initiate = (b"<InitiateMultipartUploadResult><UploadId>test-upload"
                b"</UploadId></InitiateMultipartUploadResult>")
    steps = [
        ResponseStep("HEAD", "/copy-source/key", 200,
                     headers=(("Content-Length", "3"), ("ETag", '"source"'))),
        ResponseStep("POST", "/copy-destination/key?uploads", 200, initiate),
        ResponseStep("PUT", "/copy-destination/key?partNumber=1&uploadId=test-upload",
                     412, b"<Error><Code>PreconditionFailed</Code></Error>"),
        ResponseStep("DELETE", "/copy-destination/key?uploadId=test-upload", 204),
    ]
    with FaultServer(steps) as server:
        result = run_copy({**s3_environment, "S3AR_ENDPOINT": server.endpoint})
        assert result.returncode == 2
        assert b"source object changed" in result.stderr


def test_copy_rejects_more_than_10000_parts_before_upload(s3_environment):
    steps = [
        ResponseStep("HEAD", "/copy-source/key", 200,
                     headers=(("Content-Length", str(10000 * 5 * 1024 * 1024 + 1)),
                              ("ETag", '"source"'))),
    ]
    with FaultServer(steps) as server:
        result = run_copy({**s3_environment, "S3AR_ENDPOINT": server.endpoint},
                          "--multipart-size", "5M")
        assert result.returncode == 2
        assert b"increase --multipart-size" in result.stderr
