#!/usr/bin/python3
"""Run a filesystem-backed S3-compatible Moto server for s3ar testing."""

import argparse
import json
import os
import signal
import shutil
import sqlite3
import tempfile
import threading
import warnings
import xml.etree.ElementTree as ET
from pathlib import Path
from urllib.parse import parse_qs

import boto3
from botocore.exceptions import ClientError
from moto.server import create_backend_app
from werkzeug.serving import WSGIRequestHandler, make_server


ACCESS_KEY = "test-access"
SECRET_KEY = "test-secret"
STATE_DIRECTORY = ".s3testserver"
METADATA_FILE = "metadata.json"
METADATA_DATABASE = "metadata.sqlite3"


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
        self.lock = threading.RLock()
        self.database = self._open_metadata_database()
        self.uploads = {}

    def _open_metadata_database(self):
        self.state_directory.mkdir(mode=0o700, exist_ok=True)
        database_path = self.state_directory / METADATA_DATABASE
        new_database = not database_path.exists()
        try:
            database = sqlite3.connect(database_path, check_same_thread=False)
            database.execute(
                "CREATE TABLE IF NOT EXISTS metadata ("
                "bucket TEXT NOT NULL, object_key TEXT NOT NULL, "
                "name TEXT NOT NULL, value TEXT NOT NULL, "
                "PRIMARY KEY (bucket, object_key, name))"
            )
            if new_database:
                self._import_legacy_metadata(database)
            database.commit()
            return database
        except (OSError, sqlite3.Error) as error:
            raise RuntimeError(f"cannot open {database_path}: {error}") from error

    def _import_legacy_metadata(self, database):
        try:
            legacy = json.loads(self.metadata_path.read_text(encoding="utf-8"))
        except FileNotFoundError:
            return
        except (OSError, json.JSONDecodeError) as error:
            raise RuntimeError(f"cannot read {self.metadata_path}: {error}") from error
        if not isinstance(legacy, dict):
            return
        for identity, metadata in legacy.items():
            if "/" not in identity or not isinstance(metadata, dict):
                continue
            bucket, key = identity.split("/", 1)
            database.executemany(
                "INSERT OR REPLACE INTO metadata VALUES (?, ?, ?, ?)",
                (
                    (bucket, key, name, value)
                    for name, value in metadata.items()
                    if isinstance(name, str) and isinstance(value, str)
                ),
            )

    def iter_buckets(self):
        for path in sorted(self.root.iterdir()):
            if path.is_dir() and not path.is_symlink() and path.name != STATE_DIRECTORY:
                yield path

    def iter_objects(self, bucket_path):
        for path in sorted(bucket_path.rglob("*")):
            if (
                path.is_file()
                and not path.is_symlink()
                and not path.name.startswith(".s3-object-")
            ):
                yield path, path.relative_to(bucket_path).as_posix()

    def metadata_for(self, bucket, key):
        with self.lock:
            return dict(
                self.database.execute(
                    "SELECT name, value FROM metadata "
                    "WHERE bucket = ? AND object_key = ?",
                    (bucket, key),
                )
            )

    def _replace_metadata(self, bucket, key, metadata):
        self.database.execute(
            "DELETE FROM metadata WHERE bucket = ? AND object_key = ?",
            (bucket, key),
        )
        self.database.executemany(
            "INSERT INTO metadata VALUES (?, ?, ?, ?)",
            ((bucket, key, name, value) for name, value in metadata.items()),
        )
        self.database.commit()

    def object_path(self, bucket, key):
        if not bucket or not key or bucket in {".", "..", STATE_DIRECTORY}:
            return None
        # Validate the original key before pathlib drops empty/dot components.
        components = key.split("/")
        if any(part in {"", ".", ".."} for part in components):
            return None
        # The startup loader deliberately ignores unfinished temporary files.
        if components[-1].startswith(".s3-object-"):
            return None
        relative = Path(key)
        if relative.is_absolute():
            return None
        candidate = (self.root / bucket / relative).resolve(strict=False)
        bucket_root = (self.root / bucket).resolve(strict=False)
        if candidate == bucket_root or bucket_root not in candidate.parents:
            return None
        return candidate

    @staticmethod
    def _metadata(headers):
        prefix = "x-amz-meta-"
        return {
            name[len(prefix) :].lower(): value
            for name, value in headers
            if name.lower().startswith(prefix)
        }

    @staticmethod
    def _copy_source(headers):
        return any(name.lower() == "x-amz-copy-source" for name, _value in headers)

    def _write_object(self, bucket, key, body, metadata):
        target = self.object_path(bucket, key)
        if target is None:
            raise OSError("object key cannot be represented on the filesystem")
        target.parent.mkdir(parents=True, exist_ok=True)
        fd, temporary_name = tempfile.mkstemp(prefix=".s3-object-", dir=target.parent)
        try:
            with os.fdopen(fd, "wb") as stream:
                body.seek(0)
                shutil.copyfileobj(body, stream)
            os.replace(temporary_name, target)
        except BaseException:
            try:
                os.unlink(temporary_name)
            except FileNotFoundError:
                pass
            raise
        self._replace_metadata(bucket, key, metadata)

    def _initiate_upload(self, bucket, key, headers, response_body):
        root = ET.fromstring(response_body)
        upload_id = next(
            (
                node.text
                for node in root.iter()
                if node.tag.rsplit("}", 1)[-1] == "UploadId"
            ),
            None,
        )
        if not upload_id:
            raise OSError("multipart initiation response has no upload ID")
        self.uploads[upload_id] = {
            "bucket": bucket,
            "key": key,
            "metadata": self._metadata(headers),
            "parts": {},
        }

    def _store_part(self, upload_id, part_number, body):
        upload = self.uploads.get(upload_id)
        if upload is None:
            raise OSError("unknown multipart upload")
        self.state_directory.mkdir(mode=0o700, exist_ok=True)
        fd, part_path = tempfile.mkstemp(
            prefix="upload-part.", dir=self.state_directory
        )
        try:
            with os.fdopen(fd, "wb") as stream:
                body.seek(0)
                shutil.copyfileobj(body, stream)
        except BaseException:
            try:
                os.unlink(part_path)
            except FileNotFoundError:
                pass
            raise
        previous = upload["parts"].get(part_number)
        upload["parts"][part_number] = Path(part_path)
        if previous is not None:
            previous.unlink(missing_ok=True)

    def _remove_upload(self, upload_id):
        upload = self.uploads.pop(upload_id, None)
        if upload is not None:
            for part_path in upload["parts"].values():
                part_path.unlink(missing_ok=True)

    def discard_uploads(self):
        with self.lock:
            for upload_id in list(self.uploads):
                self._remove_upload(upload_id)

    def _complete_upload(self, upload_id, body):
        upload = self.uploads.get(upload_id)
        if upload is None:
            raise OSError("unknown multipart upload")
        body.seek(0)
        root = ET.parse(body).getroot()
        part_numbers = [
            int(node.text)
            for node in root.iter()
            if node.tag.rsplit("}", 1)[-1] == "PartNumber" and node.text
        ]
        missing = [number for number in part_numbers if number not in upload["parts"]]
        if not part_numbers or missing:
            raise OSError("multipart completion references missing parts")
        with tempfile.TemporaryFile() as assembled:
            for number in part_numbers:
                with upload["parts"][number].open("rb") as part:
                    shutil.copyfileobj(part, assembled)
            self._write_object(
                upload["bucket"], upload["key"], assembled, upload["metadata"]
            )
        self._remove_upload(upload_id)

    def apply(self, method, path, query, headers, body, response_body):
        if not path.startswith("/"):
            return
        # WSGI PATH_INFO already has percent escapes decoded, but represents
        # the original UTF-8 bytes as Latin-1 code points.
        parts = path.encode("latin-1").decode("utf-8").lstrip("/").split("/", 1)
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

            parameters = parse_qs(query, keep_blank_values=True)
            upload_id = parameters.get("uploadId", [None])[0]
            if method == "POST" and query == "uploads":
                self._initiate_upload(bucket, key, headers, response_body)
            elif method == "PUT" and upload_id is not None:
                part = parameters.get("partNumber", [None])[0]
                if part is None:
                    raise OSError("multipart part has no part number")
                self._store_part(upload_id, int(part), body)
            elif method == "POST" and upload_id is not None:
                self._complete_upload(upload_id, body)
            elif method == "DELETE" and upload_id is not None:
                self._remove_upload(upload_id)
            elif method == "PUT" and not query and not self._copy_source(headers):
                self._write_object(bucket, key, body, self._metadata(headers))
            elif method == "DELETE" and not query:
                target = self.object_path(bucket, key)
                if target is None:
                    raise OSError("object key cannot be represented on the filesystem")
                try:
                    target.unlink()
                except FileNotFoundError:
                    pass
                self._replace_metadata(bucket, key, {})


