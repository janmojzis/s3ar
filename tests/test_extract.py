import io
import shutil
import subprocess
import tarfile

import botocore.exceptions
import pytest


def run(executable, *arguments, env=None):
    return subprocess.run(
        [str(executable), *arguments],
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )


def zstd_bytes(data):
    if shutil.which("zstd") is None:
        pytest.skip("zstd command is required for interoperability tests")
    result = subprocess.run(
        ["zstd", "-q", "-c"],
        input=data,
        capture_output=True,
        check=False,
    )
    assert result.returncode == 0, result.stderr.decode()
    return result.stdout


def archive_bytes(buckets):
    output = io.BytesIO()
    with tarfile.open(fileobj=output, mode="w", format=tarfile.PAX_FORMAT) as archive:
        for bucket, objects in buckets.items():
            for key, data in objects.items():
                entry = tarfile.TarInfo(f"{bucket}/{key}")
                entry.size = len(data)
                entry.pax_headers = {
                    "SCHILY.xattr.user.origin": "archive",
                }
                archive.addfile(entry, io.BytesIO(data))
    return output.getvalue()


def test_create_extract_round_trip_into_original_bucket(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="roundtrip-source")
    client.put_object(
        Bucket="roundtrip-source",
        Key="folder/item",
        Body=b"round trip data",
        Metadata={"owner": "s3ar"},
    )
    archive = tmp_path / "roundtrip.tar"
    created = run(
        executable,
        "-c",
        "-f",
        str(archive),
        "s3://roundtrip-source/",
        env=s3_environment,
    )
    assert created.returncode == 0, created.stderr
    client.delete_object(Bucket="roundtrip-source", Key="folder/item")

    extracted = run(
        executable,
        "-x",
        "-f",
        str(archive),
        "s3://roundtrip-source",
        env=s3_environment,
    )

    assert extracted.returncode == 0, extracted.stderr
    assert extracted.stderr == ""
    restored = client.get_object(Bucket="roundtrip-source", Key="folder/item")
    assert restored["Body"].read() == b"round trip data"
    assert restored["Metadata"] == {"owner": "s3ar"}


def test_extract_explicit_zstd_archive(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="extract-zstd")
    archive = tmp_path / "restore.tar.zst"
    archive.write_bytes(
        zstd_bytes(archive_bytes({"extract-zstd": {"object": b"zstd data"}}))
    )

    result = run(
        executable,
        "-x",
        "--zstd",
        "-f",
        str(archive),
        "s3://extract-zstd/",
        env=s3_environment,
    )

    assert result.returncode == 0, result.stderr
    restored = client.get_object(Bucket="extract-zstd", Key="object")
    assert restored["Body"].read() == b"zstd data"


