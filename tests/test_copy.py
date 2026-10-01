import subprocess
from datetime import datetime, timezone
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


def empty_tags_step(path="/copy-source/key"):
    return ResponseStep("GET", path + "?tagging", 200,
                        b"<Tagging><TagSet/></Tagging>")


@pytest.mark.parametrize("size", [0, 9, 6 * 1024 * 1024])
def test_copy_object(s3_server, s3_environment, size):
    data = b"x" * size
    _endpoint, client = s3_server
    for bucket in ("copy-source", "copy-destination"):
        client.create_bucket(Bucket=bucket)
    client.put_object(Bucket="copy-source", Key="key", Body=data,
                      ContentType="text/plain", Metadata={"origin": "test"},
                      ContentDisposition='attachment; filename="test.txt"',
                      ContentLanguage="cs", ContentEncoding="identity",
                      CacheControl="max-age=3600",
                      Expires=datetime(2030, 1, 1, tzinfo=timezone.utc),
                      Tagging="a%20b%2F%C5%BE%2B=x%20y%3Az%2F%2B&empty=")

    result = run_copy(s3_environment, "--multipart-size", "5M")

    assert result.returncode == 0, result.stderr.decode()
    assert result.stdout == b""
    response = client.get_object(Bucket="copy-destination", Key="key")
    assert response["Body"].read() == data
    assert response["ContentType"] == "text/plain"
    assert response["Metadata"] == {"origin": "test"}
    assert response["ContentDisposition"] == 'attachment; filename="test.txt"'
    assert response["ContentLanguage"] == "cs"
    assert response["ContentEncoding"] == "identity"
    assert response["CacheControl"] == "max-age=3600"
    assert response["Expires"] == datetime(2030, 1, 1, tzinfo=timezone.utc)
    tags = client.get_object_tagging(Bucket="copy-destination", Key="key")
    assert {tag["Key"]: tag["Value"] for tag in tags["TagSet"]} == {
        "a b/ž+": "x y:z/+", "empty": "",
    }


@pytest.mark.parametrize("size", [0, 3])
def test_copy_encodes_source_bucket(s3_environment, size):
    source = "/copy%252Fsource%20%3F%23/folder/a%20b%25%3F"
    base = "/copy-destination/key"
    copy_headers = (("x-amz-copy-source", source),
                    ("x-amz-copy-source-if-match", '"source"'))
    steps = [
        ResponseStep("HEAD", source, 200,
                     headers=(("Content-Length", str(size)),
                              ("ETag", '"source"'))),
    ]
    if size == 0:
        steps.append(ResponseStep("PUT", base, 200, b"<CopyObjectResult/>",
                                  expected_headers=copy_headers))
    else:
        steps.extend([
            empty_tags_step(source),
            ResponseStep("POST", base + "?uploads", 200,
                         b"<InitiateMultipartUploadResult><UploadId>test-upload"
                         b"</UploadId></InitiateMultipartUploadResult>"),
            ResponseStep("PUT", base + "?partNumber=1&uploadId=test-upload",
                         200, b'<CopyPartResult><ETag>"part"</ETag></CopyPartResult>',
                         expected_headers=copy_headers),
            ResponseStep("POST", base + "?uploadId=test-upload", 200,
                         b"<CompleteMultipartUploadResult/>"),
        ])
    with FaultServer(steps) as server:
        result = run_copy(
            {**s3_environment, "S3AR_ENDPOINT": server.endpoint,
             "S3AR_URI_STYLE": "path"},
            source="s3://copy%2Fsource ?#/folder/a b%?",
        )
        assert result.returncode == 0, result.stderr.decode()


def test_copy_rejects_same_identity_before_network(s3_environment):
    result = run_copy(s3_environment, destination="s3://copy-source/key")
    assert result.returncode == 2
    assert b"identical" in result.stderr


@pytest.mark.parametrize("status,body", [
    (403, b"<Error><Code>AccessDenied</Code></Error>"),
    (200, b"not XML"),
    (200, b"<Tagging/>"),
    (200, b"<Tagging><TagSet><Tag><Key>key</Key></Tag></TagSet></Tagging>"),
    (200, b"<Tagging><TagSet><Tag><Key/><Value>value</Value>"
          b"</Tag></TagSet></Tagging>"),
])
def test_copy_tag_read_failure_prevents_upload(s3_environment, status, body):
    steps = [
        ResponseStep("HEAD", "/copy-source/key", 200,
                     headers=(("Content-Length", "3"), ("ETag", '"source"'))),
        ResponseStep("GET", "/copy-source/key?tagging", status, body),
    ]
    with FaultServer(steps) as server:
        result = run_copy({**s3_environment, "S3AR_ENDPOINT": server.endpoint})
        assert result.returncode == 2


