import unittest
from unittest.mock import patch

import requests

from mcp_server.esp32_client import Esp32VisionClient, Esp32VisionError


class FakeResponse:
    def __init__(self, payload=None, status_code=200, content=b"jpeg"):
        self._payload = payload
        self.status_code = status_code
        self.content = content

    def raise_for_status(self):
        if self.status_code >= 400:
            raise RuntimeError("http failure")

    def json(self):
        return self._payload


class FakeSession:
    def __init__(self, response):
        self.response = response
        self.calls = []

    def request(self, method, url, **kwargs):
        self.calls.append((method, url, kwargs))
        return self.response

    def get(self, url, **kwargs):
        return self.request("GET", url, **kwargs)


class SequencedSession:
    """Returns queued payloads in order, one per GET."""

    def __init__(self, payloads):
        self.payloads = list(payloads)
        self.calls = []

    def get(self, url, **kwargs):
        self.calls.append(url)
        payload = self.payloads.pop(0) if self.payloads else {}
        return FakeResponse(payload)


class ConnectFailThenOkSession:
    """第一次 GET 抛连接错误，第二次返回正常响应。"""

    def __init__(self, error, response):
        self.error = error
        self.response = response
        self.calls = []

    def get(self, url, **kwargs):
        self.calls.append((url, kwargs))
        if len(self.calls) == 1:
            raise self.error
        return self.response


class AlwaysFailSession:
    """每次 GET 都抛同一个异常。"""

    def __init__(self, error):
        self.error = error
        self.calls = []

    def get(self, url, **kwargs):
        self.calls.append((url, kwargs))
        raise self.error


class TimeoutConfigurationTests(unittest.TestCase):
    def test_default_connect_timeout_is_eight_seconds(self):
        client = Esp32VisionClient("http://192.168.1.2", session=FakeSession(FakeResponse()))
        self.assertEqual(client.connect_timeout, 8.0)

    def test_connect_timeout_is_configurable(self):
        client = Esp32VisionClient(
            "http://192.168.1.2",
            connect_timeout=15,
            session=FakeSession(FakeResponse()),
        )
        self.assertEqual(client.connect_timeout, 15.0)
        self.assertEqual(client.timeout, 100.0)

    def test_connect_timeout_bounds_are_validated(self):
        for bad in (0, 0.5, 61, 300):
            with self.assertRaises(ValueError):
                Esp32VisionClient("http://192.168.1.2", connect_timeout=bad)

    def test_get_uses_configured_connect_timeout(self):
        session = FakeSession(FakeResponse({"ok": True}))
        client = Esp32VisionClient("http://192.168.1.2", connect_timeout=12, session=session)

        client.status()

        self.assertEqual(session.calls[0][2]["timeout"], (12.0, 5))


class ConnectRetryTests(unittest.TestCase):
    def test_connect_timeout_is_retried_once_before_send(self):
        session = ConnectFailThenOkSession(
            requests.exceptions.ConnectTimeout("boom"),
            FakeResponse({"isProcessing": False, "messageSeq": 1}),
        )
        client = Esp32VisionClient("http://192.168.1.2", session=session)

        with patch("mcp_server.esp32_client.time.sleep") as sleep:
            payload = client.status()

        self.assertEqual(payload["messageSeq"], 1)
        self.assertEqual(len(session.calls), 2)
        sleep.assert_called_once_with(0.3)

    def test_connect_failure_that_keeps_failing_stops_after_two_attempts(self):
        session = AlwaysFailSession(requests.exceptions.ConnectTimeout("boom"))
        client = Esp32VisionClient("http://192.168.1.2", session=session)

        with patch("mcp_server.esp32_client.time.sleep"):
            with self.assertRaises(Esp32VisionError):
                client.status()

        # 只重试一次，绝不无限重试。
        self.assertEqual(len(session.calls), 2)

    def test_trigger_wait_never_retries_after_request_was_sent(self):
        """读超时意味着请求已经发出，重试就等于重复拍照。"""
        session = AlwaysFailSession(requests.exceptions.ReadTimeout("slow"))
        client = Esp32VisionClient("http://192.168.1.2", session=session)

        with patch("mcp_server.esp32_client.time.sleep") as sleep:
            with self.assertRaises(Esp32VisionError):
                client.trigger_wait("问题", source="mcp")

        self.assertEqual(len(session.calls), 1)
        sleep.assert_not_called()

    def test_connection_reset_mid_response_is_not_retried(self):
        """响应中途断开可能已经被设备处理，不能重试。"""
        session = AlwaysFailSession(requests.exceptions.ConnectionError("reset"))
        client = Esp32VisionClient("http://192.168.1.2", session=session)

        with patch("mcp_server.esp32_client.time.sleep") as sleep:
            with self.assertRaises(Esp32VisionError):
                client.trigger_wait("问题", source="mcp")

        self.assertEqual(len(session.calls), 1)
        sleep.assert_not_called()


