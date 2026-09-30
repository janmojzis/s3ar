from collections import deque
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import socket
import threading
from typing import Iterable, Optional, Tuple


Header = Tuple[str, str]


@dataclass(frozen=True)
class ResponseStep:
    method: str
    path: str
    status: int
    body: bytes = b""
    headers: Tuple[Header, ...] = ()
    expected_headers: Tuple[Header, ...] = ()
    absent_headers: Tuple[str, ...] = ()
    disconnect_after: Optional[int] = None
    auto_content_length: bool = True

    def __post_init__(self):
        if self.disconnect_after is not None and not (
            0 <= self.disconnect_after <= len(self.body)
        ):
            raise ValueError("disconnect offset is outside the response body")


@dataclass(frozen=True)
class RecordedRequest:
    method: str
    path: str
    headers: dict = field(compare=False)


class _ScenarioServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, steps):
        super().__init__(("127.0.0.1", 0), _ScenarioHandler)
        self.steps = deque(steps)
        self.requests = []
        self.errors = []
        self.state_lock = threading.Lock()

    def take_step(self, method, path, headers):
        normalized = {name.lower(): value for name, value in headers.items()}
        request = RecordedRequest(method, path, normalized)
        with self.state_lock:
            self.requests.append(request)
            if not self.steps:
                self.errors.append(f"unexpected {method} request for {path}")
                return None
            step = self.steps.popleft()
            if method != step.method or path != step.path:
                self.errors.append(
                    f"expected {step.method} {step.path}, got {method} {path}"
                )
            for name, expected in step.expected_headers:
                actual = normalized.get(name.lower())
                if actual != expected:
                    self.errors.append(
                        f"expected {name}: {expected!r}, got {actual!r}"
                    )
            for name in step.absent_headers:
                if name.lower() in normalized:
                    self.errors.append(f"unexpected request header {name}")
            return step


class _ScenarioHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, _format, *_arguments):
        pass

    def do_GET(self):
        self._respond()

    def do_HEAD(self):
        self._respond()

    def do_POST(self):
        self._respond()

    def do_PUT(self):
        self._respond()

    def do_DELETE(self):
        self._respond()

    def _respond(self):
        step = self.server.take_step(self.command, self.path, self.headers)
        if step is None:
            self.send_response(500)
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()
            self.close_connection = True
            return

        header_names = {name.lower() for name, _value in step.headers}
        self.send_response(step.status)
        if step.auto_content_length and "content-length" not in header_names:
            self.send_header("Content-Length", str(len(step.body)))
        for name, value in step.headers:
            self.send_header(name, value)
        if step.disconnect_after is not None and "connection" not in header_names:
            self.send_header("Connection", "close")
        self.end_headers()

        body = step.body
        if step.disconnect_after is not None:
            body = body[: step.disconnect_after]
        try:
            if self.command != "HEAD" and body:
                self.wfile.write(body)
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass

        if step.disconnect_after is not None:
            self.close_connection = True
            try:
                self.connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self.connection.close()


class FaultServer:
    def __init__(self, steps: Iterable[ResponseStep]):
        self._server = _ScenarioServer(steps)
        self._thread = None

    @property
    def endpoint(self):
        host, port = self._server.server_address
        return f"http://{host}:{port}"

    @property
    def requests(self):
        with self._server.state_lock:
            return list(self._server.requests)

    def start(self):
        if self._thread is not None:
            raise RuntimeError("fault server is already running")
        self._thread = threading.Thread(
            target=self._server.serve_forever,
            name="s3ar-fault-server",
            daemon=True,
        )
        self._thread.start()
        return self

    def stop(self):
        if self._thread is None:
            return
        self._server.shutdown()
        self._server.server_close()
        self._thread.join(timeout=5)
        if self._thread.is_alive():
            raise RuntimeError("fault server thread did not stop")
        self._thread = None

    def assert_complete(self):
        with self._server.state_lock:
            errors = list(self._server.errors)
            remaining = list(self._server.steps)
        if errors:
            raise AssertionError("; ".join(errors))
        if remaining:
            raise AssertionError(f"{len(remaining)} expected HTTP request(s) missing")

    def __enter__(self):
        return self.start()

    def __exit__(self, exception_type, _exception, _traceback):
        self.stop()
        if exception_type is None:
            self.assert_complete()
        return False
