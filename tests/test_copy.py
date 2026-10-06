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


@pytest.mark.parametrize("abort_status,abort_body", [
    (204, b""),
    (404, b"<Error><Code>NoSuchUpload</Code></Error>"),
    (403, b"<Error><Code>AccessDenied</Code><Message>Abort denied.</Message></Error>"),
])
def test_copy_source_change_aborts(s3_environment, abort_status, abort_body):
    initiate = (b"<InitiateMultipartUploadResult><UploadId>test-upload"
                b"</UploadId></InitiateMultipartUploadResult>")
    steps = [
        ResponseStep("HEAD", "/copy-source/key", 200,
                     headers=(("Content-Length", "3"), ("ETag", '"source"'))),
        empty_tags_step(),
        ResponseStep("POST", "/copy-destination/key?uploads", 200, initiate),
        ResponseStep("PUT", "/copy-destination/key?partNumber=1&uploadId=test-upload",
                     412, b"<Error><Code>PreconditionFailed</Code></Error>"),
        ResponseStep("DELETE", "/copy-destination/key?uploadId=test-upload",
                     abort_status, abort_body),
    ]
    with FaultServer(steps) as server:
        result = run_copy({**s3_environment, "S3AR_ENDPOINT": server.endpoint})
        assert result.returncode == 2
        assert b"source object changed" in result.stderr
        assert (b"multipart abort failed" in result.stderr) == (abort_status == 403)
        if abort_status == 403:
            assert b"Abort denied." in result.stderr


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


def copy_command(environment, *arguments):
    return subprocess.run([str(EXECUTABLE), *arguments], capture_output=True,
                          env=environment, timeout=20)


@pytest.fixture
def copy_buckets(s3_server):
    import uuid
    _endpoint, client = s3_server
    suffix = uuid.uuid4().hex
    source, destination = f"copy-src-{suffix}", f"copy-dst-{suffix}"
    for bucket in (source, destination):
        client.create_bucket(Bucket=bucket)
    return client, source, destination


def object_keys(client, bucket):
    return {item["Key"] for page in client.get_paginator("list_objects_v2").paginate(
        Bucket=bucket) for item in page.get("Contents", [])}


@pytest.mark.parametrize("options,source_key,target,target_key", [
    ([], "dir/file.txt", "backup/", "backup/file.txt"),
    (["-T"], "dir/file.txt", "backup/", "backup/"),
    (["--no-target-directory"], "/dir/file.txt", "/backup/", "/backup/"),
    ([], "dir/file.txt", "", "file.txt"),
    ([], "dir/", "markers/", "markers/dir/"),
    ([], "dir/file.txt", "literal", "literal"),
    (["-r"], "dir/file.txt", "backup/", "backup/file.txt"),
])
def test_copy_target_mapping(s3_environment, copy_buckets, options, source_key,
                             target, target_key):
    client, source, destination = copy_buckets
    client.put_object(Bucket=source, Key=source_key, Body=b"data")
    client.put_object(Bucket=destination, Key=target_key, Body=b"old")
    result = copy_command(s3_environment, *options, f"s3://{source}/{source_key}",
                          f"s3://{destination}/{target}")
    assert result.returncode == 0, result.stderr
    assert result.stdout == b""
    assert client.get_object(Bucket=destination, Key=target_key)["Body"].read() == b"data"
    assert b"1 copied, 0 skipped, 0 failed" in result.stderr


@pytest.mark.parametrize("recursive_option", ["-r", "-R", "--recursive"])
def test_copy_recursive_prefix(s3_environment, copy_buckets, recursive_option):
    client, source, destination = copy_buckets
    keys = {"photos/": b"", "photos/a.jpg": b"a", "photos/sub/b.jpg": b"b",
            "photos//a b%?.jpg": b"special", "photos/./../literal": b"dots",
            "photos-old/no.jpg": b"neighbor", "photos": b"exact"}
    for key, body in keys.items():
        client.put_object(Bucket=source, Key=key, Body=body)
    client.put_object(Bucket=destination, Key="extra", Body=b"keep")
    result = copy_command(s3_environment, recursive_option,
                          f"s3://{source}/photos/", f"s3://{destination}/backup/")
    assert result.returncode == 0, result.stderr
    expected = {"backup/" + key[len("photos/"):] for key in keys
                if key.startswith("photos/")} | {"extra"}
    assert object_keys(client, destination) == expected
    for key, body in keys.items():
        if key.startswith("photos/"):
            target = "backup/" + key[len("photos/"):]
            assert client.get_object(Bucket=destination, Key=target)["Body"].read() == body


