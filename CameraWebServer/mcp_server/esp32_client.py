"""Bounded HTTP calls to the existing ESP32 vision firmware."""

import time
from urllib.parse import urlsplit

import requests
from urllib3.exceptions import (
    ConnectTimeoutError,
    NameResolutionError,
    NewConnectionError,
    ProtocolError,
    ReadTimeoutError,
)


def _is_before_send_failure(error):
    """判断失败是否发生在“请求还没发出去”的连接阶段。

    只有连接阶段的失败重试才安全：此时请求必然没有到达设备，重发不会让设备
    多触发一次拍照。判定依据是异常的因果链里出现了 urllib3 的连接阶段错误
    （DNS 解析失败、TCP 建连失败、连接超时）。

    一旦链里出现 ProtocolError / ReadTimeoutError，说明请求已经发出（甚至已
    被设备处理），必须返回 False —— 对 /vision/trigger_wait 重试就等于重复拍照。
    """
    current = error
    for _ in range(12):
        if current is None:
            break
        if isinstance(current, (ProtocolError, ReadTimeoutError)):
            return False
        if isinstance(current, (ConnectTimeoutError, NewConnectionError,
                                NameResolutionError)):
            return True
        current = current.__cause__ or current.__context__
    # 兜底：requests 只有在连接阶段才会抛出 ConnectTimeout。
    return isinstance(error, requests.exceptions.ConnectTimeout)


class Esp32VisionError(RuntimeError):
    pass


_CONNECT_ATTEMPTS = 2
_CONNECT_RETRY_DELAY_SECONDS = 0.3


