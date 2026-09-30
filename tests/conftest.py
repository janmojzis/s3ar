import os
from pathlib import Path
import threading

import boto3
import pytest
from moto.server import create_backend_app
from werkzeug.serving import make_server


class S3MotoServer:
    def __init__(self):
        self.server = make_server("127.0.0.1", 0, create_backend_app("s3"), True)
        self.thread = None

    def start(self):
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def get_host_and_port(self):
        return self.server.server_address[:2]

    def stop(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()


@pytest.fixture(scope="session")
def s3_server():
    server = S3MotoServer()
    server.start()
    host, port = server.get_host_and_port()
    endpoint = f"http://{host}:{port}"
    client = boto3.client(
        "s3",
        endpoint_url=endpoint,
        region_name="us-east-1",
        aws_access_key_id="test-access",
        aws_secret_access_key="test-secret",
    )
    try:
        yield endpoint, client
    finally:
        server.stop()


@pytest.fixture
def s3_environment(s3_server):
    endpoint, _client = s3_server
    environment = os.environ.copy()
    environment.update(
        {
            "S3AR_ENDPOINT": endpoint,
            "S3AR_URI_STYLE": "path",
            "S3AR_REGION": "us-east-1",
            "S3AR_ACCESS_KEY": "test-access",
            "S3AR_SECRET_KEY": "test-secret",
        }
    )
    return environment


@pytest.fixture(scope="session")
def executable():
    return Path(__file__).resolve().parents[1] / "s3ar"
