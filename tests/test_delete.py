import subprocess
from pathlib import Path
import os
import threading
from urllib.parse import quote

import boto3
import pytest
from moto.server import create_backend_app
from moto.backends import get_backend
from werkzeug.serving import make_server
from botocore.exceptions import ClientError

from fault_server import FaultServer, ResponseStep


EXECUTABLE = Path(__file__).resolve().parents[1] / "s3ar-delete"


def run(*arguments, env):
    return subprocess.run(
        [str(EXECUTABLE), *arguments],
        text=True,
        capture_output=True,
        env=env,
        check=False,
    )


def keys(client, bucket):
    response = client.list_objects_v2(Bucket=bucket)
    return sorted(item["Key"] for item in response.get("Contents", []))


@pytest.mark.parametrize("option", ["--yes", "-y"])
def test_yes_option_is_rejected(option):
    result = run(option, "s3://bucket/key", env=os.environ.copy())
    assert result.returncode == 2
    assert "Usage: s3ar-delete" in result.stderr


@pytest.mark.parametrize(
    "selection", ["s3://delete-missing-bucket", "s3://delete-missing-bucket/aaaa"]
)
def test_missing_bucket_error_includes_selection(
    s3_server, s3_environment, selection
):
    result = run("--dry-run", selection, env=s3_environment)
    assert result.returncode == 2
    assert f"unable to process {selection}:" in result.stderr


def test_missing_bucket_error_encodes_selection(s3_environment):
    result = run(
        "-vv", "--dry-run", "s3://delete-missing-bucket/line\n name%",
        env=s3_environment,
    )

    assert result.returncode == 2
    assert "debug: (argument) selection = 's3://delete-missing-bucket/line%0A%20name%25'\n" in result.stderr
    assert "unable to process s3://delete-missing-bucket/line%0A%20name%25:" in result.stderr
    assert "line\n name" not in result.stderr


@pytest.mark.parametrize("uploads", [False, True])
def test_target_delete_failure_has_one_complete_diagnostic(
    s3_environment, uploads
):
    key = "key name%"
    encoded_key = "key%20name%25"
    bucket = "delete-denied"
    prefix = f"/{bucket}?"
    query_suffix = f"&encoding-type=url&prefix={encoded_key}"
    empty_uploads = (
        b"<ListMultipartUploadsResult><EncodingType>url</EncodingType>"
        b"<IsTruncated>false</IsTruncated></ListMultipartUploadsResult>"
    )
    upload_listing = (
        b"<ListMultipartUploadsResult><EncodingType>url</EncodingType>"
        b"<IsTruncated>false</IsTruncated><Upload><Key>key%20name%25</Key>"
        b"<UploadId>u1</UploadId></Upload></ListMultipartUploadsResult>"
    )
    version_listing = (
        b"<ListVersionsResult><EncodingType>url</EncodingType>"
        b"<IsTruncated>false</IsTruncated><Version><Key>key%20name%25</Key>"
        b"<VersionId>v1</VersionId></Version></ListVersionsResult>"
    )
    steps = [
        ResponseStep(
            "GET", prefix + "uploads&max-uploads=1000" + query_suffix,
            200, upload_listing if uploads else empty_uploads,
        ),
    ]
    if not uploads:
        steps.append(ResponseStep(
            "GET", prefix + "versions&max-keys=1000" + query_suffix,
            200, version_listing,
        ))
    if uploads:
        steps.append(ResponseStep(
            "DELETE", f"/{bucket}/{encoded_key}?uploadId=u1", 403,
            b"<Error><Code>AccessDenied</Code><Message>Denied.</Message></Error>",
        ))
    else:
        steps.append(ResponseStep(
            "POST", f"/{bucket}?delete", 200,
            b"<DeleteResult><Error><Key>key name%</Key><VersionId>v1</VersionId>"
            b"<Code>AccessDenied</Code><Message>Denied.</Message>"
            b"</Error></DeleteResult>",
        ))

    with FaultServer(steps) as server:
        environment = {**s3_environment, "S3AR_ENDPOINT": server.endpoint}
        result = run(f"s3://{bucket}/{key}", env=environment)

    assert result.returncode == 2
    assert result.stderr.count("fatal:") == 1
    if uploads:
        assert f"unable to abort upload s3://{bucket}/{encoded_key}:" in result.stderr
    else:
        assert (f"unable to delete object version s3://{bucket}/{encoded_key} "
                "version=v1:" in result.stderr)
    status = 403 if uploads else 200
    assert f"Denied. (S3 AccessDenied) [HTTP {status}]" in result.stderr
    assert "unable to process" not in result.stderr


