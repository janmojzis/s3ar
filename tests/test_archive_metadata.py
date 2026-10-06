import base64
import io
from pathlib import Path
import subprocess
import tarfile

import pytest


PROBE = Path(__file__).resolve().parents[1] / "test-transform-restore"
ENVIRONMENT = {
    "S3AR_ENDPOINT": "http://localhost", "S3AR_URI_STYLE": "path",
    "S3AR_REGION": "us-east-1", "S3AR_ACCESS_KEY": "unused",
    "S3AR_SECRET_KEY": "unused",
}
PREFIX = "SCHILY.xattr.user."


def run_archive(tmp_path, attributes, mode="-x", selection="s3://",
                bucket_attributes=None, missing_bucket_format=False):
    path = tmp_path / "metadata.tar"
    with tarfile.open(path, "w", format=tarfile.PAX_FORMAT) as archive:
        bucket = tarfile.TarInfo("misleading")
        bucket.type = tarfile.DIRTYPE
        bucket.pax_headers = {PREFIX + "s3ar.format": "1",
                              PREFIX + "s3ar.bucket": "metadata-test",
                              **(bucket_attributes or {})}
        if missing_bucket_format:
            del bucket.pax_headers[PREFIX + "s3ar.format"]
        archive.addfile(bucket)
        entry = tarfile.TarInfo("misleading/object")
        entry.size = 4
        entry.pax_headers = {
            PREFIX + "s3ar.bucket": "metadata-test",
            PREFIX + "s3ar.key": "key",
            **attributes,
        }
        archive.addfile(entry, io.BytesIO(b"data"))
    return subprocess.run(
        [str(PROBE), mode, "-f", str(path), selection], env=ENVIRONMENT,
        capture_output=True, text=True, timeout=10,
    )


@pytest.mark.parametrize("format_first", [False, True])
def test_metadata_namespace_and_attribute_order(tmp_path, format_first):
    attributes = {
        PREFIX + "origin": "ignored",
        PREFIX + "s3ar.metadata.origin": "namespaced",
        PREFIX + "s3ar.metadata.empty": "",
    }
    marker = {PREFIX + "s3ar.format": "1", PREFIX + "s3ar.hash": "none"}
    attributes = {**marker, **attributes} if format_first else {**attributes, **marker}
    result = run_archive(tmp_path, attributes)
    assert result.returncode == 0, result.stderr
    actual = {line for line in result.stdout.splitlines() if line.startswith("META ")}
    assert actual == {"META origin=namespaced", "META empty="}
    assert "PUT metadata-test/key size=4\n" in result.stdout
    assert result.stdout.endswith("data\n")


@pytest.mark.parametrize("count", [0, 128, 129])
def test_restore_metadata_count_limit(tmp_path, count):
    attributes = {PREFIX + "s3ar.metadata." + f"field{i}": "value" for i in range(count)}
    attributes.update({PREFIX + "s3ar.hash": "none", PREFIX + "s3ar.format": "1"})
    result = run_archive(tmp_path, attributes)
    if count > 128:
        assert result.returncode == 2
        assert "too many object metadata fields" in result.stderr
        assert "PUT " not in result.stdout
    else:
        assert result.returncode == 0, result.stderr
        assert result.stdout.count("META ") == count
        assert result.stdout.endswith("data\n")


@pytest.mark.parametrize("mode,selection", [
    ("-t", "s3://"), ("-tv", "s3://"), ("-x", "s3://metadata-test/absent"),
])
def test_metadata_not_loaded_for_listing_or_unselected_objects(tmp_path, mode, selection):
    attributes = {PREFIX + "s3ar.metadata." + f"field{i}": "bad\r\nvalue" for i in range(129)}
    attributes.update({PREFIX + "s3ar.hash": "none", PREFIX + "s3ar.format": "1"})
    result = run_archive(tmp_path, attributes, mode, selection)
    assert "too many object metadata" not in result.stderr
    assert "invalid object metadata" not in result.stderr
    assert "PUT " not in result.stdout
    if mode.startswith("-t"):
        assert result.returncode == 0, result.stderr
        assert "metadata-test/key" in result.stdout
    else:
        assert result.returncode == 2
        assert "not found in archive" in result.stderr


@pytest.mark.parametrize("field,value,message", [
    ("format", "2", "unsupported archive metadata format"),
    ("hash", "invalid", "invalid or duplicate object hash"),
    ("key", "%00", "invalid URL-encoded PAX header"),
    ("metadata.bad", "bad\r\nvalue", "invalid object metadata"),
])
def test_invalid_archive_attributes_rejected(tmp_path, field, value, message):
    attributes = {PREFIX + "s3ar.format": "1", PREFIX + "s3ar.hash": "none",
                  PREFIX + "s3ar." + field: value}
    result = run_archive(tmp_path, attributes)
    assert result.returncode == 2
    assert message in result.stderr
    assert "PUT " not in result.stdout


