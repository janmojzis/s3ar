import io
from pathlib import Path
import subprocess
import tarfile
import urllib.parse

import pytest


RESTORE_PROBE = Path(__file__).parents[1] / "test-transform-restore"


def make_archive(path, buckets, misleading_names=False):
    with tarfile.open(path, "w", format=tarfile.PAX_FORMAT) as archive:
        for bucket, objects in buckets.items():
            entry = tarfile.TarInfo("decoy" if misleading_names else bucket)
            entry.type = tarfile.DIRTYPE
            entry.pax_headers = {
                "SCHILY.xattr.user.s3ar.format": "1",
                "SCHILY.xattr.user.s3ar.bucket": urllib.parse.quote(bucket, safe="/-._~"),
            }
            archive.addfile(entry)
            for key, data in objects.items():
                entry = tarfile.TarInfo(
                    "decoy/wrong" if misleading_names
                    else urllib.parse.quote(f"{bucket}/{key}", safe="/-._~")
                )
                entry.size = len(data)
                entry.pax_headers = {
                    "SCHILY.xattr.user.s3ar.format": "1",
                    "SCHILY.xattr.user.s3ar.bucket": urllib.parse.quote(bucket, safe="/-._~"),
                    "SCHILY.xattr.user.s3ar.hash": "none",
                    "SCHILY.xattr.user.s3ar.key": urllib.parse.quote(key, safe="/-._~"),
                    "SCHILY.xattr.user.s3ar.metadata.origin": "transform-test",
                }
                archive.addfile(entry, io.BytesIO(data))


def run(executable, *args, env=None):
    return subprocess.run(
        [str(executable), *args], env={} if env is None else env,
        capture_output=True, text=True, timeout=20,
    )


@pytest.mark.parametrize("expressions,key,expected", [
    (["s|^old/|new/|"], "key", "new/key"),
    (["s|^old/path/|old/new/path/|"], "path/key", "old/new/path/key"),
    (["s|^old/|middle/|", "s|^middle/|new/|"], "key", "new/key"),
    ([r"s|old/\(.*\)|new/\1-copy|"], "key", "new/key-copy"),
    (["s|key|&-copy|"], "key", "old/key-copy"),
    (["s|KEY|item|gi"], "key/key", "old/item/item"),
    (["s|old|new|"], "žluťoučký", "new/%C5%BElu%C5%A5ou%C4%8Dk%C3%BD"),
    (["s|x*|_|g"], "ž🙂", "_o_l_d_/_%C5%BE_%F0%9F%99%82_"),
    ([r"s|/\(ž\)|/\1-copy|"], "ž", "old/%C5%BE-copy"),
])
def test_transform_preview(executable, tmp_path, expressions, key, expected):
    archive = tmp_path / "input.tar"
    make_archive(archive, {"old": {key: b"data"}})
    args = ["-tf", str(archive)]
    for expression in expressions:
        args += ["--transform", expression]
    result = run(executable, *args)
    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines()[-1] == expected


@pytest.mark.parametrize("expression", [
    "s|/[^/]|/|", r"s|/\(.\).|/\1|", r"s|/.\(.\)|/\1|",
])
def test_transform_rejects_split_utf8_character(executable, tmp_path, expression):
    archive = tmp_path / "input.tar"
    make_archive(archive, {"old": {"ž": b"data"}})
    result = run(executable, "-tf", str(archive), "--transform", expression)
    assert result.returncode == 2
    assert "splits a UTF-8 character" in result.stderr
    assert "invalid transformed archive member" not in result.stderr
    assert result.stdout.splitlines() == ["old"]


def test_transform_selects_transformed_pax_identity(executable, tmp_path):
    archive = tmp_path / "input.tar"
    make_archive(archive, {"old": {"path/key": b"selected", "other": b"skip"}}, True)
    result = run(
        executable, "-tvvf", str(archive), "s3://new/path",
        "--transform=s|^old/|new/|",
    )
    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines()[0] == "new"
    assert result.stdout.splitlines()[1].startswith("new/path/key 8 ")
    assert "other" not in result.stdout
    assert "decoy" not in result.stdout
    assert "(argument) selection = 'new/path'" in result.stderr
    assert "debug: transform: old/path/key to new/path/key" in result.stderr
    assert "s3://" not in result.stderr


def test_transform_selects_destination_bucket_with_trailing_slash(executable, tmp_path):
    archive = tmp_path / "input.tar"
    make_archive(archive, {"uploads": {"uploads.tar": b"data"}, "other": {"key": b"skip"}})
    result = run(
        executable, "-t", "--transform=s|^uploads/|uploads2/|", "-vv",
        "-f", str(archive), "s3://uploads2/",
    )
    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines()[0] == "uploads2"
    assert result.stdout.splitlines()[1].startswith("uploads2/uploads.tar 4 ")
    assert len(result.stdout.splitlines()) == 2
    assert "not found in archive" not in result.stderr


@pytest.mark.parametrize("expression", [
    "s|old|new", "s|[|new|", "s|old|new|e", "s|old|new|S",
    r"s|old|\1|", r"s|old|\L&|", "s|old|new|;s|x|y|",
])
def test_transform_rejects_syntax_before_opening_archive(executable, expression):
    result = run(executable, "-tf", "/missing/archive.tar", "--transform", expression)
    assert result.returncode == 2
    assert "invalid --transform:" in result.stderr
    assert "cannot open archive" not in result.stderr


@pytest.mark.parametrize("expression", [
    "s|^old/|/|", "s|key$||", "s|key$|" + "x" * 1025 + "|",
])
def test_transform_validates_result(executable, tmp_path, expression):
    archive = tmp_path / "input.tar"
    make_archive(archive, {"old": {"key": b"data"}})
    result = run(executable, "-tf", str(archive), "--transform", expression)
    assert result.returncode == 2
    assert "invalid transformed archive member" in result.stderr