def test_batch_delete_reports_individual_errors(s3_environment):
    steps = [
        ResponseStep(
            "GET", "/bucket?uploads&max-uploads=1000&encoding-type=url",
            200,
            b"<ListMultipartUploadsResult><EncodingType>url</EncodingType>"
            b"<IsTruncated>false</IsTruncated></ListMultipartUploadsResult>",
        ),
        ResponseStep(
            "GET", "/bucket?versions&max-keys=1000&encoding-type=url",
            200,
            b"<ListVersionsResult><EncodingType>url</EncodingType>"
            b"<IsTruncated>false</IsTruncated>"
            b"<Version><Key>first</Key><VersionId>v1</VersionId></Version>"
            b"<Version><Key>second</Key><VersionId>v2</VersionId></Version>"
            b"</ListVersionsResult>",
        ),
        ResponseStep(
            "POST", "/bucket?delete", 200,
            b"<DeleteResult>"
            b"<Error><Key>second</Key><VersionId>v2</VersionId>"
            b"<Code>AccessDenied</Code><Message>Denied.</Message></Error>"
            b"<Deleted><Key>first</Key><VersionId>v1</VersionId></Deleted>"
            b"</DeleteResult>",
        ),
    ]
    with FaultServer(steps) as server:
        environment = {**s3_environment, "S3AR_ENDPOINT": server.endpoint}
        result = run("-v", "s3://bucket", env=environment)

    assert result.returncode == 2
    assert "s3://bucket/first deleted version=v1" in result.stderr
    assert ("unable to delete object version s3://bucket/second version=v2:"
            in result.stderr)
    assert "Denied. (S3 AccessDenied) [HTTP 200]" in result.stderr
    assert result.stderr.count("fatal:") == 1
    assert server.requests[-1].headers["content-md5"]


