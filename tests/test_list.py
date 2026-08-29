import subprocess


def run(executable, *arguments, cwd=None, env=None):
    return subprocess.run(
        [str(executable), *arguments],
        cwd=cwd,
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )


def test_list_rejects_local_archive_operand(executable):
    result = run(executable, "--list-objects", "archive.tar")

    assert result.returncode == 2
    assert "--list-objects operand must be s3://" in result.stderr


def test_list_buckets_writes_bare_names(
    executable, s3_server, s3_environment
):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="list-buckets-first")
    client.create_bucket(Bucket="list-buckets-second")

    result = run(executable, "--list-buckets", env=s3_environment)

    assert result.returncode == 0, result.stderr
    assert result.stderr == ""
    names = result.stdout.splitlines()
    assert names == sorted(names)
    assert "list-buckets-first" in names
    assert "list-buckets-second" in names
    assert all("/" not in name for name in names)


def test_list_buckets_rejects_operands(executable):
    result = run(executable, "--list-buckets", "s3://bucket")

    assert result.returncode == 2
    assert "--list-buckets does not accept operands" in result.stderr


def test_list_multiple_live_s3_operands(
    executable, s3_server, s3_environment
):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="list-multiple")
    client.put_object(Bucket="list-multiple", Key="first/a", Body=b"a")
    client.put_object(Bucket="list-multiple", Key="second/b", Body=b"b")
    client.put_object(Bucket="list-multiple", Key="outside", Body=b"x")

    result = run(
        executable,
        "--list-objects",
        "s3://list-multiple/first/",
        "s3://list-multiple/second/",
        env=s3_environment,
    )

    assert result.returncode == 0, result.stderr
    assert result.stdout == (
        "s3://list-multiple/first/a\n"
        "s3://list-multiple/second/b\n"
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
    assert long.stdout == short.stdout
    assert long.stderr == ""


def test_short_list_options_are_not_supported(executable):
    for option in ("-l", "-t"):
        result = run(executable, option, "s3://bucket")

        assert result.returncode == 2
        assert f"unknown option {option}" in result.stderr


def test_old_long_list_option_is_not_supported(executable):
    result = run(executable, "--list", "s3://bucket")

    assert result.returncode == 2
    assert "unknown option --list" in result.stderr


def test_list_objects_in_one_bucket_including_empty_bucket(
    executable, s3_server, s3_environment
):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="list-one")
    client.put_object(Bucket="list-one", Key="z.txt", Body=b"z")
    client.put_object(Bucket="list-one", Key="folder/a.txt", Body=b"a")

    result = run(
        executable, "--list-objects", "s3://list-one", env=s3_environment
    )

    assert result.returncode == 0, result.stderr
    assert result.stdout == (
        "s3://list-one/folder/a.txt\n"
        "s3://list-one/z.txt\n"
    )

    client.create_bucket(Bucket="list-empty")
    empty = run(
        executable,
        "--list-objects",
        "s3://list-empty/",
        env=s3_environment,
    )
    assert empty.returncode == 0, empty.stderr
    assert empty.stdout == ""


def test_verbose_list_s3(executable, s3_server, s3_environment):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="list-verbose")
    client.put_object(
        Bucket="list-verbose",
        Key="item",
        Body=b"1234",
        Metadata={"source": "pytest", "sha256": "3472a7"},
    )

    result = run(
        executable,
        "--list-objects",
        "-v",
        "s3://list-verbose/",
        env=s3_environment,
    )

    assert result.returncode == 0, result.stderr
    assert result.stdout == (
        "s3://list-verbose/item\t4\tsha256=3472a7,source=pytest\n"
    )


def test_list_objects_in_all_s3_buckets(
    executable, s3_server, s3_environment
):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="aaa-list-all")
    client.create_bucket(Bucket="zzz-list-all")
    client.put_object(Bucket="aaa-list-all", Key="object", Body=b"x")

    result = run(
        executable, "--list-objects", "s3://", env=s3_environment
    )

    assert result.returncode == 0, result.stderr
    lines = result.stdout.splitlines()
    assert "s3://aaa-list-all/object" in lines
    assert "s3://aaa-list-all" not in lines
    assert "s3://zzz-list-all" not in lines


def test_list_live_s3_prefix(executable, s3_server, s3_environment):
    _endpoint, client = s3_server
    client.create_bucket(Bucket="list-prefix")
    client.put_object(Bucket="list-prefix", Key="folder/first", Body=b"1")
    client.put_object(Bucket="list-prefix", Key="folder/second", Body=b"22")
    client.put_object(Bucket="list-prefix", Key="outside", Body=b"outside")

    result = run(
        executable,
        "--list-objects",
        "s3://list-prefix/folder/",
        env=s3_environment,
    )

    assert result.returncode == 0, result.stderr
    assert result.stdout == (
        "s3://list-prefix/folder/first\n"
        "s3://list-prefix/folder/second\n"
    )


def test_list_live_s3_continues_after_first_page(
    executable, s3_server, s3_environment
):
    _endpoint, client = s3_server
    bucket = "list-pagination"
    keys = [f"item-{index:04d}" for index in range(1001)]
    client.create_bucket(Bucket=bucket)
    for key in keys:
        client.put_object(Bucket=bucket, Key=key, Body=b"")

    result = run(
        executable,
        "--list-objects",
        f"s3://{bucket}",
        env=s3_environment,
    )

    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == [
        *(f"s3://{bucket}/{key}" for key in keys),
    ]


def test_file_option_is_invalid_for_list(executable):
    result = run(executable, "--list-objects", "-f", "archive.tar")

    assert result.returncode == 2
    assert "-f is valid only with -c or -x" in result.stderr


def test_zstd_rejects_live_s3_list(executable):
    result = run(
        executable, "--list-objects", "--zstd", "s3://bucket/"
    )

    assert result.returncode == 2
    assert "option --zstd is not valid with --list-objects" in result.stderr