def test_transform_rejects_create(executable):
    result = run(executable, "-c", "s3://old", "--transform=s|old|new|")
    assert result.returncode == 2
    assert "--transform requires -x or -t" in result.stderr


def test_transform_requires_expression(executable):
    result = run(executable, "-t", "--transform")
    assert result.returncode == 2
    assert "option requires an argument --transform" in result.stderr


def test_transform_does_not_select_original_names(executable, tmp_path):
    archive = tmp_path / "input.tar"
    make_archive(archive, {"old": {"key": b"data"}})
    result = run(executable, "-tf", str(archive), "s3://old", "--transform=s|^old/|new/|")
    assert result.returncode == 2
    assert "not found in archive s3://old" in result.stderr
    assert result.stdout == ""


@pytest.mark.parametrize("expression,destination,expected_buckets", [
    ("s|^old/|new/|", "new/path/key", ["new"]),
    ("s|^old/path/|new/renamed/|", "new/renamed/key", ["new"]),
])
def test_transform_restore_streams_to_transformed_identity(
    tmp_path, expression, destination, expected_buckets,
):
    archive = tmp_path / "input.tar"
    data = b"selected content" * 10
    make_archive(archive, {"old": {"path/key": data, "other": b"skip"}}, True)
    environment = {
        "S3AR_ENDPOINT": "http://localhost", "S3AR_URI_STYLE": "path",
        "S3AR_REGION": "us-east-1", "S3AR_ACCESS_KEY": "unused",
        "S3AR_SECRET_KEY": "unused",
    }
    result = run(
        RESTORE_PROBE, "-xvf", str(archive), "s3://" + destination.rsplit("/", 1)[0],
        "--transform", expression, env=environment,
    )
    assert result.returncode == 0, result.stderr
    for bucket in expected_buckets:
        assert f"BUCKET {bucket}\n" in result.stdout
    assert "BUCKET old\n" not in result.stdout
    assert f"PUT {destination} size={len(data)}\n" in result.stdout
    assert "META origin=transform-test\n" in result.stdout
    assert data.decode() + "\n" in result.stdout
    assert "other" not in result.stdout
    assert f"s3ar: info: {destination} none\n" in result.stderr
    assert "s3://" not in result.stderr


def test_transform_restores_to_new_bucket(
    executable, s3_server, s3_environment, tmp_path,
):
    _, client = s3_server
    archive = tmp_path / "input.tar"
    make_archive(archive, {"transform-old": {"path/key": b"selected", "other": b"skip"}})
    result = run(
        executable, "-xvf", str(archive), "s3://transform-new/renamed",
        "--transform=s|^transform-old/|transform-new/|",
        "--transform=s|/path/|/renamed/|", env=s3_environment,
    )
    assert result.returncode == 0, result.stderr
    response = client.get_object(Bucket="transform-new", Key="renamed/key")
    assert response["Body"].read() == b"selected"
    assert response["Metadata"] == {"origin": "transform-test"}
    objects = client.list_objects_v2(Bucket="transform-new")["Contents"]
    assert [item["Key"] for item in objects] == ["renamed/key"]
    assert "s3ar: info: transform-new/renamed/key none" in result.stderr
    assert "s3://" not in result.stderr


@pytest.mark.parametrize("merge_buckets", [False, True])
def test_restore_many_interleaved_buckets(tmp_path, merge_buckets):
    path = tmp_path / "many-buckets.tar"
    buckets = [f"bucket-{i:04d}" for i in range(128)]
    with tarfile.open(path, "w", format=tarfile.PAX_FORMAT) as archive:
        # Repeated directory entries must not initialize a bucket twice.
        for bucket in buckets + list(reversed(buckets)):
            entry = tarfile.TarInfo(bucket)
            entry.type = tarfile.DIRTYPE
            entry.pax_headers = {
                "SCHILY.xattr.user.s3ar.format": "1",
                "SCHILY.xattr.user.s3ar.bucket": bucket,
            }
            archive.addfile(entry)
        for round_number in range(3):
            for bucket in reversed(buckets):
                key = f"{bucket}-{round_number}"
                entry = tarfile.TarInfo(f"{bucket}/{key}")
                entry.pax_headers = {
                    "SCHILY.xattr.user.s3ar.format": "1",
                    "SCHILY.xattr.user.s3ar.bucket": bucket,
                    "SCHILY.xattr.user.s3ar.hash": "none",
                    "SCHILY.xattr.user.s3ar.key": key,
                }
                archive.addfile(entry)
    environment = {
        "S3AR_ENDPOINT": "http://localhost", "S3AR_URI_STYLE": "path",
        "S3AR_REGION": "us-east-1", "S3AR_ACCESS_KEY": "unused",
        "S3AR_SECRET_KEY": "unused",
    }
    args = ["-xf", str(path), "s3://"]
    if merge_buckets:
        args += ["--transform", "s|^bucket-[0-9]*/|merged/|"]
    result = run(RESTORE_PROBE, *args, env=environment)
    assert result.returncode == 0, result.stderr
    lines = result.stdout.splitlines()
    initialized = [line for line in lines if line.startswith("BUCKET ")]
    expected = ["BUCKET merged"] if merge_buckets else [
        f"BUCKET {bucket}" for bucket in buckets
    ]
    assert initialized == expected
    uploaded = [line for line in lines if line.startswith("PUT ")]
    assert uploaded == [
        f"PUT {'merged' if merge_buckets else bucket}/{bucket}-{round_number} size=0"
        for round_number in range(3) for bucket in reversed(buckets)
    ]
