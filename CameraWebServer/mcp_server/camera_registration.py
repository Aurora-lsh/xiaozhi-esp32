"""Authenticated camera registration and address sharing across bridge processes."""

import hashlib
import hmac
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import ipaddress
import json
import logging
import os
import socket
from pathlib import Path
import tempfile
import threading

LOGGER = logging.getLogger("CAMERA_REGISTRATION")
BASE_DIR = Path(__file__).resolve().parent


def _camera_ip(value):
    if not isinstance(value, str):
        raise ValueError("ip must be an IPv4 address")
    address = ipaddress.IPv4Address(value)
    if (address.is_unspecified or address.is_loopback or address.is_multicast
            or address.is_reserved or int(address) == 0xffffffff):
        raise ValueError("ip must be a reachable unicast IPv4 address")
    return str(address)


class CameraRegistry:
    def __init__(self, path, key):
        self.path = Path(path)
        self.key = key
        self.key_id = hashlib.sha256(key.encode("utf-8")).hexdigest()
        self.lock = threading.Lock()

    def base_url(self):
        try:
            state = json.loads(self.path.read_text(encoding="utf-8"))
            if state["key_id"] != self.key_id:
                return ""
            return "http://" + _camera_ip(state["ip"])
        except (OSError, ValueError, KeyError, TypeError):
            return ""

    def register(self, ip):
        ip = _camera_ip(ip)
        with self.lock:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            temporary_path = None
            try:
                with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8",
                        dir=self.path.parent, delete=False) as stream:
                    temporary_path = Path(stream.name)
                    json.dump({"ip": ip, "key_id": self.key_id}, stream)
                os.replace(temporary_path, self.path)
            finally:
                if temporary_path is not None:
                    temporary_path.unlink(missing_ok=True)
        LOGGER.info("Camera address registered: http://%s", ip)
        return "http://" + ip


def registry_from_env():
    path = Path(os.getenv("CAMERA_STATE_FILE") or "runtime/camera_address.json")
    if not path.is_absolute():
        path = BASE_DIR / path
    return CameraRegistry(
        path,
        os.getenv("CAMERA_REGISTRATION_KEY", ""),
    )


def resolve_camera_url():
    if os.getenv("CAMERA_REGISTRATION_KEY", ""):
        address = registry_from_env().base_url()
        if address:
            return address
    return os.getenv("ESP32_BASE_URL", "").strip().rstrip("/")


class _BoundedHTTPServer(ThreadingHTTPServer):
    def __init__(self, address, handler):
        self.slots = threading.BoundedSemaphore(4)
        super().__init__(address, handler)

    def process_request(self, request, client_address):
        if not self.slots.acquire(blocking=False):
            try:
                request.settimeout(0.1)
                request.sendall(b"HTTP/1.0 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n")
            except OSError:
                pass
            finally:
                self.shutdown_request(request)
            return
        try:
            super().process_request(request, client_address)
        except Exception:
            self.slots.release()
            raise

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self.slots.release()

    def handle_error(self, request, client_address):
        LOGGER.debug("Registration connection closed before response")


class RegistrationServer:
    def __init__(self, registry, host="0.0.0.0", port=8765):
        if not registry.key or len(registry.key) > 128 or any(
                ord(character) < 33 or ord(character) > 126 for character in registry.key):
            raise ValueError("CAMERA_REGISTRATION_KEY must contain 1-128 printable ASCII characters without spaces")
        self.registry = registry
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def setup(self):
                super().setup()
                self.connection.settimeout(3)
                self.deadline = threading.Timer(3, self.expire)
                self.deadline.daemon = True
                self.deadline.start()

            def expire(self):
                try:
                    self.connection.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass

            def finish(self):
                self.deadline.cancel()
                super().finish()

            def log_message(self, format, *args):
                pass

            def reply(self, status, payload):
                body = json.dumps(payload).encode("utf-8")
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self):
                self.reply(405, {"error": "POST required"})

            def do_POST(self):
                if self.path != "/camera/register":
                    self.reply(404, {"error": "unknown endpoint"})
                    return
                supplied = self.headers.get("X-Camera-Key", "").encode("utf-8")
                if not hmac.compare_digest(supplied, registry.key.encode("utf-8")):
                    self.reply(403, {"error": "invalid key"})
                    return
                try:
                    length = int(self.headers.get("Content-Length", "0"))
                    if self.headers.get("Transfer-Encoding") or length <= 0:
                        raise ValueError("Content-Length required")
                    if length > 1024:
                        self.reply(413, {"error": "request too large"})
                        return
                    payload = json.loads(self.rfile.read(length))
                    if not isinstance(payload, dict):
                        raise ValueError("JSON object required")
                    ip = _camera_ip(payload.get("ip"))
                except (ValueError, OSError):
                    self.reply(400, {"error": "invalid JSON or IPv4 address"})
                    return
                try:
                    address = owner.registry.register(ip)
                except OSError:
                    LOGGER.error("Could not save camera registration")
                    self.reply(503, {"error": "could not save address"})
                    return
                self.reply(200, {"success": True, "base_url": address})

        self.httpd = _BoundedHTTPServer((host, int(port)), Handler)
        self.port = self.httpd.server_port
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)

    def start(self):
        self.thread.start()
        LOGGER.info("Camera registration listening on %s:%d/camera/register",
                    self.httpd.server_address[0], self.port)

    def close(self):
        if self.thread.is_alive():
            self.httpd.shutdown()
            self.thread.join()
        self.httpd.server_close()


def start_registration_from_env():
    if not os.getenv("CAMERA_REGISTRATION_KEY", ""):
        return None
    server = RegistrationServer(registry_from_env(),
        os.getenv("CAMERA_REGISTRATION_HOST", "0.0.0.0"),
        int(os.getenv("CAMERA_REGISTRATION_PORT", "8765")))
    server.start()
    return server
