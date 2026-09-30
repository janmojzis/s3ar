import json

import boto3
import pytest
from botocore.exceptions import ClientError
from moto.s3.models import s3_backends

from s3testserver import (
    ACCESS_KEY,
    SECRET_KEY,
    FilesystemMotoServer,
    FilesystemStore,
    load_filesystem_into_moto,
)


def server_client(server):
    host, port = server.get_host_and_port()
    endpoint = f"http://{host}:{port}"
    return endpoint, boto3.client(
        "s3",
        endpoint_url=endpoint,
        region_name="us-east-1",
        aws_access_key_id=ACCESS_KEY,
        aws_secret_access_key=SECRET_KEY,
    )


def test_server_persists_objects_as_bucket_key_tree(tmp_path):
    store = FilesystemStore(tmp_path)
    server = FilesystemMotoServer(store, "127.0.0.1", 0)
    server.start()
    _endpoint, client = server_client(server)
    try:
        client.create_bucket(Bucket="manual-test")
        client.put_object(
            Bucket="manual-test",
            Key="path/object.txt",
            Body=b"stored data",
            Metadata={"source": "pytest"},
        )
    finally:
        server.stop()

    assert (tmp_path / "manual-test" / "path" / "object.txt").read_bytes() == b"stored data"
    assert store.metadata_for("manual-test", "path/object.txt") == {"source": "pytest"}


def test_server_persists_completed_multipart_upload_across_restart(tmp_path):
    store = FilesystemStore(tmp_path)
    server = FilesystemMotoServer(store, "127.0.0.1", 0)
    server.start()
    _endpoint, client = server_client(server)
    first = b"a" * (5 * 1024 * 1024)
    second = b"last part"
    try:
        client.create_bucket(Bucket="multipart-test")
        upload = client.create_multipart_upload(
            Bucket="multipart-test",
            Key="large.bin",
            Metadata={"source": "multipart"},
        )
        upload_id = upload["UploadId"]
        parts = []
        for number, data in enumerate((first, second), 1):
            response = client.upload_part(
                Bucket="multipart-test",
                Key="large.bin",
                UploadId=upload_id,
                PartNumber=number,
                Body=data,
            )
            parts.append({"PartNumber": number, "ETag": response["ETag"]})
        client.complete_multipart_upload(
            Bucket="multipart-test",
            Key="large.bin",
            UploadId=upload_id,
            MultipartUpload={"Parts": parts},
        )
    finally:
        server.stop()

    assert (tmp_path / "multipart-test" / "large.bin").read_bytes() == first + second
    restarted_store = FilesystemStore(tmp_path)
    restarted = FilesystemMotoServer(restarted_store, "127.0.0.1", 0)
    restarted.start()
    endpoint, restarted_client = server_client(restarted)
    try:
        load_filesystem_into_moto(restarted_store, endpoint)
        response = restarted_client.get_object(
            Bucket="multipart-test", Key="large.bin"
        )
        assert response["Body"].read() == first + second
        assert response["Metadata"] == {"source": "multipart"}
    finally:
        restarted.stop()


def test_server_preserves_metadata_header_spelling(tmp_path):
    store = FilesystemStore(tmp_path)
    server = FilesystemMotoServer(store, "127.0.0.1", 0)
    server.start()
    _endpoint, client = server_client(server)
    try:
        client.create_bucket(Bucket="metadata-test")
        client.put_object(
            Bucket="metadata-test",
            Key="object",
            Body=b"data",
            Metadata={"with_underscore": "one", "with-hyphen": "two"},
        )
    finally:
        server.stop()

    assert store.metadata_for("metadata-test", "object") == {
        "with_underscore": "one",
        "with-hyphen": "two",
    }


def test_server_returns_clean_error_for_filesystem_collision(tmp_path):
    store = FilesystemStore(tmp_path)
    server = FilesystemMotoServer(store, "127.0.0.1", 0)
    server.start()
    _endpoint, client = server_client(server)
    try:
        client.create_bucket(Bucket="collision-test")
        client.put_object(Bucket="collision-test", Key="a", Body=b"file")
        with pytest.raises(ClientError) as failure:
            client.put_object(Bucket="collision-test", Key="a/b", Body=b"child")
        assert failure.value.response["ResponseMetadata"]["HTTPStatusCode"] == 409
    finally:
        server.stop()


