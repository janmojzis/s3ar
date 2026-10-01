import subprocess

import pytest


def run(program, *arguments, env):
    result = subprocess.run(
        [str(program), *map(str, arguments)],
        env=env, text=True, capture_output=True, timeout=30,
    )
    assert result.returncode == 0, result.stderr
    return result


@pytest.mark.parametrize("slash", ["", "/", "///"])
@pytest.mark.parametrize("exact", [False, True])
def test_selection_agrees_across_commands(
    executable, s3_server, s3_environment, tmp_path, slash, exact
):
    _, client = s3_server
    suffix = f"{len(slash)}-{int(exact)}"
    bucket = f"selection-source-{suffix}"
    destination = f"selection-target-{suffix}"
    selected = ["photo/", "photo/a", "photo/deep/b"]
    if exact:
        selected.insert(0, "photo")
    neighbors = ["phot", "photo-old", "photo1", "elsewhere/photo"]
    client.create_bucket(Bucket=bucket)
    for key in selected + neighbors:
        client.put_object(Bucket=bucket, Key=key, Body=key.encode())
    uri = f"s3://{bucket}/photo{slash}"
    expected = [bucket, *[f"{bucket}/{key}" for key in selected]]

    live = run(executable.with_name("s3ar-list"), uri, env=s3_environment)
    assert live.stdout.splitlines() == [f"s3://{name}" for name in expected]

    selected_archive = tmp_path / "selected.tar"
    run(executable, "-cf", selected_archive, uri, env=s3_environment)
    created = run(executable, "-tf", selected_archive, env=s3_environment)
    assert created.stdout.splitlines() == expected

    full_archive = tmp_path / "full.tar"
    run(executable, "-cf", full_archive, f"s3://{bucket}", env=s3_environment)
    listed = run(executable, "-tf", full_archive, uri, env=s3_environment)
    assert listed.stdout.splitlines() == expected

    run(
        executable, "-xf", full_archive,
        f"--transform=s|^{bucket}/|{destination}/|",
        f"s3://{destination}/photo{slash}", env=s3_environment,
    )
    restored = client.list_objects_v2(Bucket=destination)["Contents"]
    assert {obj["Key"] for obj in restored} == set(selected)
    for key in selected:
        body = client.get_object(Bucket=destination, Key=key)["Body"].read()
        assert body == key.encode()

    delete = executable.with_name("s3ar-delete")
    preview = run(delete, "-v", "--dry-run", uri, env=s3_environment)
    preview_keys = {
        line.split("info: ", 1)[1].split(" would delete version=", 1)[0]
        for line in preview.stderr.splitlines() if " would delete version=" in line
    }
    assert preview_keys == {f"s3://{bucket}/{key}" for key in selected}
    run(delete, uri, env=s3_environment)
    remaining = client.list_objects_v2(Bucket=bucket)["Contents"]
    assert {obj["Key"] for obj in remaining} == set(neighbors)


@pytest.mark.parametrize("operation", ["-c", "-x", "-t", "list"])
def test_invalid_later_operand_is_rejected_before_processing(
    executable, s3_environment, tmp_path, operation
):
    archive = tmp_path / "archive.tar"
    if operation == "list":
        arguments = [executable.with_name("s3ar-list")]
    else:
        arguments = [executable, operation, "-f", archive]
    result = subprocess.run(
        [*map(str, arguments), "s3://selection-never-read", "s3:///invalid"],
        env=s3_environment, text=True, capture_output=True, timeout=30,
    )
    assert result.returncode == 2
    assert "invalid S3 operand" in result.stderr
    assert result.stdout == ""
    assert "selection-never-read" not in result.stderr
    assert not archive.exists()


@pytest.mark.parametrize("tool", ["list", "create"])
@pytest.mark.parametrize("found", [False, True])
def test_selection_filters_neighbors_across_listing_pages(
    executable, s3_environment, tmp_path, tool, found
):
    from fault_server import FaultServer, ResponseStep

    def page(key, more):
        token = "<NextContinuationToken>next</NextContinuationToken>" if more else ""
        return (
            "<ListBucketResult><EncodingType>url</EncodingType>"
            f"<IsTruncated>{str(more).lower()}</IsTruncated>{token}"
            f"<Contents><Key>{key}</Key><Size>4</Size>"
            "<LastModified>2026-09-03T12:00:00Z</LastModified>"
            "<ETag>\"object\"</ETag></Contents></ListBucketResult>"
        ).encode()

    listing_path = "/bucket?list-type=2&max-keys=1000&encoding-type=url&prefix=photo"
    steps = []
    if tool == "create":
        steps.append(ResponseStep(
            "GET", "/bucket?acl", 200,
            b"<AccessControlPolicy><Owner><ID>owner</ID></Owner>"
            b"<AccessControlList/></AccessControlPolicy>",
        ))
    steps += [
        ResponseStep("GET", listing_path, 200, page("photo-old", True)),
        ResponseStep("GET", listing_path + "&continuation-token=next", 200,
                     page("photo/a" if found else "photo1", False)),
    ]
    if tool == "create" and found:
        steps.append(ResponseStep("GET", "/bucket/photo/a", 200, b"data",
                                  headers=(("ETag", '"object"'),)))
    archive = tmp_path / "pages.tar"
    arguments = (
        [executable.with_name("s3ar-list")]
        if tool == "list" else [executable, "-cf", archive]
    )
    with FaultServer(steps) as server:
        environment = {**s3_environment, "S3AR_ENDPOINT": server.endpoint}
        result = subprocess.run(
            [*map(str, arguments), "s3://bucket/photo"],
            env=environment, text=True, capture_output=True, timeout=30,
        )
    if found:
        assert result.returncode == 0, result.stderr
        if tool == "create":
            result = run(executable, "-tf", archive, env=s3_environment)
            assert result.stdout.splitlines() == ["bucket", "bucket/photo/a"]
        else:
            assert result.stdout.splitlines() == ["s3://bucket", "s3://bucket/photo/a"]
    else:
        assert result.returncode == 2
        assert "not found s3://bucket/photo" in result.stderr
        assert not archive.exists()