@pytest.mark.parametrize("root_suffix,target", [("/", ""), ("", "backup/")])
def test_copy_whole_bucket(s3_environment, copy_buckets, root_suffix, target):
    client, source, destination = copy_buckets
    for key in ("file", "dir/file", "/leading", "marker/"):
        client.put_object(Bucket=source, Key=key, Body=b"")
    result = copy_command(s3_environment, "-r", f"s3://{source}{root_suffix}",
                          f"s3://{destination}/{target}")
    assert result.returncode == 0, result.stderr
    assert object_keys(client, destination) == {
        target + key for key in ("file", "dir/file", "/leading", "marker/")}


@pytest.mark.parametrize("target_option", [False, True])
def test_copy_multiple_objects(s3_environment, copy_buckets, target_option):
    client, source, destination = copy_buckets
    for key in ("dir/x", "other/y"):
        client.put_object(Bucket=source, Key=key, Body=key.encode())
    operands = [f"s3://{source}/dir/x", f"s3://{source}/other/y"]
    arguments = (["-t", f"s3://{destination}/backup", *operands] if target_option
                 else [*operands, f"s3://{destination}/backup/"])
    result = copy_command(s3_environment, *arguments)
    assert result.returncode == 0, result.stderr
    assert object_keys(client, destination) == {"backup/x", "backup/y"}


def test_copy_multiple_prefix_collision_and_continue(s3_environment, copy_buckets):
    client, source, destination = copy_buckets
    for key in ("first/x", "second/x", "second/y"):
        client.put_object(Bucket=source, Key=key, Body=key.encode())
    result = copy_command(s3_environment, "-r", f"s3://{source}/first/",
                          f"s3://{source}/second/", f"s3://{destination}/")
    assert result.returncode == 2
    assert b"collision" in result.stderr
    assert b"2 copied, 0 skipped, 1 failed" in result.stderr
    assert client.get_object(Bucket=destination, Key="x")["Body"].read() == b"first/x"
    assert object_keys(client, destination) == {"x", "y"}


def test_copy_missing_object_continues(s3_environment, copy_buckets):
    client, source, destination = copy_buckets
    client.put_object(Bucket=source, Key="good", Body=b"ok")
    result = copy_command(s3_environment, f"s3://{source}/missing",
                          f"s3://{source}/good", f"s3://{destination}/")
    assert result.returncode == 2
    assert b"1 copied, 0 skipped, 1 failed" in result.stderr
    assert object_keys(client, destination) == {"good"}


@pytest.mark.parametrize("recursive", [False, True])
def test_copy_dry_run_never_creates_bucket(s3_environment, copy_buckets, recursive):
    client, source, destination = copy_buckets
    client.delete_bucket(Bucket=destination)
    client.put_object(Bucket=source, Key="dir/a b%?", Body=b"data")
    arguments = ["--dry-run", "--create-bucket"]
    arguments += (["-r", f"s3://{source}/dir/"] if recursive
                  else [f"s3://{source}/dir/a b%?"])
    result = copy_command(s3_environment, *arguments, f"s3://{destination}/backup/")
    assert result.returncode == 0, result.stderr
    assert result.stdout == (f"s3://{source}/dir/a%20b%25%3F -> "
                             f"s3://{destination}/backup/a%20b%25%3F\n").encode()
    assert b"1 planned, 0 skipped, 0 failed" in result.stderr
    assert destination not in {bucket["Name"] for bucket in client.list_buckets()["Buckets"]}


@pytest.mark.parametrize("prefix,status", [("", 0), ("missing/", 2)])
def test_copy_empty_selection(s3_environment, copy_buckets, prefix, status):
    _client, source, destination = copy_buckets
    result = copy_command(s3_environment, "-r", f"s3://{source}/{prefix}",
                          f"s3://{destination}/")
    assert result.returncode == status, result.stderr


def test_copy_root_marker_skipped(s3_environment, copy_buckets):
    client, source, destination = copy_buckets
    client.put_object(Bucket=source, Key="dir/", Body=b"")
    client.put_object(Bucket=source, Key="dir/x", Body=b"x")
    result = copy_command(s3_environment, "-r", f"s3://{source}/dir/",
                          f"s3://{destination}/")
    assert result.returncode == 0, result.stderr
    assert b"1 copied, 1 skipped, 0 failed" in result.stderr
    assert object_keys(client, destination) == {"x"}