class Esp32VisionClientTests(unittest.TestCase):
    def test_trigger_wait_polls_status_when_not_finished(self):
        session = SequencedSession([
            {"accepted": True, "finished": False, "seqBefore": 7, "isProcessing": True},
            {"isProcessing": True, "messageSeq": 7, "lastSuccess": False},
            {"isProcessing": False, "messageSeq": 8,
             "lastSuccess": True, "lastDurationMs": 3300, "lastMessage": "一只猫"},
        ])
        client = Esp32VisionClient("http://192.168.1.2", session=session)

        result = client.trigger_wait("问题", source="mcp")

        self.assertTrue(result["success"])
        self.assertEqual(result["message"], "一只猫")
        self.assertEqual(result["durationMs"], 3300)
        # 第一次打 trigger_wait，后面都是轮询 status
        self.assertIn("/vision/trigger_wait?", session.calls[0])
        self.assertTrue(session.calls[1].endswith("/vision/status"))
        self.assertTrue(session.calls[2].endswith("/vision/status"))

    def test_trigger_wait_reports_busy_when_rejected(self):
        session = FakeSession(FakeResponse({
            "accepted": False, "finished": False, "message": "设备忙：正在识别",
        }))
        client = Esp32VisionClient("http://192.168.1.2", session=session)

        with self.assertRaises(Esp32VisionError) as ctx:
            client.trigger_wait("问题", source="mcp")
        self.assertIn("设备忙", str(ctx.exception))

    def test_trigger_wait_appends_mcp_source(self):
        session = FakeSession(FakeResponse({
            "finished": True, "success": True,
            "durationMs": 10, "message": "ok",
        }))
        client = Esp32VisionClient("http://192.168.1.2", session=session)

        client.trigger_wait("问题", source="mcp")

        self.assertEqual(
            session.calls[0][1],
            "http://192.168.1.2/vision/trigger_wait?prompt=%E9%97%AE%E9%A2%98&source=mcp",
        )

    def test_report_pushes_status_without_raising(self):
        session = FakeSession(FakeResponse({"success": True}))
        client = Esp32VisionClient("http://192.168.1.2", session=session)

        client.report(connected=True, endpoint="wss://example/mcp", log="hi", level="warn")

        url = session.calls[0][1]
        self.assertTrue(url.startswith("http://192.168.1.2/mcp/report?"))
        self.assertIn("connected=1", url)
        self.assertIn("log=hi", url)

    def test_trigger_wait_returns_success_payload(self):
        session = FakeSession(FakeResponse({
            "finished": True,
            "success": True,
            "durationMs": 2100,
            "message": "一张桌子",
        }))
        client = Esp32VisionClient("http://192.168.1.2", session=session)

        result = client.trigger_wait("画面里有什么？")

        self.assertTrue(result["success"])
        self.assertEqual(result["message"], "一张桌子")
        self.assertEqual(session.calls[0][0], "GET")
        self.assertEqual(session.calls[0][1], "http://192.168.1.2/vision/trigger_wait?prompt=%E7%94%BB%E9%9D%A2%E9%87%8C%E6%9C%89%E4%BB%80%E4%B9%88%EF%BC%9F")

    def test_http_failure_is_reported_as_vision_error(self):
        session = FakeSession(FakeResponse(status_code=503))
        client = Esp32VisionClient("http://192.168.1.2", session=session)

        with self.assertRaises(Esp32VisionError):
            client.status()

    def test_download_image_returns_bytes(self):
        session = FakeSession(FakeResponse(content=b"jpeg-data"))
        client = Esp32VisionClient("http://192.168.1.2", session=session)

        image = client.download_image()

        self.assertEqual(image, b"jpeg-data")
        self.assertEqual(session.calls[0][1], "http://192.168.1.2/vision/image")


if __name__ == "__main__":
    unittest.main()