@pytest.mark.parametrize("field,message", [
    ("key", "duplicate S3 identity PAX header"),
    ("hash", "invalid or duplicate object hash"),
])
def test_duplicate_attributes_rejected(tmp_path, field, message):
    value = "key" if field == "key" else "none"
    attributes = {PREFIX + "s3ar.format": "1", PREFIX + "s3ar.hash": "none",
                  "LIBARCHIVE.xattr.user.s3ar." + field:
                      base64.b64encode(value.encode()).decode()}
    result = run_archive(tmp_path, attributes)
    assert result.returncode == 2
    assert message in result.stderr


@pytest.mark.parametrize("mode", ["-t", "-tv", "-x"])
def test_etag_validation_only_for_verbose_listing(tmp_path, mode):
    attributes = {PREFIX + "s3ar.format": "1", PREFIX + "s3ar.hash": "none",
                  PREFIX + "s3ar.etag": "bad\r\nvalue"}
    result = run_archive(tmp_path, attributes, mode)
    if mode == "-tv":
        assert result.returncode == 2
        assert "invalid object ETag" in result.stderr
    else:
        assert result.returncode == 0, result.stderr


@pytest.mark.parametrize("mode", ["-t", "-x"])
def test_missing_hash_rejected_even_for_unselected_object(tmp_path, mode):
    result = run_archive(tmp_path, {PREFIX + "s3ar.format": "1"}, mode,
                         "s3://metadata-test/absent")
    assert result.returncode == 2
    assert "missing object hash in format-1 archive" in result.stderr


def test_ignored_namespace_does_not_consume_metadata_limit(tmp_path):
    attributes = {PREFIX + f"field{i}": "ignored" for i in range(200)}
    attributes.update({PREFIX + "s3ar.metadata.origin": "kept",
                       PREFIX + "s3ar.hash": "none", PREFIX + "s3ar.format": "1"})
    result = run_archive(tmp_path, attributes)
    assert result.returncode == 0, result.stderr
    assert result.stdout.count("META ") == 1
    assert "META origin=kept\n" in result.stdout


def test_valid_etag_and_hash_in_verbose_listing(tmp_path):
    digest = "sha512:" + "0" * 128
    attributes = {PREFIX + "s3ar.etag": '"etag"', PREFIX + "s3ar.hash": digest,
                  PREFIX + "s3ar.format": "1"}
    result = run_archive(tmp_path, attributes, "-tv")
    assert result.returncode == 0, result.stderr
    assert f'metadata-test/key 4 0 "etag" {digest}\n' in result.stdout


@pytest.mark.parametrize("member", ["bucket", "object"])
@pytest.mark.parametrize("values", [("1", "1"), ("1", "2"), ("2", "1")])
@pytest.mark.parametrize("libarchive_first", [False, True])
@pytest.mark.parametrize("mode,selection", [
    ("-t", "s3://"), ("-x", "s3://"), ("-x", "s3://metadata-test/absent"),
])
def test_duplicate_format_markers_rejected(
    tmp_path, member, values, libarchive_first, mode, selection
):
    schily, libarchive = values
    markers = {
        PREFIX + "s3ar.format": schily,
        "LIBARCHIVE.xattr.user.s3ar.format":
            base64.b64encode(libarchive.encode()).decode(),
    }
    if libarchive_first:
        markers = dict(reversed(list(markers.items())))
    attributes = {PREFIX + "s3ar.hash": "none"}
    attributes.update(markers if member == "object" else {PREFIX + "s3ar.format": "1"})
    result = run_archive(tmp_path, attributes, mode, selection,
                         bucket_attributes=markers if member == "bucket" else None)
    assert result.returncode == 2
    assert "duplicate archive metadata format" in result.stderr
    assert "PUT " not in result.stdout
    assert "metadata-test/key" not in result.stdout


@pytest.mark.parametrize("mode", ["-t", "-x"])
@pytest.mark.parametrize("version", ["1", "2"])
def test_single_bucket_format_marker(tmp_path, mode, version):
    attributes = {PREFIX + "s3ar.format": "1", PREFIX + "s3ar.hash": "none"}
    result = run_archive(tmp_path, attributes, mode,
                         bucket_attributes={PREFIX + "s3ar.format": version})
    assert result.returncode == (0 if version == "1" else 2), result.stderr
    if version == "2":
        assert "unsupported archive metadata format" in result.stderr
        assert "PUT " not in result.stdout
    elif mode == "-x":
        assert "PUT metadata-test/key size=4" in result.stdout
    else:
        assert "metadata-test/key" in result.stdout


@pytest.mark.parametrize("member", ["bucket", "object"])
@pytest.mark.parametrize("mode,selection", [
    ("-t", "s3://"), ("-x", "s3://"), ("-x", "s3://metadata-test/absent"),
])
def test_missing_format_rejected(tmp_path, member, mode, selection):
    attributes = {PREFIX + "s3ar.hash": "none", PREFIX + "origin": "old metadata"}
    if member == "bucket":
        attributes[PREFIX + "s3ar.format"] = "1"
    result = run_archive(tmp_path, attributes, mode, selection,
                         missing_bucket_format=member == "bucket")
    assert result.returncode == 2
    assert "missing archive metadata format" in result.stderr
    assert "PUT " not in result.stdout
