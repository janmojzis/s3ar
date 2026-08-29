#!/usr/bin/python3
"""Run a filesystem-backed S3-compatible Moto server for s3ar testing."""

import argparse
import io
import json
import os
import signal
import tempfile
import threading
from pathlib import Path
from urllib.parse import unquote

import boto3
from moto.server import ThreadedMotoServer, create_backend_app
from werkzeug.serving import make_server


ACCESS_KEY = "test-access"
SECRET_KEY = "test-secret"
STATE_DIRECTORY = ".s3testserver"
METADATA_FILE = "metadata.json"


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="Run a filesystem-backed S3-compatible test server using Moto."
    )
    parser.add_argument(
        "directory",
        type=Path,
        help="data root; objects are stored as DIRECTORY/BUCKET/KEY",
    )
    parser.add_argument(
        "--host",
        default="127.0.0.1",
        help="address to listen on (default: %(default)s)",
    )
    parser.add_argument(
        "--port",
        default=9000,
        type=int,
        help="TCP port; use 0 to select a free port (default: %(default)s)",
    )
    arguments = parser.parse_args()
    if not 0 <= arguments.port <= 65535:
        parser.error("--port must be between 0 and 65535")
    try:
        arguments.directory.mkdir(parents=True, exist_ok=True)
        arguments.directory = arguments.directory.resolve(strict=True)
    except OSError as error:
        parser.error(f"cannot create data directory: {error}")
    if not arguments.directory.is_dir():
        parser.error("data path is not a directory")
    return arguments


class FilesystemStore:
    """Mirror successful Moto writes to a bucket/key directory tree."""

    def __init__(self, root):
        self.root = root
        self.state_directory = root / STATE_DIRECTORY
        self.metadata_path = self.state_directory / METADATA_FILE
        self.lock = threading.Lock()
        self.metadata = self._load_metadata()

    def _load_metadata(self):
        try:
            value = json.loads(self.metadata_path.read_text(encoding="utf-8"))
            return value if isinstance(value, dict) else {}
        except FileNotFoundError:
            return {}
        except (OSError, json.JSONDecodeError) as error:
            raise RuntimeError(f"cannot read {self.metadata_path}: {error}") from error

    def _save_metadata(self):
        self.state_directory.mkdir(mode=0o700, exist_ok=True)
        fd, temporary_name = tempfile.mkstemp(
            prefix="metadata.", dir=self.state_directory, text=True
        )
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as stream:
                json.dump(self.metadata, stream, sort_keys=True, indent=2)
                stream.write("\n")
            os.replace(temporary_name, self.metadata_path)
        except BaseException:
            try:
                os.unlink(temporary_name)
            except FileNotFoundError:
                pass
            raise

    def iter_buckets(self):
        for path in sorted(self.root.iterdir()):
            if path.is_dir() and not path.is_symlink() and path.name != STATE_DIRECTORY:
                yield path

    def iter_objects(self, bucket_path):
        for path in sorted(bucket_path.rglob("*")):
            if path.is_file() and not path.is_symlink():
                yield path, path.relative_to(bucket_path).as_posix()

    def metadata_for(self, bucket, key):
        value = self.metadata.get(f"{bucket}/{key}", {})
        return value if isinstance(value, dict) else {}

    def object_path(self, bucket, key):
        if not bucket or not key or bucket in {".", "..", STATE_DIRECTORY}:
            return None
        relative = Path(key)
        if relative.is_absolute() or any(part in {"", ".", ".."} for part in relative.parts):
            return None
        candidate = (self.root / bucket / relative).resolve(strict=False)
        bucket_root = (self.root / bucket).resolve(strict=False)
        if candidate == bucket_root or bucket_root not in candidate.parents:
            return None
        return candidate

    def apply(self, method, path, query, headers, body):
        if query or not path.startswith("/"):
            return
        parts = unquote(path).lstrip("/").split("/", 1)
        bucket = parts[0]
        key = parts[1] if len(parts) == 2 and parts[1] else None
        if not bucket or bucket == STATE_DIRECTORY:
            return

        with self.lock:
            if key is None:
                bucket_path = self.root / bucket
                if method == "PUT":
                    bucket_path.mkdir(parents=False, exist_ok=True)
                elif method == "DELETE":
                    try:
                        bucket_path.rmdir()
                    except FileNotFoundError:
                        pass
                return

            target = self.object_path(bucket, key)
            if target is None:
                return
            metadata_key = f"{bucket}/{key}"
            if method == "PUT" and "HTTP_X_AMZ_COPY_SOURCE" not in headers:
                target.parent.mkdir(parents=True, exist_ok=True)
                fd, temporary_name = tempfile.mkstemp(prefix=".s3-object-", dir=target.parent)
                try:
                    with os.fdopen(fd, "wb") as stream:
                        stream.write(body)
                    os.replace(temporary_name, target)
                except BaseException:
                    try:
                        os.unlink(temporary_name)
                    except FileNotFoundError:
                        pass
                    raise
                object_metadata = {
                    name[len("HTTP_X_AMZ_META_") :].lower().replace("_", "-"): value
                    for name, value in headers.items()
                    if name.startswith("HTTP_X_AMZ_META_")
                }
                if object_metadata:
                    self.metadata[metadata_key] = object_metadata
                else:
                    self.metadata.pop(metadata_key, None)
                self._save_metadata()
            elif method == "DELETE":
                try:
                    target.unlink()
                except FileNotFoundError:
                    pass
                self.metadata.pop(metadata_key, None)
                self._save_metadata()