@pytest.mark.parametrize("character", ["\x01", "\x0b", "\x1f", "\ufffe", "\uffff"])
@pytest.mark.parametrize("mixed,denied,batch_denied", [
    (False, False, False), (False, True, False),
    (True, False, False), (True, True, False), (True, False, True),
])
def test_batch_delete_falls_back_for_xml_incompatible_keys(
    s3_environment, character, mixed, denied, batch_denied
):
    key = f"folder/b{character}"
    encoded_key = quote(key, safe="/")
    version = "v/&1"
    encoded_version = quote(version, safe="")
    close = (("Connection", "close"),)
    prefix = "/bucket?"
    suffix = "&encoding-type=url&prefix=folder%2F"
    empty_uploads = (
        b"<ListMultipartUploadsResult><EncodingType>url</EncodingType>"
        b"<IsTruncated>false</IsTruncated></ListMultipartUploadsResult>"
    )
    empty_versions = (
        b"<ListVersionsResult><EncodingType>url</EncodingType>"
        b"<IsTruncated>false</IsTruncated></ListVersionsResult>"
    )
    targets = [(key, version)]
    if mixed:
        targets = [("folder/a", "v1"), *targets, ("folder/z", "v2")]
    entries = "".join(
        f"<Version><Key>{quote(name, safe='/')}</Key>"
        f"<VersionId>{identifier.replace('&', '&amp;')}</VersionId></Version>"
        for name, identifier in targets
    )
    listing = (
        "<ListVersionsResult><EncodingType>url</EncodingType>"
        f"<IsTruncated>false</IsTruncated>{entries}</ListVersionsResult>"
    ).encode()
    steps = [
        ResponseStep("GET", prefix + "uploads&max-uploads=1000" + suffix,
                     200, empty_uploads, close),
        ResponseStep("GET", prefix + "versions&max-keys=1000" + suffix,
                     200, listing, close),
    ]
    if mixed:
        batch_response = (
            b"<DeleteResult><Deleted><Key>folder/a</Key><VersionId>v1</VersionId>"
            b"</Deleted>"
        )
        if batch_denied:
            batch_response += (
                b"<Error><Key>folder/z</Key><VersionId>v2</VersionId>"
                b"<Code>AccessDenied</Code><Message>Batch denied.</Message></Error>"
            )
        else:
            batch_response += (
                b"<Deleted><Key>folder/z</Key><VersionId>v2</VersionId></Deleted>"
            )
        batch_response += b"</DeleteResult>"
        steps.append(ResponseStep(
            "POST", "/bucket?delete", 200, batch_response, close,
        ))
    steps.append(ResponseStep(
        "DELETE", f"/bucket/{encoded_key}?versionId={encoded_version}",
        403 if denied else 204,
        (b"<Error><Code>AccessDenied</Code><Message>Denied.</Message></Error>"
         if denied else b""), close,
    ))
    if not denied and not batch_denied:
        steps.append(ResponseStep(
            "GET", prefix + "versions&max-keys=1000" + suffix,
            200, empty_versions, close,
        ))
    with FaultServer(steps) as server:
        environment = {**s3_environment, "S3AR_ENDPOINT": server.endpoint}
        result = run("-v", "s3://bucket/folder/", env=environment)

    if batch_denied:
        assert result.returncode == 2
        assert "Batch denied. (S3 AccessDenied) [HTTP 200]" in result.stderr
        assert f"s3://bucket/{encoded_key} deleted version={version}" in result.stderr
        assert "folder/a deleted version=v1" in result.stderr
        assert "success:" not in result.stderr
    elif denied:
        assert result.returncode == 2
        assert "Denied. (S3 AccessDenied) [HTTP 403]" in result.stderr
        assert "success:" not in result.stderr
    else:
        assert result.returncode == 0, result.stderr
        assert f"s3://bucket/{encoded_key} deleted version={version}" in result.stderr
        assert f"success: {len(targets)} versions, 0 uploads, 0 buckets" in result.stderr
        if mixed:
            assert "folder/a deleted version=v1" in result.stderr
            assert "folder/z deleted version=v2" in result.stderr
    assert len([r for r in server.requests if r.method == "POST"]) == int(mixed)


def test_trace_redacts_delete_query_values(s3_environment):
    steps = [
        ResponseStep(
            "GET", "/bucket?uploads&max-uploads=1000&encoding-type=url&prefix=key",
            200,
            b"<ListMultipartUploadsResult><EncodingType>url</EncodingType>"
            b"<IsTruncated>false</IsTruncated><Upload><Key>key</Key>"
            b"<UploadId>private-upload-id</UploadId></Upload>"
            b"</ListMultipartUploadsResult>",
        ),
        ResponseStep("DELETE", "/bucket/key?uploadId=private-upload-id", 204),
        ResponseStep(
            "GET", "/bucket?uploads&max-uploads=1000&encoding-type=url&prefix=key",
            200,
            b"<ListMultipartUploadsResult><EncodingType>url</EncodingType>"
            b"<IsTruncated>false</IsTruncated></ListMultipartUploadsResult>",
        ),
        ResponseStep(
            "GET", "/bucket?versions&max-keys=1000&encoding-type=url&prefix=key",
            200,
            b"<ListVersionsResult><EncodingType>url</EncodingType>"
            b"<IsTruncated>false</IsTruncated></ListVersionsResult>",
        ),
    ]
    with FaultServer(steps) as server:
        environment = {**s3_environment, "S3AR_ENDPOINT": server.endpoint}
        result = run("-vvv", "s3://bucket/key", env=environment)

    assert result.returncode == 0, result.stderr
    traces = [line for line in result.stderr.splitlines() if ": trace:" in line]
    assert len([line for line in traces if "http id=" in line]) == 4
    assert any("uploadId=[redacted]" in line for line in traces)
    assert all("private-upload-id" not in line for line in traces)


