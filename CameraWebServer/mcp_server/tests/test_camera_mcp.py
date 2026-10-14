import unittest
from unittest.mock import patch

from mcp_server import camera_mcp


class CameraMcpTests(unittest.TestCase):
    def test_look_at_camera_returns_description(self):
        fake = type("FakeClient", (), {
            "trigger_wait": lambda self, prompt=None, source=None: {
                "finished": True, "success": True,
                "durationMs": 1200, "message": "看到一个杯子",
            },
            "status": lambda self: {"imageBytes": 1234, "luma": 80},
        })()

        with patch.object(camera_mcp, "_CLIENT", fake):
            result = camera_mcp.look_at_camera("画面里有什么？")

        self.assertTrue(result["success"])
        self.assertEqual(result["description"], "看到一个杯子")
        self.assertEqual(result["duration_ms"], 1200)

    def test_watch_camera_rejects_invalid_duration(self):
        result = camera_mcp.watch_camera("观察动作", duration_seconds=11)

        self.assertFalse(result["success"])
        self.assertIn("duration_seconds", result["error"])

    def test_camera_link_uses_current_registered_address(self):
        fake = type("FakeClient", (), {
            "base_url": "http://192.168.1.20",
            "trigger_wait": lambda self, **kwargs: {
                "finished": True, "success": True, "message": "杯子",
            },
        })()
        with patch.object(camera_mcp, "_CLIENT", fake), patch.object(
                camera_mcp, "resolve_camera_url", return_value="http://192.168.1.30"):
            result = camera_mcp.look_at_camera("画面里有什么？")
        self.assertEqual(result["web_url"], "http://192.168.1.30/vision")


if __name__ == "__main__":
    unittest.main()