@pytest.mark.parametrize("arguments,diagnostic", [
    (["s3://a/", "s3://b/"], b"requires -r"),
    (["-r", "s3://a/p/", "s3://b/key"], b"require a destination"),
    (["-r", "s3://a/p/", "s3://a/p/sub/"], b"overlap"),
    (["-r", "s3://a/p/sub/", "s3://a/p/"], b"overlap"),
    (["-r", "s3://a/", "s3://a/"], b"overlap"),
    (["s3://a/x", "s3://a/y", "s3://b/key"], b"multiple sources"),
    (["-T", "s3://a/x", "s3://a/y", "s3://b/key"], b"requires one"),
    (["-T", "s3://a/x", "s3://b/"], b"requires one"),
    (["-t", "s3://b/", "-T", "s3://a/x"], b"cannot be combined"),
    (["-t", "s3://b/", "-t", "s3://c/", "s3://a/x"], b"specified twice"),
    (["s3://a/x", "https://b/x"], b"invalid S3 operand"),
])
def test_copy_invalid_arguments_before_network(arguments, diagnostic):
    result = copy_command({}, *arguments)
    assert result.returncode == 2
    assert diagnostic in result.stderr
    assert b"invalid configuration" not in result.stderr


def test_copy_protects_other_exact_source(s3_environment, copy_buckets):
    client, source, _destination = copy_buckets
    client.put_object(Bucket=source, Key="original/x", Body=b"first")
    client.put_object(Bucket=source, Key="target/x", Body=b"second")
    result = copy_command(s3_environment, f"s3://{source}/original/x",
                          f"s3://{source}/target/x", f"s3://{source}/target/")
    assert result.returncode == 2
    assert b"overwrite another source" in result.stderr
    assert client.get_object(Bucket=source, Key="target/x")["Body"].read() == b"second"


def listing_xml(keys, token=None):
    import html
    from urllib.parse import quote
    contents = "".join(
        f"<Contents><Key>{html.escape(quote(key, safe=''))}</Key><Size>0</Size>"
        "<LastModified>2026-01-01T00:00:00Z</LastModified><ETag>empty</ETag></Contents>"
        for key in keys)
    more = "true" if token else "false"
    continuation = f"<NextContinuationToken>{html.escape(token)}</NextContinuationToken>" if token else ""
    return (f"<ListBucketResult><EncodingType>url</EncodingType>"
            f"<IsTruncated>{more}</IsTruncated>{contents}{continuation}"
            "</ListBucketResult>").encode()


def test_copy_dry_run_pages_over_1000_objects(s3_environment):
    base = "/copy-source?list-type=2&max-keys=1000&encoding-type=url&prefix=dir%2F"
    keys = [f"dir/{index:04}" for index in range(1000)]
    steps = [ResponseStep("GET", base, 200, listing_xml(keys, "next+/?")),
             ResponseStep("GET", base + "&continuation-token=next%2B%2F%3F", 200,
                          listing_xml(["dir/last"]))]
    with FaultServer(steps) as server:
        result = copy_command({**s3_environment, "S3AR_ENDPOINT": server.endpoint},
                              "--dry-run", "-r", "s3://copy-source/dir/",
                              "s3://copy-destination/backup/")
    assert result.returncode == 0, result.stderr
    assert len(result.stdout.splitlines()) == 1001
    assert result.stdout.splitlines()[-1].endswith(b"/backup/last")
    assert b"1001 planned" in result.stderr


def test_copy_listing_error_continues_next_source(s3_environment):
    base = "/copy-source?list-type=2&max-keys=1000&encoding-type=url&prefix="
    steps = [ResponseStep("GET", base + "bad%2F", 403,
                          b"<Error><Code>AccessDenied</Code></Error>"),
             ResponseStep("GET", base + "good%2F", 200, listing_xml(["good/x"]))]
    with FaultServer(steps) as server:
        result = copy_command({**s3_environment, "S3AR_ENDPOINT": server.endpoint},
                              "--dry-run", "-r", "s3://copy-source/bad/",
                              "s3://copy-source/good/", "s3://copy-destination/")
    assert result.returncode == 2
    assert b"1 planned, 0 skipped, 1 failed" in result.stderr


def test_copy_overlong_mapped_key_continues(s3_environment):
    prefix = "p" * 1022 + "/"
    steps = [ResponseStep("GET", "/copy-source?list-type=2&max-keys=1000&encoding-type=url",
                          200, listing_xml(["long", "x"]))]
    with FaultServer(steps) as server:
        result = copy_command({**s3_environment, "S3AR_ENDPOINT": server.endpoint},
                              "--dry-run", "-r", "s3://copy-source/",
                              f"s3://copy-destination/{prefix}")
    assert result.returncode == 2
    assert b"too long" in result.stderr
    assert b"1 planned, 0 skipped, 1 failed" in result.stderr


