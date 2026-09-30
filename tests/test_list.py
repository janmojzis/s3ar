import io
import subprocess
import tarfile

import pytest


def run(executable, *arguments, cwd=None, env=None):
    return subprocess.run(
        [str(executable), *arguments],
        cwd=cwd,
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )


def test_gnu_tar_operation_aliases_are_recognized(executable):
    for option in ("--create",):
        result = run(executable, option)
        assert result.returncode == 2
        assert "requires at least one S3 operand" in result.stderr
        assert "unknown option" not in result.stderr

    for option in ("--extract", "--get"):
        result = run(executable, option)
        assert result.returncode == 2
        assert "unknown option" not in result.stderr


def test_gnu_tar_help_alias(executable):
    short = run(executable, "-h")
    long = run(executable, "--help")

    assert short.returncode == 0, short.stderr
    assert long.returncode == 0, long.stderr
    assert long.stdout == short.stdout == ""
    assert long.stderr == short.stderr
    assert long.stderr.startswith("Usage: s3ar ")


def test_invalid_locale_does_not_affect_help(executable):
    result = run(
        executable,
        "--help",
        env={"LC_ALL": "s3ar-test-invalid-locale"},
    )

    assert result.returncode == 0
    assert result.stdout == ""
    assert result.stderr.startswith("Usage: s3ar ")


def test_short_list_options_are_not_supported(executable):
    for option in ("-l",):
        result = run(executable, option, "s3://bucket")

        assert result.returncode == 2
        assert f"unknown option {option}" in result.stderr


@pytest.mark.parametrize("option", ["--list-buckets", "--list-objects"])
def test_live_list_options_are_not_supported(executable, option):
    result = run(executable, option)

    assert result.returncode == 2
    assert f"unknown option {option}" in result.stderr


def test_unknown_option_names_the_option_not_the_program(executable):
    result = run(executable, "-qc", "s3://")

    assert result.returncode == 2
    assert "unknown option -q" in result.stderr


def test_missing_option_argument_is_diagnosed(executable):
    result = run(executable, "-c", "-f")

    assert result.returncode == 2
    assert "option requires an argument -f" in result.stderr


def test_archive_open_error_preserves_spaces(executable, tmp_path):
    missing = tmp_path / "missing archive.tar"
    result = run(executable, "-v", "-t", "-f", str(missing), env={})

    assert result.returncode == 2
    assert result.stdout == ""
    assert result.stderr.startswith("s3ar: fatal: cannot open archive: ")
    assert str(missing) in result.stderr
    assert "%20" not in result.stderr


def test_list_archive_uses_pax_identity_without_s3_configuration(
    executable, tmp_path
):
    archive_path = tmp_path / "list.tar"
    data = io.BytesIO()
    with tarfile.open(fileobj=data, mode="w", format=tarfile.PAX_FORMAT) as archive:
        bucket = tarfile.TarInfo("informational-name")
        bucket.type = tarfile.DIRTYPE
        bucket.pax_headers = {
            "SCHILY.xattr.user.s3ar.format": "1",
            "SCHILY.xattr.user.s3ar.bucket": "archive-bucket",
        }
        archive.addfile(bucket)
        member = tarfile.TarInfo("informational-name/wrong-key")
        member.size = 4
        member.mtime = 123
        member.pax_headers = {
            "SCHILY.xattr.user.s3ar.format": "1",
            "SCHILY.xattr.user.s3ar.bucket": "archive-bucket",
            "SCHILY.xattr.user.s3ar.key": "folder/key%20name%2B%25%C5%BE",
        }
        archive.addfile(member, io.BytesIO(b"data"))
        current = tarfile.TarInfo("another-informational-name")
        current.mtime = 456
        current.pax_headers = {
            "SCHILY.xattr.user.s3ar.format": "1",
            "SCHILY.xattr.user.s3ar.bucket": "archive-bucket",
            "SCHILY.xattr.user.s3ar.key": "current",
            "SCHILY.xattr.user.s3ar.etag": '"3472a7"',
        }
        archive.addfile(current, io.BytesIO())
    archive_path.write_bytes(data.getvalue())

    short = run(executable, "-tf", str(archive_path), env={})
    long = run(executable, "--list", "-f", str(archive_path), env={})
    verbose = run(executable, "-tvf", str(archive_path), env={})

    assert short.returncode == 0, short.stderr
    assert short.stdout == (
        "archive-bucket\n"
        "archive-bucket/folder/key%20name%2B%25%C5%BE\n"
        "archive-bucket/current\n"
    )
    assert long.returncode == 0, long.stderr
    assert long.stdout == short.stdout
    assert verbose.returncode == 0, verbose.stderr
    assert verbose.stdout == (
        "archive-bucket\n"
        "archive-bucket/folder/key%20name%2B%25%C5%BE 4 123 - -\n"
        'archive-bucket/current 0 456 "3472a7" -\n'
    )