def test_exact_key_dry_run_and_delete(s3_server, s3_environment):
    _, client = s3_server
    bucket = "delete-exact-key"
    client.create_bucket(Bucket=bucket)
    for key in ("photo", "photo1", "photo/a"):
        client.put_object(Bucket=bucket, Key=key, Body=b"data")

    summary = run("--dry-run", f"s3://{bucket}/photo", env=s3_environment)
    assert summary.returncode == 0, summary.stderr
    assert "success: 1 versions, 0 uploads, 0 buckets" in summary.stderr
    assert "info:" not in summary.stderr

    preview = run("-v", "--dry-run", f"s3://{bucket}/photo", env=s3_environment)
    assert preview.returncode == 0, preview.stderr
    assert "info: s3://delete-exact-key/photo would delete version=" in preview.stderr
    assert "debug:" not in preview.stderr
    assert "trace:" not in preview.stderr
    assert " deleted\n" not in preview.stderr
    assert "s3://delete-exact-key/photo1" not in preview.stderr
    assert "s3://delete-exact-key/photo/a" not in preview.stderr
    assert keys(client, bucket) == ["photo", "photo/a", "photo1"]

    debug = run("-vv", "--dry-run", f"s3://{bucket}/photo", env=s3_environment)
    assert debug.returncode == 0, debug.stderr
    assert "debug: (argument) selection = '" in debug.stderr
    assert "trace:" not in debug.stderr

    traced = run("-vvv", "--dry-run", f"s3://{bucket}/photo", env=s3_environment)
    assert traced.returncode == 0, traced.stderr
    assert any(
        " GET /delete-exact-key?" in line
        for line in traced.stderr.splitlines()
        if ": trace: http id=" in line
    )

    deleted = run("-v", f"s3://{bucket}/photo", env=s3_environment)
    assert deleted.returncode == 0, deleted.stderr
    assert "would delete" not in deleted.stderr
    assert "info: s3://delete-exact-key/photo deleted version=" in deleted.stderr
    assert keys(client, bucket) == ["photo/a", "photo1"]

    second = run(f"s3://{bucket}/photo/a", env=s3_environment)
    assert second.returncode == 0, second.stderr
    assert "success: 1 versions, 0 uploads, 0 buckets" in second.stderr
    assert "info:" not in second.stderr
    assert keys(client, bucket) == ["photo1"]


def test_delete_encodes_selection_and_object_names(s3_server, s3_environment):
    _endpoint, client = s3_server
    bucket = "delete-encoded-name"
    key = "folder/key name%"
    encoded_uri = f"s3://{bucket}/folder/key%20name%25"
    client.create_bucket(Bucket=bucket)
    client.put_object(Bucket=bucket, Key=key, Body=b"data")

    preview = run("-vv", "--dry-run", f"s3://{bucket}/{key}",
                  env=s3_environment)
    assert preview.returncode == 0, preview.stderr
    assert f"debug: (argument) selection = '{encoded_uri}'\n" in preview.stderr
    assert f"info: {encoded_uri} would delete version=" in preview.stderr
    assert "key name%" not in preview.stderr

    deleted = run("-v", f"s3://{bucket}/{key}", env=s3_environment)
    assert deleted.returncode == 0, deleted.stderr
    assert f"info: {encoded_uri} deleted version=" in deleted.stderr
    assert keys(client, bucket) == []


def test_slash_prefix_only_and_keeps_bucket(s3_server, s3_environment):
    _, client = s3_server
    bucket = "delete-slash-prefix"
    client.create_bucket(Bucket=bucket)
    for key in ("photo", "photo1", "photo/a", "photo/b"):
        client.put_object(Bucket=bucket, Key=key, Body=b"data")

    result = run(f"s3://{bucket}/photo/", env=s3_environment)
    assert result.returncode == 0, result.stderr
    assert keys(client, bucket) == ["photo", "photo1"]
    client.head_bucket(Bucket=bucket)


