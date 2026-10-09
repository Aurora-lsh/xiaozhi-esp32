"""Best-effort push of MCP bridge status and logs to the ESP32 web UI.

Reporting must never break MCP calls: every failure is swallowed and only
logged at debug level.
"""

import logging
import os
from urllib.parse import quote

import requests

LOGGER = logging.getLogger("MCP_REPORT")
_ENDPOINT_MAX = 90
_DEFAULT_CONNECT_TIMEOUT = 8.0


def _connect_timeout():
    """与 esp32_client 共用同一个连接超时配置。

    上报是 best-effort 的，这里绝不抛异常：配置非法时回退到默认值。
    """
    raw = os.getenv("ESP32_CONNECT_TIMEOUT_SECONDS", "")
    try:
        value = float(raw)
    except (TypeError, ValueError):
        return _DEFAULT_CONNECT_TIMEOUT
    if not 1 <= value <= 60:
        return _DEFAULT_CONNECT_TIMEOUT
    return value


def _base_url():
    return os.getenv("ESP32_BASE_URL", "").strip().rstrip("/")


_SESSION = None


def _session():
    """Reuse one connection pool: the ESP32 httpd has few sockets, and a fresh
    connection every 5 seconds churns them unnecessarily."""
    global _SESSION
    if _SESSION is None:
        _SESSION = requests.Session()
        # Local camera traffic must not pass through the PC's HTTP proxy.
        _SESSION.trust_env = False
    return _SESSION


def report(connected=None, endpoint=None, log=None, level="info"):
    """Push status and/or one log line to the firmware's /mcp/report.

    Returns True when the firmware accepted the report.
    """
    base = _base_url()
    if not base:
        return False

    params = []
    if connected is not None:
        params.append("connected=" + ("1" if connected else "0"))
    if endpoint:
        params.append("endpoint=" + quote(str(endpoint)[:_ENDPOINT_MAX], safe=""))
    if log:
        params.append("log=" + quote(str(log)[:300], safe=""))
        params.append("level=" + quote(str(level or "info")[:7], safe=""))
    if not params:
        return False

    try:
        response = _session().get(
            base + "/mcp/report?" + "&".join(params),
            timeout=(_connect_timeout(), 5),
        )
        return response.status_code < 400
    except Exception as error:  # pragma: no cover - diagnostics only
        LOGGER.debug("report to ESP32 failed: %s", error)
        return False