class PersistenceMiddleware:
    def __init__(self, app, store):
        self.app = app
        self.store = store

    def __call__(self, environ, start_response):
        method = environ.get("REQUEST_METHOD", "")
        mutating = method in {"PUT", "POST", "DELETE"}
        body = None
        if method in {"PUT", "POST"}:
            body = tempfile.TemporaryFile()
            content_length = environ.get("CONTENT_LENGTH", "")
            if content_length:
                remaining = int(content_length)
                while remaining:
                    chunk = environ["wsgi.input"].read(min(remaining, 1024 * 1024))
                    if not chunk:
                        break
                    body.write(chunk)
                    remaining -= len(chunk)
            elif environ.get("HTTP_TRANSFER_ENCODING", "").lower() == "chunked":
                while chunk := environ["wsgi.input"].read(1024 * 1024):
                    body.write(chunk)
            body.seek(0)
            environ["wsgi.input"] = body
            environ["CONTENT_LENGTH"] = str(body.seek(0, os.SEEK_END))
            body.seek(0)

        captured = {}

        def remember_status(status, response_headers, exc_info=None):
            captured["status"] = status
            captured["headers"] = response_headers
            captured["exc_info"] = exc_info
            return lambda _data: None

        response = self.app(environ, remember_status if mutating else start_response)
        if not mutating:
            return response
        try:
            response_body = b"".join(response)
        finally:
            close = getattr(response, "close", None)
            if close is not None:
                close()
        status_code = int(captured["status"].split(" ", 1)[0])
        if 200 <= status_code < 300:
            try:
                if body is None:
                    body = tempfile.TemporaryFile()
                self.store.apply(
                    method,
                    environ.get("PATH_INFO", ""),
                    environ.get("QUERY_STRING", ""),
                    environ.get("s3test.raw_headers", []),
                    body,
                    response_body,
                )
            except (OSError, ValueError, ET.ParseError, sqlite3.Error) as error:
                response_body = f"filesystem persistence failed: {error}\n".encode()
                captured["status"] = "409 Conflict"
                captured["headers"] = [
                    ("Content-Type", "text/plain; charset=utf-8"),
                    ("Content-Length", str(len(response_body))),
                ]
        if body is not None:
            body.close()
        start_response(
            captured["status"], captured["headers"], captured.get("exc_info")
        )
        return [response_body]


