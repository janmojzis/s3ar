import os
from pathlib import Path

import boto3
import pytest
from moto.server import ThreadedMotoServer, create_backend_app
from werkzeug.serving import make_server


class S3MotoServer(ThreadedMotoServer):
    """Run only Moto's S3 backend, without signature-based dispatch."""

    def _server_entry(self):
        self._server = make_server(
            self._ip_address,
            self._port,
            create_backend_app("s3"),
            True,
        )
        self._server_ready_event.set()
        self._server.serve_forever()


@pytest.fixture(scope="session")
def s3_server():
    server = S3MotoServer(ip_address="127.0.0.1", port=0, verbose=False)
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