class PersistenceMiddleware:
    def __init__(self, app, store):
        self.app = app
        self.store = store

    def __call__(self, environ, start_response):
        method = environ.get("REQUEST_METHOD", "")
        body = b""
        if method == "PUT":
            content_length = environ.get("CONTENT_LENGTH", "")
            if content_length:
                body = environ["wsgi.input"].read(int(content_length))
            elif environ.get("HTTP_TRANSFER_ENCODING", "").lower() == "chunked":
                body = environ["wsgi.input"].read()
            environ["wsgi.input"] = io.BytesIO(body)
            environ["CONTENT_LENGTH"] = str(len(body))

        response_status = []

        def remember_status(status, response_headers, exc_info=None):
            response_status.append(int(status.split(" ", 1)[0]))
            return start_response(status, response_headers, exc_info)

        response = self.app(environ, remember_status)

        def finish_response():
            try:
                yield from response
            finally:
                close = getattr(response, "close", None)
                if close is not None:
                    close()
                if response_status and 200 <= response_status[-1] < 300:
                    self.store.apply(
                        method,
                        environ.get("PATH_INFO", ""),
                        environ.get("QUERY_STRING", ""),
                        environ,
                        body,
                    )

        return finish_response()


class FilesystemMotoServer(ThreadedMotoServer):
    def __init__(self, store, ip_address, port):
        super().__init__(ip_address=ip_address, port=port, verbose=False)
        self.store = store

    def _server_entry(self):
        moto = create_backend_app("s3")
        app = PersistenceMiddleware(moto, self.store)
        self._server = make_server(self._ip_address, self._port, app, True)
        self._server_ready_event.set()
        self._server.serve_forever()


def load_filesystem_into_moto(store, endpoint):
    client = boto3.client(
        "s3",
        endpoint_url=endpoint,
        region_name="us-east-1",
        aws_access_key_id=ACCESS_KEY,
        aws_secret_access_key=SECRET_KEY,
    )
    for bucket_path in store.iter_buckets():
        client.create_bucket(Bucket=bucket_path.name)
        for object_path, key in store.iter_objects(bucket_path):
            with object_path.open("rb") as stream:
                client.put_object(
                    Bucket=bucket_path.name,
                    Key=key,
                    Body=stream,
                    Metadata=store.metadata_for(bucket_path.name, key),
                )


def main():
    arguments = parse_arguments()
    try:
        store = FilesystemStore(arguments.directory)
    except RuntimeError as error:
        raise SystemExit(f"s3testserver: {error}") from error
    stopped = threading.Event()

    def request_stop(_signum, _frame):
        stopped.set()

    signal.signal(signal.SIGINT, request_stop)
    signal.signal(signal.SIGTERM, request_stop)

    server = FilesystemMotoServer(
        store=store,
        ip_address=arguments.host,
        port=arguments.port,
    )
    server.start()
    host, port = server.get_host_and_port()
    client_host = "127.0.0.1" if host in {"0.0.0.0", "::"} else host
    endpoint = f"http://{client_host}:{port}"

    try:
        load_filesystem_into_moto(store, endpoint)
    except BaseException:
        server.stop()
        raise

    print("S3 test server for s3ar is running with filesystem persistence.", flush=True)
    print(f"Data:       {arguments.directory}/<bucket>/<key>", flush=True)
    print(f"Endpoint:   {endpoint}", flush=True)
    print(f"Access key: {ACCESS_KEY}", flush=True)
    print(f"Secret key: {SECRET_KEY}", flush=True)
    print(flush=True)
    print("Configure s3ar in another shell:", flush=True)
    print(f"export S3AR_ENDPOINT={endpoint}", flush=True)
    print("export S3AR_URI_STYLE=path", flush=True)
    print("export S3AR_REGION=us-east-1", flush=True)
    print(f"export S3AR_ACCESS_KEY={ACCESS_KEY}", flush=True)
    print(f"export S3AR_SECRET_KEY={SECRET_KEY}", flush=True)
    print(flush=True)
    print("Press Ctrl+C to stop the server.", flush=True)

    try:
        stopped.wait()
    finally:
        server.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