class RawHeaderRequestHandler(WSGIRequestHandler):
    def make_environ(self):
        environ = super().make_environ()
        environ["s3test.raw_headers"] = list(self.headers.raw_items())
        return environ


class FilesystemMotoServer:
    def __init__(self, store, ip_address, port):
        self.store = store
        moto = create_backend_app("s3")
        app = PersistenceMiddleware(moto, self.store)
        self.server = make_server(
            ip_address,
            port,
            app,
            threaded=True,
            request_handler=RawHeaderRequestHandler,
        )
        self.thread = None

    def start(self):
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def get_host_and_port(self):
        return self.server.server_address[:2]

    def stop(self):
        self.server.shutdown()
        self.server.server_close()
        if self.thread is not None:
            self.thread.join()
            self.thread = None
        self.store.discard_uploads()


def load_filesystem_into_moto(store, endpoint):
    client = boto3.client(
        "s3",
        endpoint_url=endpoint,
        region_name="us-east-1",
        aws_access_key_id=ACCESS_KEY,
        aws_secret_access_key=SECRET_KEY,
    )
    for bucket_path in store.iter_buckets():
        try:
            client.create_bucket(Bucket=bucket_path.name)
        except ClientError as error:
            warnings.warn(
                f"skipping invalid bucket directory {bucket_path}: {error}",
                stacklevel=2,
            )
            continue
        for object_path, key in store.iter_objects(bucket_path):
            try:
                with object_path.open("rb") as stream:
                    client.put_object(
                        Bucket=bucket_path.name,
                        Key=key,
                        Body=stream,
                        Metadata=store.metadata_for(bucket_path.name, key),
                    )
            except (ClientError, OSError) as error:
                warnings.warn(
                    f"skipping object {object_path}: {error}", stacklevel=2
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