def test_exact_key_removes_all_versions_without_neighbors(s3_server, s3_environment):
    _, client = s3_server
    bucket = "delete-exact-versions"
    client.create_bucket(Bucket=bucket)
    client.put_bucket_versioning(
        Bucket=bucket, VersioningConfiguration={"Status": "Enabled"}
    )
    for body in (b"first", b"second"):
        client.put_object(Bucket=bucket, Key="photo", Body=body)
    client.delete_object(Bucket=bucket, Key="photo")
    client.put_object(Bucket=bucket, Key="photo/a", Body=b"keep")
    client.put_object(Bucket=bucket, Key="photo1", Body=b"keep")

    result = run(f"s3://{bucket}/photo", env=s3_environment)

    assert result.returncode == 0, result.stderr
    response = client.list_object_versions(Bucket=bucket)
    remaining = {
        item["Key"]
        for kind in ("Versions", "DeleteMarkers")
        for item in response.get(kind, [])
    }
    assert remaining == {"photo/a", "photo1"}


def test_delete_crosses_version_listing_pages(s3_server, s3_environment):
    _, client = s3_server
    bucket = "delete-many-objects"
    client.create_bucket(Bucket=bucket)
    for index in range(1001):
        client.put_object(Bucket=bucket, Key=f"photo/{index:04d}", Body=b"")
    client.put_object(Bucket=bucket, Key="outside", Body=b"keep")

    result = run("-vvv", f"s3://{bucket}/photo/", env=s3_environment)

    assert result.returncode == 0, result.stderr
    assert keys(client, bucket) == ["outside"]
    traces = [line for line in result.stderr.splitlines() if ": trace: http id=" in line]
    assert sum(" POST " in line and "?delete" in line for line in traces) == 2
    assert not any(" DELETE " in line for line in traces)


def test_bucket_delete_removes_versions_markers_and_uploads(
    s3_server, s3_environment
):
    _, client = s3_server
    bucket = "delete-versioned-bucket"
    client.create_bucket(Bucket=bucket)
    client.put_bucket_versioning(
        Bucket=bucket, VersioningConfiguration={"Status": "Enabled"}
    )
    client.put_object(Bucket=bucket, Key="photo", Body=b"first")
    client.put_object(Bucket=bucket, Key="photo", Body=b"second")
    client.delete_object(Bucket=bucket, Key="photo")
    upload = client.create_multipart_upload(Bucket=bucket, Key="unfinished")

    result = run("-v", f"s3://{bucket}", env=s3_environment)

    assert result.returncode == 0, result.stderr
    assert "3 versions" in result.stderr
    assert "1 uploads" in result.stderr
    assert f"info: s3://{bucket} deleted\n" in result.stderr
    with pytest.raises(ClientError) as caught:
        client.head_bucket(Bucket=bucket)
    assert caught.value.response["ResponseMetadata"]["HTTPStatusCode"] == 404
    assert upload["UploadId"]


def test_all_buckets_delete():
    # Moto's HTTP servers share the same in-process S3 backend. Start this
    # all-buckets test with an empty backend, regardless of earlier tests.
    for account_backend in get_backend("s3").values():
        account_backend.reset()
    server = make_server("127.0.0.1", 0, create_backend_app("s3"), True)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    endpoint = f"http://127.0.0.1:{server.server_port}"
    client = boto3.client(
        "s3",
        endpoint_url=endpoint,
        region_name="us-east-1",
        aws_access_key_id="test-access",
        aws_secret_access_key="test-secret",
    )
    environment = os.environ.copy()
    environment.update(
        {
            "S3AR_ENDPOINT": endpoint,
            "S3AR_URI_STYLE": "path",
            "S3AR_REGION": "us-east-1",
            "S3AR_ACCESS_KEY": "test-access",
            "S3AR_SECRET_KEY": "test-secret",
        }
    )
    try:
        for bucket in ("delete-all-a", "delete-all-b"):
            client.create_bucket(Bucket=bucket)
            client.put_object(Bucket=bucket, Key="key", Body=b"data")

        result = run("s3://", env=environment)

        assert result.returncode == 0, result.stderr
        assert client.list_buckets()["Buckets"] == []
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
