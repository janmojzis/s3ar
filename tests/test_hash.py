import hashlib
import io
from pathlib import Path
import subprocess
import tarfile

import pytest

from fault_server import FaultServer, ResponseStep


def write_archive(path, body, value, legacy=False):
    with tarfile.open(path, "w", format=tarfile.PAX_FORMAT) as archive:
        directory = tarfile.TarInfo("hash-test")
        directory.type = tarfile.DIRTYPE
        directory.pax_headers = {"SCHILY.xattr.user.s3ar.bucket": "hash-test"}
        archive.addfile(directory)
        entry = tarfile.TarInfo("hash-test/key")
        entry.size = len(body)
        entry.pax_headers = {
            "SCHILY.xattr.user.s3ar.bucket": "hash-test",
            "SCHILY.xattr.user.s3ar.key": "key",
        }
        if not legacy:
            entry.pax_headers["SCHILY.xattr.user.s3ar.format"] = "1"
            entry.pax_headers["SCHILY.xattr.user.s3ar.hash"] = value
        archive.addfile(entry, io.BytesIO(body))


def invoke(executable, env, *args):
    return subprocess.run([str(executable), *args], env=env,
                          capture_output=True, text=True, timeout=30)


@pytest.mark.parametrize("enabled", [False, True])
def test_create_hash_is_opt_in(executable, s3_server, s3_environment, tmp_path, enabled):
    _, client = s3_server
    client.create_bucket(Bucket="hash-test")
    client.put_object(Bucket="hash-test", Key="key", Body=b"abc")
    path = tmp_path / "archive.tar"
    result = invoke(executable, s3_environment, "-cvf", str(path),
                    *(["--hash"] if enabled else []), "s3://hash-test/key")
    assert result.returncode == 0, result.stderr
    expected = "sha512:" + hashlib.sha512(b"abc").hexdigest() if enabled else "none"
    with tarfile.open(path) as archive:
        assert archive.getmember("hash-test/key").pax_headers[
            "SCHILY.xattr.user.s3ar.hash"] == expected
    assert result.stderr.rstrip().endswith(" " + expected)
    assert "warning:" not in result.stderr


@pytest.mark.parametrize("size", [0, 3, 16 * 1024 * 1024 + 3])
@pytest.mark.parametrize("matching, enabled", [(True, True), (False, True), (False, False)])
def test_restore_hash_preserves_destination_on_mismatch(
    executable, s3_server, s3_environment, tmp_path, size, matching, enabled
):
    _, client = s3_server
    client.create_bucket(Bucket="hash-test")
    client.put_object(Bucket="hash-test", Key="key", Body=b"original")
    body = b"x" * size
    value = "sha512:" + (hashlib.sha512(body).hexdigest() if matching else "0" * 128)
    path = tmp_path / "archive.tar"
    write_archive(path, body, value)
    result = invoke(executable, s3_environment, "-xvf", str(path),
                    *(["--hash"] if enabled else []), "s3://")
    restored = client.get_object(Bucket="hash-test", Key="key")["Body"].read()
    if enabled and not matching:
        assert result.returncode == 2
        assert "SHA-512 mismatch for hash-test/key" in result.stderr
        assert restored == b"original"
        assert "verified" not in result.stderr
        assert not client.list_multipart_uploads(Bucket="hash-test").get("Uploads")
    else:
        assert result.returncode == 0, result.stderr
        assert restored == body
        assert result.stderr.rstrip().endswith(value + (" verified" if enabled else ""))


@pytest.mark.parametrize("legacy", [False, True])
def test_restore_without_digest_warns(executable, s3_server, s3_environment, tmp_path, legacy):
    path = tmp_path / "archive.tar"
    write_archive(path, b"abc", "none", legacy)
    result = invoke(executable, s3_environment, "-xvf", str(path), "--hash", "s3://")
    assert result.returncode == 0, result.stderr
    assert result.stderr.count("warning: hash unavailable") == 1
    assert result.stderr.rstrip().endswith(" none unverified")
    _, client = s3_server
    assert client.get_object(Bucket="hash-test", Key="key")["Body"].read() == b"abc"


@pytest.mark.parametrize("matching", [True, False])
def test_multipart_hash_retry_and_abort(executable, s3_environment, tmp_path, matching):
    body = b"x" * (16 * 1024 * 1024 + 3)
    digest = hashlib.sha512(body).hexdigest() if matching else "0" * 128
    path = tmp_path / "archive.tar"
    write_archive(path, body, "sha512:" + digest)
    close = (("Connection", "close"),)
    part = close + (("ETag", '"part"'),)
    base = "/hash-test/key"
    steps = [
        ResponseStep("HEAD", "/hash-test", 200, headers=close),
        ResponseStep("POST", base + "?uploads", 200,
                     b"<InitiateMultipartUploadResult><UploadId>id</UploadId>"
                     b"</InitiateMultipartUploadResult>", close),
        ResponseStep("PUT", base + "?partNumber=1&uploadId=id", 503,
                     b"<Error><Code>SlowDown</Code></Error>", close),
        ResponseStep("PUT", base + "?partNumber=1&uploadId=id", 200, headers=part),
        ResponseStep("PUT", base + "?partNumber=2&uploadId=id", 200, headers=part),
    ]
    if matching:
        steps.append(ResponseStep("POST", base + "?uploadId=id", 200,
                                  b"<CompleteMultipartUploadResult/>", close))
    else:
        steps.append(ResponseStep("DELETE", base + "?uploadId=id", 204, headers=close))
    with FaultServer(steps) as server:
        result = invoke(executable, {**s3_environment, "S3AR_ENDPOINT": server.endpoint},
                        "-xvf", str(path), "--hash", "s3://")
    assert result.returncode == (0 if matching else 2), result.stderr
    assert (" verified" if matching else "SHA-512 mismatch") in result.stderr


def test_hash_rejected_for_listing(executable):
    result = invoke(executable, {}, "-t", "--hash", "-f", "/missing")
    assert result.returncode == 2
    assert "--hash requires -c or -x" in result.stderr


@pytest.mark.parametrize("abort_failed", [False, True])
def test_reader_uses_abort_result_instead_of_diagnostic_text(tmp_path, abort_failed):
    path = tmp_path / "hash-mismatch.tar"
    write_archive(path, b"data", "sha512:" + "0" * 128)
    environment = {
        "S3AR_ENDPOINT": "http://localhost", "S3AR_URI_STYLE": "path",
        "S3AR_REGION": "us-east-1", "S3AR_ACCESS_KEY": "unused",
        "S3AR_SECRET_KEY": "unused",
        "S3AR_TEST_ABORT_FAILURE": "yes" if abort_failed else "no",
    }
    probe = Path(__file__).resolve().parents[1] / "test-transform-restore"
    result = invoke(probe, environment, "-x", "--hash", "-f", str(path), "s3://")
    assert result.returncode == 2
    if abort_failed:
        assert "unable to clean up multipart upload" in result.stderr
        assert "cleanup refused" in result.stderr
        assert "SHA-512 mismatch for" not in result.stderr
    else:
        assert "SHA-512 mismatch for" in result.stderr
        assert "unable to clean up multipart upload" not in result.stderr