def test_extract_all_buckets(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    archive = tmp_path / "all-restore.tar"
    archive.write_bytes(
        archive_bytes(
            {
                "restore-first": {"one": b"first"},
                "restore-full": {"path/file": b"restored"},
            }
        )
    )

    result = run(
        executable, "-x", "-f", str(archive), "s3://", env=s3_environment
    )

    assert result.returncode == 0, result.stderr
    first = client.get_object(Bucket="restore-first", Key="one")
    assert first["Body"].read() == b"first"
    restored = client.get_object(Bucket="restore-full", Key="path/file")
    assert restored["Body"].read() == b"restored"
    assert restored["Metadata"] == {"origin": "archive"}


def test_extract_bucket_filter_skips_other_buckets(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    archive = tmp_path / "multiple.tar"
    archive.write_bytes(
        archive_bytes({"filter-skip-first": {"same": b"1"}, "filter-skip-second": {"same": b"2"}})
    )

    result = run(
        executable,
        "-x",
        "-f",
        str(archive),
        "s3://filter-skip-second/",
        env=s3_environment,
    )

    assert result.returncode == 0, result.stderr
    restored = client.get_object(Bucket="filter-skip-second", Key="same")
    assert restored["Body"].read() == b"2"
    with pytest.raises(botocore.exceptions.ClientError):
        client.head_bucket(Bucket="filter-skip-first")


def test_invalid_archive_is_rejected_before_target_bucket_is_created(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    archive = tmp_path / "invalid.tar"
    data = io.BytesIO()
    with tarfile.open(fileobj=data, mode="w", format=tarfile.PAX_FORMAT) as tar:
        entry = tarfile.TarInfo("missing-key")
        entry.size = 1
        tar.addfile(entry, io.BytesIO(b"x"))
    archive.write_bytes(data.getvalue())

    result = run(
        executable,
        "-x",
        "-f",
        str(archive),
        "s3://",
        env=s3_environment,
    )

    assert result.returncode != 0
    try:
        client.head_bucket(Bucket="missing-key")
    except botocore.exceptions.ClientError as error:
        assert error.response["ResponseMetadata"]["HTTPStatusCode"] == 404
    else:
        raise AssertionError("bucket was created for an invalid archive")


def test_extract_rejects_s3_tarfile(executable):
    result = run(
        executable,
        "-x",
        "-f",
        "s3://extract-archives/source.tar",
        "s3://remote-original/",
    )

    assert result.returncode == 2
    assert "TARFILE must be a local filesystem path or '-'" in result.stderr


def test_extract_without_f_reads_tar_from_stdin(
    executable, s3_server, s3_environment
):
    _endpoint, client = s3_server
    data = archive_bytes({"stdin-original": {"path/item": b"stdin data"}})

    result = subprocess.run(
        [str(executable), "-x", "s3://stdin-original/"],
        env=s3_environment,
        input=data,
        capture_output=True,
        check=False,
    )

    assert result.returncode == 0, result.stderr.decode()
    assert result.stderr == b""
    restored = client.get_object(Bucket="stdin-original", Key="path/item")
    assert restored["Body"].read() == b"stdin data"


def test_extract_with_f_dash_reads_tar_from_stdin(
    executable, s3_server, s3_environment
):
    _endpoint, client = s3_server
    data = archive_bytes({"stdin-f-dash": {"path/item": b"explicit stdin"}})

    result = subprocess.run(
        [str(executable), "-xf", "-", "s3://stdin-f-dash/"],
        env=s3_environment,
        input=data,
        capture_output=True,
        check=False,
    )

    assert result.returncode == 0, result.stderr.decode()
    assert result.stderr == b""
    restored = client.get_object(Bucket="stdin-f-dash", Key="path/item")
    assert restored["Body"].read() == b"explicit stdin"


def test_verbose_extract_lists_restored_objects(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    archive = tmp_path / "verbose-objects.tar"
    archive.write_bytes(
        archive_bytes(
            {
                "verbose-objects": {
                    "first": b"1",
                    "folder/second": b"2",
                }
            }
        )
    )

    result = run(
        executable,
        "-x",
        "-v",
        "-f",
        str(archive),
        "s3://verbose-objects/",
        env=s3_environment,
    )

    assert result.returncode == 0, result.stderr
    assert result.stdout == ""
    assert result.stderr == (
        "verbose-objects/first\nverbose-objects/folder/second\n"
    )


def test_extract_prefix_restores_only_matching_keys(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    archive = tmp_path / "prefix.tar"
    archive.write_bytes(
        archive_bytes(
            {
                "prefix-restore": {
                    "album/photos/a.jpg": b"a",
                    "album/photos/b.jpg": b"b",
                    "album/photos-old/c.jpg": b"c",
                    "other/d.jpg": b"d",
                }
            }
        )
    )

    result = run(
        executable,
        "-x",
        "-f",
        str(archive),
        "s3://prefix-restore/album/photos/",
        env=s3_environment,
    )

    assert result.returncode == 0, result.stderr
    contents = client.list_objects_v2(Bucket="prefix-restore")["Contents"]
    assert {item["Key"] for item in contents} == {
        "album/photos/a.jpg",
        "album/photos/b.jpg",
    }


def test_extract_multiple_filters_deduplicate_overlaps(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    archive = tmp_path / "multiple-filters.tar"
    archive.write_bytes(
        archive_bytes(
            {
                "extract-multiple-first": {
                    "selected/a": b"a",
                    "selected/deeper/b": b"b",
                    "outside": b"outside",
                },
                "extract-multiple-second": {"selected/c": b"c"},
            }
        )
    )

    result = run(
        executable,
        "-xvf",
        str(archive),
        "s3://extract-multiple-first/selected/",
        "s3://extract-multiple-first/selected/deeper/",
        "s3://extract-multiple-second/selected/",
        env=s3_environment,
    )

    assert result.returncode == 0, result.stderr
    assert result.stdout == ""
    assert result.stderr.splitlines() == [
        "extract-multiple-first/selected/a",
        "extract-multiple-first/selected/deeper/b",
        "extract-multiple-second/selected/c",
    ]
    first = client.list_objects_v2(Bucket="extract-multiple-first")[
        "Contents"
    ]
    second = client.list_objects_v2(Bucket="extract-multiple-second")[
        "Contents"
    ]
    assert {item["Key"] for item in first} == {
        "selected/a",
        "selected/deeper/b",
    }
    assert {item["Key"] for item in second} == {"selected/c"}


def test_extract_without_filters_restores_all_members(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    archive = tmp_path / "all-members.tar"
    archive.write_bytes(
        archive_bytes(
            {
                "extract-all-first": {"a": b"a"},
                "extract-all-second": {"b": b"b"},
            }
        )
    )

    result = run(
        executable,
        "-xf",
        str(archive),
        env=s3_environment,
    )

    assert result.returncode == 0, result.stderr
    first = client.get_object(Bucket="extract-all-first", Key="a")
    second = client.get_object(Bucket="extract-all-second", Key="b")
    assert first["Body"].read() == b"a"
    assert second["Body"].read() == b"b"