@pytest.mark.parametrize("tagged", [False, True])
def test_copy_passes_encoded_tags_at_initiation(s3_environment, tagged):
    tag_set = (b"<Tag><Key>a b/+</Key><Value>x y:/+</Value></Tag>"
               b"<Tag><Key>empty</Key><Value/></Tag>" if tagged else b"")
    tags = b'<Tagging xmlns="http://s3.amazonaws.com/doc/2006-03-01/">' \
           b"<TagSet>" + tag_set + b"</TagSet></Tagging>"
    expected = (("x-amz-tagging", "a%20b%2F%2B=x%20y%3A%2F%2B&empty="),) \
               if tagged else ()
    steps = [
        ResponseStep("HEAD", "/copy-source/key", 200,
                     headers=(("Content-Length", "3"), ("ETag", '"source"'))),
        ResponseStep("GET", "/copy-source/key?tagging", 200, tags),
        ResponseStep("POST", "/copy-destination/key?uploads", 200,
                     b"<InitiateMultipartUploadResult><UploadId>test-upload"
                     b"</UploadId></InitiateMultipartUploadResult>",
                     expected_headers=expected,
                     absent_headers=() if tagged else ("x-amz-tagging",)),
        ResponseStep("PUT", "/copy-destination/key?partNumber=1&uploadId=test-upload",
                     200, b'<CopyPartResult><ETag>"part"</ETag></CopyPartResult>'),
        ResponseStep("POST", "/copy-destination/key?uploadId=test-upload", 200,
                     b"<CompleteMultipartUploadResult/>"),
    ]
    with FaultServer(steps) as server:
        result = run_copy({**s3_environment, "S3AR_ENDPOINT": server.endpoint})
        assert result.returncode == 0, result.stderr.decode()


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


@pytest.mark.parametrize("body,diagnostic", [
    (b"<Error><Code>AccessDenied</Code></Error>", b"AccessDenied"),
    (b"<CopyPartResult/>", b"CopyPartResult lacks ETag"),
    (b"<CopyPartResult><ETag/></CopyPartResult>", b"CopyPartResult lacks ETag"),
])
def test_copy_aborts_after_invalid_part_response(s3_environment, body, diagnostic):
    initiate = (b"<InitiateMultipartUploadResult><UploadId>test-upload"
                b"</UploadId></InitiateMultipartUploadResult>")
    steps = [
        ResponseStep("HEAD", "/copy-source/key", 200,
                     headers=(("Content-Length", "1"), ("ETag", '"source"'))),
        empty_tags_step(),
        ResponseStep("POST", "/copy-destination/key?uploads", 200, initiate),
        ResponseStep("PUT", "/copy-destination/key?partNumber=1&uploadId=test-upload",
                     200, body),
        ResponseStep("DELETE", "/copy-destination/key?uploadId=test-upload",
                     204),
    ]
    with FaultServer(steps) as server:
        result = run_copy({**s3_environment, "S3AR_ENDPOINT": server.endpoint})
    assert result.returncode == 2
    assert diagnostic in result.stderr
    assert len(server.requests) == 5


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
        empty_tags_step("/copy-source/a%20b%25%3F"),
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
        empty_tags_step(),
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
        empty_tags_step(),
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


@pytest.mark.parametrize("outcome", ["success", "denied", "uncertain"])
def test_copy_multipart_completion_with_encoded_upload_id(s3_environment, outcome):
    close = (("Connection", "close"),)
    base = "/copy-destination/key"
    upload_path = base + "?uploadId=copy%2B%2F%25%3D%26id"
    steps = [
        ResponseStep("HEAD", "/copy-source/key", 200,
                     headers=close + (("Content-Length", "3"), ("ETag", '"source"'))),
        empty_tags_step(),
        ResponseStep("POST", base + "?uploads", 200,
                     b"<InitiateMultipartUploadResult><UploadId>copy+/%=&amp;id"
                     b"</UploadId></InitiateMultipartUploadResult>", close),
        ResponseStep("PUT", base + "?partNumber=1&uploadId=copy%2B%2F%25%3D%26id",
                     200, b'<CopyPartResult><ETag>"part"</ETag></CopyPartResult>', close),
    ]
    if outcome == "uncertain":
        steps.extend([
            ResponseStep("POST", upload_path, 200,
                         b"<CompleteMultipartUploadResult/>", close, disconnect_after=0),
            ResponseStep("POST", upload_path, 404,
                         b"<Error><Code>NoSuchUpload</Code></Error>", close),
        ])
    elif outcome == "denied":
        steps.extend([
            ResponseStep("POST", upload_path, 200,
                         b"<Error><Code>AccessDenied</Code></Error>", close),
            ResponseStep("DELETE", upload_path, 204, headers=close),
        ])
    else:
        steps.append(ResponseStep("POST", upload_path, 200,
                                  b"<CompleteMultipartUploadResult/>", close))
    with FaultServer(steps) as server:
        result = run_copy({**s3_environment, "S3AR_ENDPOINT": server.endpoint})

    assert result.returncode == (0 if outcome == "success" else 2), result.stderr
    if outcome == "uncertain":
        assert b"completion outcome uncertain" in result.stderr
    elif outcome == "denied":
        assert b"AccessDenied" in result.stderr
