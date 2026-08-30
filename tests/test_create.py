import io
import shutil
import stat
import subprocess
import tarfile

import pytest


def run(executable, *arguments, cwd=None, env=None, umask=-1):
    return subprocess.run(
        [str(executable), *arguments],
        cwd=cwd,
        env=env,
        umask=umask,
        text=True,
        capture_output=True,
        check=False,
    )


def decompress_zstd(path):
    if shutil.which("zstd") is None:
        pytest.skip("zstd command is required for interoperability tests")
    result = subprocess.run(
        ["zstd", "-q", "-d", "-c", str(path)],
        capture_output=True,
        check=False,
    )
    assert result.returncode == 0, result.stderr.decode()
    return result.stdout


def members_by_name(path):
    with tarfile.open(path, "r:") as archive:
        return {member.name: member for member in archive.getmembers()}


def raw_tar_entries(path):
    data = path.read_bytes()
    entries = []
    offset = 0
    while data[offset : offset + 512] != bytes(512):
        header = data[offset : offset + 512]
        assert len(header) == 512
        stored_checksum = int(header[148:156].rstrip(b"\0 ") or b"0", 8)
        checksum_header = bytearray(header)
        checksum_header[148:156] = b" " * 8
        assert sum(checksum_header) == stored_checksum
        size = int(header[124:136].rstrip(b"\0 ") or b"0", 8)
        name = header[:100].split(b"\0", 1)[0]
        prefix = header[345:500].split(b"\0", 1)[0]
        if prefix:
            name = prefix + b"/" + name
        payload = data[offset + 512 : offset + 512 + size]
        entries.append((name, header[156:157], payload))
        offset += 512 + ((size + 511) // 512) * 512
    assert data[offset : offset + 1024] == bytes(1024)
    return entries


def test_create_single_bucket_with_full_path_and_metadata(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="create-single")
    client.put_object(
        Bucket="create-single",
        Key="folder/object.txt",
        Body=b"object data",
        Metadata={"source": "create-test"},
    )
    target = tmp_path / "single.tar"

    result = run(
        executable,
        "-c",
        "-f",
        str(target),
        "s3://create-single",
        env=s3_environment,
    )

    assert result.returncode == 0, result.stderr
    members = members_by_name(target)
    assert set(members) == {"create-single", "create-single/folder/object.txt"}
    assert not any(
        name.startswith("LIBARCHIVE.xattr.")
        for member in members.values()
        for name in member.pax_headers
    )
    item = members["create-single/folder/object.txt"]
    bucket = members["create-single"]
    assert bucket.isdir()
    assert bucket.pax_headers["SCHILY.xattr.s3ar.bucket-acl"] == "unavailable"
    assert item.size == len(b"object data")
    assert "SCHILY.xattr.bucket" not in item.pax_headers
    assert item.pax_headers["SCHILY.xattr.user.source"] == "create-test"

    raw = raw_tar_entries(target)
    object_pax = next(
        payload for _name, kind, payload in raw
        if kind == b"x" and b"SCHILY.xattr.user.source=" in payload
    )
    assert b"LIBARCHIVE.xattr." not in object_pax
    assert b"SCHILY.xattr.bucket=" not in object_pax
    assert b"SCHILY.xattr.user.source=create-test\n" in object_pax
    object_header = next(
        name for name, kind, _payload in raw if kind == b"0" and name == b"create-single/folder/object.txt"
    )
    assert object_header == b"create-single/folder/object.txt"

def test_create_zstd_archive(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="create-zstd")
    client.put_object(Bucket="create-zstd", Key="object", Body=b"zstd data")
    target = tmp_path / "backup.tar.zst"

    result = run(
        executable,
        "-c",
        "--zstd",
        "-f",
        str(target),
        "s3://create-zstd/",
        env=s3_environment,
    )

    assert result.returncode == 0, result.stderr
    assert target.read_bytes().startswith(b"\x28\xb5\x2f\xfd")
    with tarfile.open(
        fileobj=io.BytesIO(decompress_zstd(target)), mode="r:"
    ) as archive:
        assert archive.getnames() == ["create-zstd", "create-zstd/object"]

def test_create_all_buckets_uses_full_paths_and_includes_empty_buckets(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="create-empty")
    client.create_bucket(Bucket="create-full")
    client.put_object(Bucket="create-full", Key="item", Body=b"x")
    target = tmp_path / "all.tar"

    result = run(
        executable, "-c", "-f", str(target), "s3://", env=s3_environment
    )

    assert result.returncode == 0, result.stderr
    members = members_by_name(target)
    assert members["create-empty"].isdir()
    assert "create-full/item" in members
    item = members["create-full/item"]
    assert "SCHILY.xattr.bucket" not in item.pax_headers


@pytest.mark.parametrize(
    ("process_umask", "expected_mode"),
    [(0o000, 0o666), (0o022, 0o644), (0o077, 0o600)],
    ids=("umask-000", "umask-022", "umask-077"),
)
def test_create_new_archive_respects_umask(
    executable,
    s3_server,
    s3_environment,
    tmp_path,
    process_umask,
    expected_mode,
):
    _endpoint, client = s3_server
    bucket = f"create-mode-{process_umask:03o}"
    client.create_bucket(Bucket=bucket)
    target = tmp_path / "mode.tar"

    result = run(
        executable,
        "-c",
        "-f",
        str(target),
        f"s3://{bucket}",
        env=s3_environment,
        umask=process_umask,
    )

    assert result.returncode == 0, result.stderr
    assert stat.S_IMODE(target.stat().st_mode) == expected_mode


def test_create_bucket_prefix_keeps_full_key(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="create-prefix")
    client.put_object(Bucket="create-prefix", Key="album/photos/a.jpg", Body=b"a")
    client.put_object(Bucket="create-prefix", Key="album/photos/b.jpg", Body=b"b")
    client.put_object(Bucket="create-prefix", Key="album/photos-old/c.jpg", Body=b"c")
    client.put_object(Bucket="create-prefix", Key="other/d.jpg", Body=b"d")
    target = tmp_path / "prefix.tar"

    result = run(
        executable,
        "-c",
        "-f",
        str(target),
        "s3://create-prefix/album/photos/",
        env=s3_environment,
    )

    assert result.returncode == 0, result.stderr
    members = members_by_name(target)
    assert set(members) == {
        "create-prefix",
        "create-prefix/album/photos/a.jpg",
        "create-prefix/album/photos/b.jpg",
    }


def test_create_multiple_sources_preserves_overlapping_prefixes(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="create-multiple-first")
    client.create_bucket(Bucket="create-multiple-second")
    client.put_object(
        Bucket="create-multiple-first", Key="shared/a", Body=b"a"
    )
    client.put_object(
        Bucket="create-multiple-first", Key="shared/deeper/b", Body=b"b"
    )
    client.put_object(
        Bucket="create-multiple-first", Key="outside", Body=b"outside"
    )
    client.put_object(Bucket="create-multiple-second", Key="c", Body=b"c")
    target = tmp_path / "multiple.tar"

    result = run(
        executable,
        "-cf",
        str(target),
        "s3://create-multiple-first/shared/",
        "s3://create-multiple-first/shared/deeper/",
        "s3://create-multiple-second/",
        env=s3_environment,
    )

    assert result.returncode == 0, result.stderr
    with tarfile.open(target, "r:") as archive:
        names = archive.getnames()
        assert names.count("create-multiple-first/shared/a") == 1
        # As with tar, an entry selected by multiple operands is archived
        # once for each matching operand.
        assert names.count("create-multiple-first/shared/deeper/b") == 2
        assert names.count("create-multiple-second/c") == 1
        assert "create-multiple-first/outside" not in names


def test_create_overwrites_existing_tarfile(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="create-overwrite")
    client.put_object(
        Bucket="create-overwrite", Key="new-object", Body=b"new archive data"
    )
    target = tmp_path / "existing.tar"
    target.write_bytes(b"old contents that must be truncated")
    target.chmod(0o640)

    result = run(
        executable,
        "-c",
        "-f",
        str(target),
        "s3://create-overwrite/",
        env=s3_environment,
    )

    assert result.returncode == 0, result.stderr
    assert stat.S_IMODE(target.stat().st_mode) == 0o640
    with tarfile.open(target, "r:") as archive:
        assert archive.getnames() == [
            "create-overwrite",
            "create-overwrite/new-object",
        ]
        assert (
            archive.extractfile("create-overwrite/new-object").read()
            == b"new archive data"
        )


def test_failed_create_leaves_directly_written_tarfile(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="create-partial")
    client.put_object(
        Bucket="create-partial", Key="folder/../unsafe", Body=b"unsafe"
    )
    target = tmp_path / "partial.tar"

    result = run(
        executable,
        "-c",
        "-f",
        str(target),
        "s3://create-partial/",
        env=s3_environment,
    )

    assert result.returncode != 0
    assert "unsafe S3 key" in result.stderr
    assert target.exists()
    assert list(tmp_path.glob("partial.tar.tmp.*")) == []


def test_create_rejects_explicit_unsafe_key(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="create-unsafe-explicit")
    client.put_object(
        Bucket="create-unsafe-explicit", Key="folder/../unsafe", Body=b"unsafe"
    )

    result = run(
        executable,
        "-c",
        "-f",
        str(tmp_path / "unsafe.tar"),
        "s3://create-unsafe-explicit/folder/../unsafe",
        env=s3_environment,
    )

    assert result.returncode == 2
    assert "unsafe S3 key" in result.stderr


def test_create_without_f_writes_tar_to_stdout(
    executable, s3_server, s3_environment
):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="create-stdout")
    client.put_object(Bucket="create-stdout", Key="item", Body=b"stdout data")

    result = subprocess.run(
        [str(executable), "-c", "s3://create-stdout/"],
        env=s3_environment,
        capture_output=True,
        check=False,
    )

    assert result.returncode == 0, result.stderr.decode()
    assert result.stderr == b""
    with tarfile.open(fileobj=io.BytesIO(result.stdout), mode="r:") as archive:
        members = {member.name: member for member in archive.getmembers()}
        assert set(members) == {"create-stdout", "create-stdout/item"}
        assert "SCHILY.xattr.bucket" not in members["create-stdout/item"].pax_headers


