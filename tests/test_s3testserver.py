import boto3

from s3testserver import (
    ACCESS_KEY,
    SECRET_KEY,
    FilesystemMotoServer,
    FilesystemStore,
)


def test_server_persists_objects_as_bucket_key_tree(tmp_path):
    store = FilesystemStore(tmp_path)
    server = FilesystemMotoServer(store, "127.0.0.1", 0)
    server.start()
    host, port = server.get_host_and_port()
    client = boto3.client(
        "s3",
        endpoint_url=f"http://{host}:{port}",
        region_name="us-east-1",
        aws_access_key_id=ACCESS_KEY,
        aws_secret_access_key=SECRET_KEY,
    )
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