def test_server_removes_staged_parts_when_upload_is_aborted(tmp_path):
    store = FilesystemStore(tmp_path)
    server = FilesystemMotoServer(store, "127.0.0.1", 0)
    server.start()
    _endpoint, client = server_client(server)
    try:
        client.create_bucket(Bucket="abort-test")
        upload = client.create_multipart_upload(Bucket="abort-test", Key="object")
        client.upload_part(
            Bucket="abort-test",
            Key="object",
            UploadId=upload["UploadId"],
            PartNumber=1,
            Body=b"temporary",
        )
        client.abort_multipart_upload(
            Bucket="abort-test", Key="object", UploadId=upload["UploadId"]
        )
    finally:
        server.stop()

    assert not list((tmp_path / ".s3testserver").glob("upload-part.*"))


@pytest.mark.parametrize(
    "key", ["path//object", "path/./object", "./path/object", "path/object/"]
)
def test_server_rejects_normalized_aliases_without_overwriting_files(tmp_path, key):
    bucket = "alias-key-test"
    store = FilesystemStore(tmp_path)
    server = FilesystemMotoServer(store, "127.0.0.1", 0)
    server.start()
    _endpoint, client = server_client(server)
    try:
        client.create_bucket(Bucket=bucket)
        client.put_object(Bucket=bucket, Key="path/object", Body=b"original")
        with pytest.raises(ClientError) as failure:
            client.put_object(Bucket=bucket, Key=key, Body=b"replacement")
        assert failure.value.response["ResponseMetadata"]["HTTPStatusCode"] == 409
        assert (tmp_path / bucket / "path/object").read_bytes() == b"original"
        with pytest.raises(ClientError) as failure:
            client.delete_object(Bucket=bucket, Key=key)
        assert failure.value.response["ResponseMetadata"]["HTTPStatusCode"] == 409
        assert (tmp_path / bucket / "path/object").read_bytes() == b"original"
    finally:
        server.stop()


@pytest.mark.parametrize("key", [".s3-object-important", "path/.s3-object-important"])
def test_server_rejects_reserved_temporary_filenames(tmp_path, key):
    store = FilesystemStore(tmp_path)
    server = FilesystemMotoServer(store, "127.0.0.1", 0)
    server.start()
    _endpoint, client = server_client(server)
    try:
        client.create_bucket(Bucket="reserved-key-test")
        with pytest.raises(ClientError) as failure:
            client.put_object(Bucket="reserved-key-test", Key=key, Body=b"data")
        assert failure.value.response["ResponseMetadata"]["HTTPStatusCode"] == 409
        assert not (tmp_path / "reserved-key-test" / key).exists()
    finally:
        server.stop()


def test_server_imports_legacy_metadata_json(tmp_path):
    state = tmp_path / ".s3testserver"
    state.mkdir()
    (state / "metadata.json").write_text(
        json.dumps({"legacy-bucket/path/object": {"source": "legacy"}}),
        encoding="utf-8",
    )

    store = FilesystemStore(tmp_path)

    assert store.metadata_for("legacy-bucket", "path/object") == {
        "source": "legacy"
    }


@pytest.mark.parametrize("key", ["literal%2Fkey", "café", "prefix/%252F"])
def test_server_preserves_encoded_key_identity_across_restart(tmp_path, key):
    bucket = "encoded-key-test"
    store = FilesystemStore(tmp_path)
    server = FilesystemMotoServer(store, "127.0.0.1", 0)
    server.start()
    _endpoint, client = server_client(server)
    try:
        client.create_bucket(Bucket=bucket)
        client.put_object(
            Bucket=bucket, Key=key, Body=b"original", Metadata={"source": "test"}
        )
    finally:
        server.stop()

    assert (tmp_path / bucket / key).read_bytes() == b"original"
    assert store.metadata_for(bucket, key) == {"source": "test"}

    # Moto backends survive server.stop(); reset to model a fresh process.
    s3_backends.reset()
    restarted_store = FilesystemStore(tmp_path)
    restarted = FilesystemMotoServer(restarted_store, "127.0.0.1", 0)
    restarted.start()
    endpoint, restarted_client = server_client(restarted)
    try:
        load_filesystem_into_moto(restarted_store, endpoint)
        response = restarted_client.get_object(Bucket=bucket, Key=key)
        assert response["Body"].read() == b"original"
        assert response["Metadata"] == {"source": "test"}
        objects = restarted_client.list_objects_v2(Bucket=bucket)["Contents"]
        assert [item["Key"] for item in objects] == [key]
    finally:
        restarted.stop()
