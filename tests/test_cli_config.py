import subprocess

import pytest


@pytest.mark.parametrize("tool,options", [
    ("s3ar", ["-c", "-f", "{file}", "s3://bucket"]),
    ("s3ar", ["-x", "-f", "{file}", "s3://bucket"]),
    ("s3ar-put", ["-f", "{file}", "s3://bucket/key"]),
    ("s3ar-get", ["-f", "{file}", "s3://bucket/key"]),
    ("s3ar-copy", ["s3://bucket/key", "s3://bucket/destination"]),
    ("s3ar-delete", ["--dry-run", "s3://bucket"]),
    ("s3ar-list", ["s3://bucket"]),
])
@pytest.mark.parametrize("failure", ["missing_endpoint", "invalid_style", "invalid_endpoint"])
def test_cli_initialization_errors(executable, tmp_path, tool, options, failure):
    environment = {
        "S3AR_ENDPOINT": "http://127.0.0.1:1",
        "S3AR_REGION": "us-east-1",
        "S3AR_ACCESS_KEY": "test-access",
        "S3AR_SECRET_KEY": "test-secret",
    }
    if failure == "missing_endpoint":
        del environment["S3AR_ENDPOINT"]
        message = "invalid configuration: $S3AR_ENDPOINT not set"
    elif failure == "invalid_style":
        environment["S3AR_URI_STYLE"] = "invalid"
        message = "invalid configuration: $S3AR_URI_STYLE must be 'path' or 'virtual'"
    else:
        environment["S3AR_ENDPOINT"] = "invalid-endpoint"
        message = "unable to initialize S3 client: "
    target = tmp_path / "data"
    target.write_bytes(b"original data")
    result = subprocess.run(
        [str(executable.with_name(tool)), *[arg.replace("{file}", str(target)) for arg in options]],
        env=environment, capture_output=True, text=True, timeout=5,
    )
    expected_status = 1 if tool == "s3ar-list" and failure == "invalid_endpoint" else 2
    assert result.returncode == expected_status
    assert result.stdout == ""
    assert result.stderr.startswith(f"{tool}: fatal: {message}")
    assert len(result.stderr.splitlines()) == 1
    assert target.read_bytes() == b"original data"
    assert list(tmp_path.iterdir()) == [target]


@pytest.mark.parametrize("options,message", [
    (["-c", "--multipart-size"], "option requires an argument --multipart-size"),
    (["-c", "--multipart-size", "4M"], "--multipart-size must be between 5M and 5G"),
    (["-x", "--multipart-size", "6G"], "--multipart-size must be between 5M and 5G"),
    (["-c", "--multipart-size", "5M", "--multipart-size", "16M"],
     "multipart size specified twice"),
    (["-t", "--multipart-size", "5M"], "--multipart-size requires -c or -x"),
])
def test_archive_multipart_size_invalid(executable, options, message):
    result = subprocess.run([str(executable), *options], env={},
                            capture_output=True, text=True, timeout=5)
    assert result.returncode == 2
    assert message in result.stderr