def test_create_with_f_dash_writes_tar_to_stdout(
    executable, s3_server, s3_environment
):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="create-f-dash")
    client.put_object(Bucket="create-f-dash", Key="item", Body=b"f dash data")

    result = subprocess.run(
        [str(executable), "-cf", "-", "s3://create-f-dash/"],
        env=s3_environment,
        capture_output=True,
        check=False,
    )

    assert result.returncode == 0, result.stderr.decode()
    assert result.stderr == b""
    with tarfile.open(fileobj=io.BytesIO(result.stdout), mode="r:") as archive:
        assert archive.extractfile("create-f-dash/item").read() == b"f dash data"


def test_verbose_create_lists_archived_objects(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="verbose-create")
    client.put_object(Bucket="verbose-create", Key="first", Body=b"1")
    client.put_object(Bucket="verbose-create", Key="folder/second", Body=b"2")
    target = tmp_path / "verbose.tar"

    result = run(
        executable,
        "-c",
        "-v",
        "-f",
        str(target),
        "s3://verbose-create/",
        env=s3_environment,
    )

    assert result.returncode == 0, result.stderr
    assert result.stdout == ""
    assert result.stderr == (
        "verbose-create\n"
        "verbose-create/first\n"
        "verbose-create/folder/second\n"
    )