class Esp32VisionClient:
    def __init__(self, base_url, timeout=100.0, connect_timeout=8.0, session=None):
        url = urlsplit(base_url)
        if url.scheme not in ("http", "https") or not url.hostname or url.query or url.fragment:
            raise ValueError("ESP32_BASE_URL must be an http(s) device URL")
        if not 1 <= float(timeout) <= 300:
            raise ValueError("ESP32_TIMEOUT_SECONDS must be between 1 and 300")
        if not 1 <= float(connect_timeout) <= 60:
            raise ValueError("ESP32_CONNECT_TIMEOUT_SECONDS must be between 1 and 60")
        self.base_url = base_url.rstrip("/")
        self.timeout = float(timeout)
        self.connect_timeout = float(connect_timeout)
        self.session = session if session is not None else requests.Session()
        # Local camera traffic must not pass through the PC's HTTP proxy.
        self.session.trust_env = False

    def _get(self, path, timeout):
        """带连接阶段重试的 GET。

        手机热点（尤其 WPA3 + 省电调度）会出现几百毫秒到数秒的瞬时“黑洞”，
        旧的固定 3 秒连接超时会把这种抖动误报成“设备离线”。这里把连接超时
        变成可配置（ESP32_CONNECT_TIMEOUT_SECONDS，默认 8 秒），并且只在
        连接阶段失败（DNS/建连失败或连接超时，请求必然还没发出）
        时重试一次；一旦判断请求可能已经发出就绝不重试，避免重复拍照。
        """
        url = self.base_url + path
        last_error = None
        for attempt in range(_CONNECT_ATTEMPTS):
            try:
                response = self.session.get(
                    url, timeout=(self.connect_timeout, timeout)
                )
                response.raise_for_status()
                return response
            except Exception as error:
                last_error = error
                # 只有“请求还没发出去”的连接阶段失败才重试；读超时、HTTP
                # 错误码、响应中途断开等一律不重试，避免重复触发拍照。
                if _is_before_send_failure(error) and attempt + 1 < _CONNECT_ATTEMPTS:
                    time.sleep(_CONNECT_RETRY_DELAY_SECONDS)
                    continue
                break
        # Never retry a trigger: it may already be executing on the device.
        raise Esp32VisionError(
            "ESP32 请求失败或超时，请确认设备在线；未自动重复触发拍照"
        ) from last_error

    def _json(self, path, timeout):
        response = self._get(path, timeout)
        try:
            payload = response.json()
        except ValueError as error:
            raise Esp32VisionError("ESP32 返回了无效 JSON") from error
        if not isinstance(payload, dict):
            raise Esp32VisionError("ESP32 返回格式错误")
        return payload

    def trigger_wait(self, prompt=None, source=None):
        """触发一次识别并取回结果。

        固件侧不再把整个识别过程压在 HTTP 处理函数里（es_http_server 只有一条
        任务，长阻塞会让所有接口排队）。它会先投递请求并短暂等待；若这段时间内
        没出结果，就返回 finished=false，由这里轮询 /vision/status 等待完成。
        """
        path = "/vision/trigger_wait"
        params = []
        if prompt:
            params.append("prompt=" + requests.utils.quote(str(prompt), safe=""))
        if source:
            params.append("source=" + requests.utils.quote(str(source), safe=""))
        if params:
            path += "?" + "&".join(params)

        payload = self._json(path, self.timeout)
        if payload.get("finished") is True:
            return self._result(payload)

        if payload.get("accepted") is False:
            raise Esp32VisionError(
                str(payload.get("message") or "设备忙，请稍后再试；未重复触发")[:400]
            )
        return self._poll_result(payload)

    def _poll_result(self, first_payload):
        """轮询 /vision/status，直到本轮结果就绪。

        固件在每轮识别结束（无论成功或失败）都会 publishMessage()，使 messageSeq
        前进，所以判定条件是“序号已前进 且 不再处理中”。序号拿不到时退化为
        “观察到进入处理中、随后又回到空闲”，避免直接误取上一轮结果。
        """
        deadline = time.monotonic() + self.timeout
        message_seq = first_payload.get("seqBefore")
        have_seq = isinstance(message_seq, int)
        saw_processing = False
        interval = 0.4

        while True:
            if time.monotonic() >= deadline:
                raise Esp32VisionError("等待设备识别完成超时，请稍后查看网页状态；未重复触发")
            status = self._json("/vision/status", 8)

            if status.get("isProcessing") is True:
                saw_processing = True
                time.sleep(interval)
                interval = min(interval * 1.5, 2.0)
                continue

            seq = status.get("messageSeq")
            if have_seq:
                done = isinstance(seq, int) and seq > message_seq
            else:
                # 拿不到起点序号时，至少要观察到一次“正在处理”，否则可能是旧结果
                done = saw_processing
            if done:
                return self._result({
                    "success": status.get("lastSuccess"),
                    "durationMs": status.get("lastDurationMs"),
                    "message": status.get("lastMessage"),
                })

            time.sleep(interval)
            interval = min(interval * 1.5, 2.0)

    @staticmethod
    def _result(payload):
        if payload.get("success") is not True:
            raise Esp32VisionError(str(payload.get("message") or "设备识别失败")[:400])
        message = payload.get("message")
        if not isinstance(message, str) or not message.strip():
            raise Esp32VisionError("识别返回了空文本")
        return payload

    def status(self):
        return self._json("/vision/status", 5)

    def mcp_status(self):
        """Read the firmware-side MCP bridge status (not the PC-side one)."""
        return self._json("/mcp/status", 5)

    def report(self, connected=None, endpoint=None, log=None, level="info"):
        """Push bridge status and/or a log line to the firmware for the web UI.

        Never raises: reporting is best-effort and must not break MCP calls.
        """
        params = []
        if connected is not None:
            params.append("connected=" + ("1" if connected else "0"))
        if endpoint:
            params.append("endpoint=" + requests.utils.quote(str(endpoint), safe=""))
        if log:
            params.append("log=" + requests.utils.quote(str(log), safe=""))
            params.append("level=" + requests.utils.quote(str(level), safe=""))
        if not params:
            return None
        path = "/mcp/report?" + "&".join(params)
        try:
            return self._json(path, 5)
        except Esp32VisionError:
            return None

    def download_image(self):
        return self._get("/vision/image", 10).content