@pytest.mark.parametrize("interrupt_name", ["SIGINT", "SIGTERM"])
def test_copy_interrupted_part_aborts_and_stops(s3_environment, interrupt_name):
    import signal
    import threading
    from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

    part_started, aborted, release = (threading.Event() for _ in range(3))
    requests = []

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_arguments):
            pass

        def respond(self, body=b"", status=200, headers=()):
            self.send_response(status)
            if not any(name.lower() == "content-length" for name, _value in headers):
                self.send_header("Content-Length", str(len(body)))
            for name, value in headers:
                self.send_header(name, value)
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(body)

        def do_GET(self):
            requests.append(("GET", self.path))
            if "list-type=2" in self.path:
                self.respond(listing_xml(["dir/first", "dir/second"]))
            else:
                self.respond(b"<Tagging><TagSet/></Tagging>")

        def do_HEAD(self):
            requests.append(("HEAD", self.path))
            self.respond(headers=(("Content-Length", "3"), ("ETag", '"source"')))

        def do_POST(self):
            requests.append(("POST", self.path))
            self.respond(b"<InitiateMultipartUploadResult><UploadId>test-upload"
                         b"</UploadId></InitiateMultipartUploadResult>")

        def do_PUT(self):
            requests.append(("PUT", self.path))
            part_started.set()
            release.wait(10)

        def do_DELETE(self):
            requests.append(("DELETE", self.path))
            self.respond(status=204)
            aborted.set()

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    endpoint = f"http://127.0.0.1:{server.server_port}"
    process = subprocess.Popen(
        [str(EXECUTABLE), "-r", "s3://copy-source/dir/", "s3://copy-destination/"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        env={**s3_environment, "S3AR_ENDPOINT": endpoint})
    try:
        if not part_started.wait(5):
            assert process.poll() is None, process.communicate(timeout=5)[1]
            pytest.fail(f"copy did not start a part: {requests!r}")
        process.send_signal(getattr(signal, interrupt_name))
        assert aborted.wait(5), "multipart upload was not aborted"
        stdout, stderr = process.communicate(timeout=5)
        assert process.returncode == 2
        assert stdout == b""
        assert b"interrupted" in stderr
        assert requests[-1] == ("DELETE", "/copy-destination/first?uploadId=test-upload")
        assert not any("second" in path for _method, path in requests)
    finally:
        release.set()
        if process.poll() is None:
            process.kill()
            process.communicate()
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


def test_copy_create_bucket(s3_environment, copy_buckets):
    client, source, destination = copy_buckets
    client.delete_bucket(Bucket=destination)
    client.put_object(Bucket=source, Key="key", Body=b"")
    result = copy_command(s3_environment, "-r", "--create-bucket",
                          f"s3://{source}/", f"s3://{destination}/")
    assert result.returncode == 0, result.stderr
    assert object_keys(client, destination) == {"key"}


def test_copy_dry_run_registry_grows_and_detects_collision(s3_environment):
    base = "/copy-source?list-type=2&max-keys=1000&encoding-type=url&prefix="
    first = [f"one/{index:03}" for index in range(150)]
    steps = [ResponseStep("GET", base + "one%2F", 200, listing_xml(first)),
             ResponseStep("GET", base + "two%2F", 200, listing_xml(["two/149", "two/last"]))]
    with FaultServer(steps) as server:
        result = copy_command({**s3_environment, "S3AR_ENDPOINT": server.endpoint},
                              "--dry-run", "-r", "s3://copy-source/one/",
                              "s3://copy-source/two/", "s3://copy-destination/")
    assert result.returncode == 2
    assert len(result.stdout.splitlines()) == 151
    assert b"151 planned, 0 skipped, 1 failed" in result.stderr


def test_copy_dry_run_missing_exact_source(s3_environment):
    with FaultServer([ResponseStep("HEAD", "/copy-source/missing", 404)]) as server:
        result = copy_command({**s3_environment, "S3AR_ENDPOINT": server.endpoint},
                              "--dry-run", "s3://copy-source/missing",
                              "s3://copy-destination/key")
    assert result.returncode == 2
    assert result.stdout == b""


def test_copy_rejects_listing_outside_prefix(s3_environment):
    steps = [ResponseStep("GET", "/copy-source?list-type=2&max-keys=1000&encoding-type=url&prefix=dir%2F",
                          200, listing_xml(["x"]))]
    with FaultServer(steps) as server:
        result = copy_command({**s3_environment, "S3AR_ENDPOINT": server.endpoint},
                              "--dry-run", "-r", "s3://copy-source/dir/",
                              "s3://copy-destination/")
    assert result.returncode == 2
    assert b"outside the source prefix" in result.stderr
    assert result.stdout == b""