def test_verbose_create_to_stdout_lists_objects_on_stderr(
    executable, s3_server, s3_environment
):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="verbose-create-stdout")
    client.put_object(Bucket="verbose-create-stdout", Key="item", Body=b"data")

    result = subprocess.run(
        [str(executable), "-cvf", "-", "s3://verbose-create-stdout/"],
        env=s3_environment,
        capture_output=True,
        check=False,
    )

    assert result.returncode == 0, result.stderr.decode()
    assert result.stderr == (
        b"verbose-create-stdout\nverbose-create-stdout/item\n"
    )
    with tarfile.open(fileobj=io.BytesIO(result.stdout), mode="r:") as archive:
        assert archive.getnames() == [
            "verbose-create-stdout",
            "verbose-create-stdout/item",
        ]


def test_create_rejects_s3_tarfile(executable):
    result = run(
        executable,
        "-cf",
        "s3://archives/backup.tar",
        "s3://source/",
    )

    assert result.returncode == 2
    assert "TARFILE must be a local filesystem path or '-'" in result.stderr


def test_unicode_keys_round_trip(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    source_bucket = "unicode-source"
    objects = {
        "čeština/žluťoučký-kůň.txt": "český obsah".encode(),
        "日本語/写真.jpg": "日本語の内容".encode(),
        "العربية/ملف.txt": "محتوى عربي".encode(),
        "emoji/🌍-🚀.bin": b"emoji",
    }
    client.create_bucket(Bucket=source_bucket)
    for key, body in objects.items():
        client.put_object(Bucket=source_bucket, Key=key, Body=body)
    archive_path = tmp_path / "unicode.tar"

    created = run(
        executable,
        "-c",
        "-f",
        str(archive_path),
        f"s3://{source_bucket}/",
        env=s3_environment,
    )
    assert created.returncode == 0, created.stderr

    for key in objects:
        client.delete_object(Bucket=source_bucket, Key=key)

    extracted = run(
        executable,
        "-x",
        "-f",
        str(archive_path),
        f"s3://{source_bucket}/",
        env=s3_environment,
    )
    assert extracted.returncode == 0, extracted.stderr
    restored = client.list_objects_v2(Bucket=source_bucket).get("Contents", [])
    assert {item["Key"] for item in restored} == set(objects)
    for key, body in objects.items():
        response = client.get_object(Bucket=source_bucket, Key=key)
        assert response["Body"].read() == body


def test_create_continues_after_first_list_page(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    bucket = "create-pagination"
    keys = [f"item-{index:04d}" for index in range(513)]
    client.create_bucket(Bucket=bucket)
    for key in keys:
        client.put_object(Bucket=bucket, Key=key, Body=b"")
    archive_path = tmp_path / "pagination.tar"

    result = run(
        executable,
        "-c",
        "-f",
        str(archive_path),
        f"s3://{bucket}/",
        env=s3_environment,
    )

    assert result.returncode == 0, result.stderr
    with tarfile.open(archive_path, "r:") as archive:
        assert archive.getnames() == [bucket, *(f"{bucket}/{key}" for key in keys)]


def test_long_key_round_trip(
    executable, s3_server, s3_environment, tmp_path
):
    _endpoint, client = s3_server
    source_bucket = "long-key-source"
    key = "segment/" + "a" * 992
    assert len(key.encode()) == 1000
    body = b"long key data"
    client.create_bucket(Bucket=source_bucket)
    client.put_object(Bucket=source_bucket, Key=key, Body=body)
    archive_path = tmp_path / "long-key.tar"

    created = run(
        executable,
        "-c",
        "-f",
        str(archive_path),
        f"s3://{source_bucket}/",
        env=s3_environment,
    )
    assert created.returncode == 0, created.stderr

    with tarfile.open(archive_path, "r:") as archive:
        assert f"{source_bucket}/{key}" in archive.getnames()

    raw = raw_tar_entries(archive_path)
    object_pax = next(
        payload
        for _name, kind, payload in raw
        if kind == b"x" and f"path={source_bucket}/{key}\n".encode() in payload
    )
    assert b"SCHILY.xattr.bucket=" not in object_pax
    assert b"LIBARCHIVE.xattr." not in object_pax

    extracted = run(
        executable,
        "-x",
        "-f",
        str(archive_path),
        f"s3://{source_bucket}/",
        env=s3_environment,
    )
    assert extracted.returncode == 0, extracted.stderr
    response = client.get_object(Bucket=source_bucket, Key=key)
    assert response["Body"].read() == body
