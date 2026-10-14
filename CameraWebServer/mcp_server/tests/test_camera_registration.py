import os
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import requests

from mcp_server.camera_registration import CameraRegistry, RegistrationServer, resolve_camera_url
from mcp_server.esp32_client import Esp32VisionClient, Esp32VisionError
from mcp_server.tests.test_esp32_client import FakeResponse, FakeSession
from mcp_server import reporting


class CameraRegistrationTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.path = Path(self.directory.name) / "camera.json"
        self.key = "test-registration-key"
        self.registry = CameraRegistry(self.path, self.key)
        self.server = RegistrationServer(self.registry, "127.0.0.1", 0)
        self.server.start()
        self.url = "http://127.0.0.1:%d/camera/register" % self.server.port
        self.session = requests.Session()
        self.session.trust_env = False

    def tearDown(self):
        self.session.close()
        self.server.close()
        self.directory.cleanup()

    def post(self, payload, key=None):
        return self.session.post(self.url, json=payload, headers={
            "X-Camera-Key": self.key if key is None else key,
        }, timeout=3)

    def test_valid_registration_updates_existing_client_and_other_process_reader(self):
        session = FakeSession(FakeResponse({"isProcessing": False}))
        reader = CameraRegistry(self.path, self.key)
        client = Esp32VisionClient("", session=session, base_url_provider=reader.base_url)
        self.assertEqual(self.post({"ip": "192.168.1.20"}).status_code, 200)
        client.status()
        self.assertTrue(session.calls[-1][1].startswith("http://192.168.1.20/"))
        self.assertEqual(self.post({"ip": "192.168.1.30"}).status_code, 200)
        client.status()
        self.assertTrue(session.calls[-1][1].startswith("http://192.168.1.30/"))
        self.assertEqual(CameraRegistry(self.path, self.key).base_url(), "http://192.168.1.30")
        address = subprocess.check_output([sys.executable, "-B", "-c",
            "import sys; from mcp_server.camera_registration import CameraRegistry; "
            "print(CameraRegistry(sys.argv[1], sys.argv[2]).base_url())",
            str(self.path), self.key], text=True).strip()
        self.assertEqual(address, "http://192.168.1.30")
        self.assertNotIn(self.key, self.path.read_text())

    def test_wrong_or_missing_key_does_not_change_address(self):
        self.post({"ip": "192.168.1.20"})
        for key in ("incorrect", ""):
            self.assertEqual(self.post({"ip": "192.168.1.30"}, key).status_code, 403)
        self.assertEqual(self.registry.base_url(), "http://192.168.1.20")

    def test_pc_configuration_page_and_proxy_are_removed(self):
        root = self.session.get("http://127.0.0.1:%d/" % self.server.port, timeout=3)
        self.assertEqual(root.status_code, 405)
        self.assertNotIn("ESP32-CAM 上位机配置", root.text)
        config = self.session.post("http://127.0.0.1:%d/api/device/config" % self.server.port,
            json={"ssid": "new-ap"}, timeout=3)
        reboot = self.session.post("http://127.0.0.1:%d/api/device/reboot" % self.server.port, timeout=3)
        self.assertEqual(config.status_code, 404)
        self.assertEqual(reboot.status_code, 404)

    def test_invalid_ip_or_payload_is_rejected_without_state_changes(self):
        for payload in ({}, [], {"ip": 123}, {"ip": "localhost"},
                        {"ip": "192.168.1.2/path"}, {"ip": "127.0.0.1"},
                        {"ip": "0.0.0.0"}, {"ip": "224.0.0.1"},
                        {"ip": "255.255.255.255"}, {"ip": "::1"}):
            self.assertEqual(self.post(payload).status_code, 400, payload)
        self.assertFalse(self.path.exists())

    def test_malformed_and_oversized_json_and_wrong_path(self):
        headers = {"X-Camera-Key": self.key, "Content-Type": "application/json"}
        self.assertEqual(self.session.post(self.url, data="{bad", headers=headers, timeout=3).status_code, 400)
        self.assertEqual(self.session.post(self.url, data="x" * 1025, headers=headers, timeout=3).status_code, 413)
        self.assertEqual(self.session.post(self.url + "/other", json={}, headers=headers, timeout=3).status_code, 404)
        self.assertEqual(self.session.get(self.url, timeout=3).status_code, 405)

    def test_corrupt_state_and_changed_key_do_not_restore_address(self):
        self.post({"ip": "192.168.1.20"})
        self.assertEqual(CameraRegistry(self.path, "different-key").base_url(), "")
        self.path.write_text("{bad")
        self.assertEqual(self.registry.base_url(), "")

    def test_waiting_for_registration_has_clear_error_and_no_request(self):
        session = FakeSession(FakeResponse({}))
        client = Esp32VisionClient("", session=session, base_url_provider=self.registry.base_url)
        with self.assertRaisesRegex(Esp32VisionError, "等待.*上报"):
            client.status()
        self.assertEqual(session.calls, [])

    def test_reporting_uses_registered_address_and_static_fallback_remains(self):
        env = {"CAMERA_REGISTRATION_KEY": self.key, "CAMERA_STATE_FILE": str(self.path),
               "ESP32_BASE_URL": "http://192.168.1.10"}
        with patch.dict(os.environ, env):
            self.assertEqual(resolve_camera_url(), "http://192.168.1.10")
            self.post({"ip": "192.168.1.20"})
            self.assertEqual(reporting._base_url(), "http://192.168.1.20")
            session = FakeSession(FakeResponse({}))
            with patch.object(reporting, "_SESSION", session):
                self.assertTrue(reporting.report(connected=True))
            self.assertTrue(session.calls[-1][1].startswith("http://192.168.1.20/"))

    def test_state_write_failure_returns_503_and_preserves_previous_address(self):
        self.post({"ip": "192.168.1.20"})
        with patch.object(self.registry, "register", side_effect=OSError("disk failure")):
            self.assertEqual(self.post({"ip": "192.168.1.30"}).status_code, 503)
        self.assertEqual(self.registry.base_url(), "http://192.168.1.20")

    def test_registration_connection_count_is_bounded(self):
        sockets = []
        try:
            for _ in range(4):
                connection = socket.create_connection(("127.0.0.1", self.server.port), timeout=3)
                connection.sendall(b"POST /camera/register HTTP/1.1\r\n")
                sockets.append(connection)
            self.assertEqual(self.post({"ip": "192.168.1.20"}).status_code, 503)
            self.assertFalse(self.path.exists())
        finally:
            for connection in sockets:
                connection.close()

    def test_slow_headers_have_total_deadline(self):
        connection = socket.create_connection(("127.0.0.1", self.server.port), timeout=4)
        stopped = threading.Event()
        def drip():
            while not stopped.wait(0.6):
                try:
                    connection.sendall(b"a")
                except OSError:
                    return
        connection.sendall(b"POST /camera/register HTTP/1.1\r\nX-Slow: ")
        sender = threading.Thread(target=drip)
        started = time.monotonic()
        sender.start()
        try:
            connection.recv(1)
            self.assertLess(time.monotonic() - started, 3.9)
        finally:
            stopped.set()
            connection.close()
            sender.join()


if __name__ == "__main__":
    unittest.main()
