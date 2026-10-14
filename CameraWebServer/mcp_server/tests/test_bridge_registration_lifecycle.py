import asyncio
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import requests

from mcp_server import xiaozhi_mcp_bridge as bridge
from mcp_server.camera_registration import CameraRegistry, RegistrationServer


class BridgeRegistrationLifecycleTests(unittest.IsolatedAsyncioTestCase):
    async def test_listener_available_before_cloud_connection_and_closed_on_cancel(self):
        with tempfile.TemporaryDirectory() as directory:
            registry = CameraRegistry(Path(directory) / "camera.json", "test-lifecycle-key")
            server = RegistrationServer(registry, "127.0.0.1", 0)
            starts = []

            def start():
                starts.append(True)
                server.start()
                return server

            def register():
                with requests.Session() as session:
                    session.trust_env = False
                    return session.post("http://127.0.0.1:%d/camera/register" % server.port,
                        headers={"X-Camera-Key": "test-lifecycle-key"},
                        json={"ip": "192.168.1.55"}, timeout=3).status_code

            async def before_cloud_connection():
                self.assertEqual(await asyncio.to_thread(register), 200)
                self.assertEqual(registry.base_url(), "http://192.168.1.55")
                raise asyncio.CancelledError()

            try:
                with patch.object(bridge, "start_registration_from_env", side_effect=start), \
                     patch.object(bridge, "_run_bridge", side_effect=before_cloud_connection):
                    with self.assertRaises(asyncio.CancelledError):
                        await bridge.main()
                self.assertEqual(len(starts), 1)
                self.assertFalse(server.thread.is_alive())
            finally:
                server.close()
