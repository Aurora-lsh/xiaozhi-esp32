"""Bridge the local ESP32 stdio MCP server to Xiaozhi's WebSocket endpoint."""

from __future__ import annotations

import asyncio
import logging
import os
from pathlib import Path
import subprocess
import sys
from typing import BinaryIO

from dotenv import load_dotenv
from websockets.legacy.client import connect

try:
    from . import reporting
    from .camera_registration import start_registration_from_env
except ImportError:
    import reporting
    from camera_registration import start_registration_from_env


BASE_DIR = Path(__file__).resolve().parent
load_dotenv(BASE_DIR / ".env", encoding="utf-8-sig")
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s - %(name)s - %(levelname)s - %(message)s",
    stream=sys.stderr,
)
LOGGER = logging.getLogger("XIAOZHI_ESP32_MCP")
INITIAL_BACKOFF_SECONDS = 1
MAX_BACKOFF_SECONDS = 600
REPORT_INTERVAL_SECONDS = 5


def _endpoint_label(endpoint: str) -> str:
    """Keep only the host/path so the token is never shown on the web page."""
    head = endpoint.split("?", 1)[0]
    return head[:90]


class _BridgeState:
    """Shared connection state pushed to the ESP32 web UI."""

    def __init__(self, endpoint: str) -> None:
        self.endpoint = endpoint
        self.connected = False

    def snapshot(self):
        return self.connected, self.endpoint


async def _report_status_loop(state: _BridgeState) -> None:
    """Push bridge connection state to the firmware every few seconds."""
    while True:
        try:
            connected, endpoint = state.snapshot()
            await asyncio.to_thread(
                reporting.report, connected,
                _endpoint_label(endpoint) if endpoint else None,
            )
        except Exception as error:  # pragma: no cover - diagnostics only
            LOGGER.debug("status report failed: %s", error)
        await asyncio.sleep(REPORT_INTERVAL_SECONDS)


async def _push_log(message: str, level: str = "info") -> None:
    """Best-effort one-line log push to the firmware web page."""
    try:
        await asyncio.to_thread(reporting.report, None, None, message, level)
    except Exception as error:  # pragma: no cover - diagnostics only
        LOGGER.debug("log push failed: %s", error)


def _write_line(stream: BinaryIO, data: bytes) -> None:
    stream.write(data)
    stream.flush()


async def _websocket_to_process(websocket, process: subprocess.Popen[bytes]) -> None:
    assert process.stdin is not None
    async for message in websocket:
        if isinstance(message, bytes):
            message = message.decode("utf-8")
        await asyncio.to_thread(_write_line, process.stdin, (message + "\n").encode("utf-8"))
    raise RuntimeError("Xiaozhi MCP WebSocket closed")


async def _process_to_websocket(process: subprocess.Popen[bytes], websocket) -> None:
    assert process.stdout is not None
    while True:
        line = await asyncio.to_thread(process.stdout.readline)
        if not line:
            raise RuntimeError("ESP32 MCP process stopped")
        await websocket.send(line.decode("utf-8").rstrip("\r\n"))


# Rich/FastMCP 会用制表符画框，这类行没有诊断价值，转发到固件只会把
# 真正有用的日志挤出环形缓冲，因此过滤掉。
_DECOR_CHARS = set("\u2500\u2501\u2502\u2503\u250c\u2510\u2514\u2518\u251c\u2524"
                   "\u252c\u2534\u253c\u2580\u2584\u2588\u258c\u2590\u2591\u2592\u2593"
                   "\u2550\u2551\u2554\u2557\u255a\u255d\u2560\u2563\u2566\u2569\u256c")


def _is_decorative(text: str) -> bool:
    """True when a line carries no real information (banner/frame drawing)."""
    stripped = text.strip()
    if not stripped:
        return True
    meaningful = [c for c in stripped if c not in _DECOR_CHARS and not c.isspace()]
    return len(meaningful) == 0


async def _log_stderr(process: subprocess.Popen[bytes]) -> None:
    assert process.stderr is not None
    while True:
        line = await asyncio.to_thread(process.stderr.readline)
        if not line:
            return
        text = line.decode("utf-8", errors="replace").rstrip()
        if not text:
            continue
        LOGGER.info("[esp32-mcp] %s", text)
        # Forward the child's diagnostics to the firmware so the web page shows
        # what the MCP server is doing without needing the PC console.
        if not _is_decorative(text):
            await _push_log(text, "info")


async def _stop_process(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    process.terminate()
    try:
        await asyncio.to_thread(process.wait, 5)
    except subprocess.TimeoutExpired:
        process.kill()
        await asyncio.to_thread(process.wait)


async def _run_connected(endpoint: str, state: _BridgeState) -> None:
    LOGGER.info("Connecting to the Xiaozhi MCP endpoint")
    async with connect(endpoint, ping_interval=20, ping_timeout=20, max_size=2 * 1024 * 1024) as websocket:
        LOGGER.info("Connected to the Xiaozhi MCP endpoint")
        state.connected = True
        await _push_log("小智 MCP 已连接", "info")

        child_env = os.environ.copy()
        child_env["CAMERA_REGISTRATION_MANAGED_BY_BRIDGE"] = "1"
        child_env["PYTHONIOENCODING"] = "utf-8"
        child_env["PYTHONUTF8"] = "1"
        # FastMCP 的 rich 日志会折行并画框，转发到固件网页很难看；
        # 关掉后每个事件只占一行。
        child_env["FASTMCP_ENABLE_RICH_LOGGING"] = "false"
        process = subprocess.Popen(
            [sys.executable, "-B", str(BASE_DIR / "camera_mcp.py")],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            cwd=str(BASE_DIR),
            env=child_env,
        )
        tasks = [
            asyncio.create_task(_websocket_to_process(websocket, process)),
            asyncio.create_task(_process_to_websocket(process, websocket)),
            asyncio.create_task(_log_stderr(process)),
            asyncio.create_task(_report_status_loop(state)),
        ]
        try:
            done, pending = await asyncio.wait(tasks[:2], return_when=asyncio.FIRST_COMPLETED)
            for task in pending:
                task.cancel()
            for task in done:
                task.result()
        finally:
            state.connected = False
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            await _stop_process(process)


async def _run_bridge() -> None:
    endpoint = os.getenv("MCP_ENDPOINT", "").strip()
    if not endpoint:
        raise RuntimeError("MCP_ENDPOINT is not configured in mcp_server/.env")
    state = _BridgeState(endpoint)
    backoff = INITIAL_BACKOFF_SECONDS
    while True:
        try:
            await _run_connected(endpoint, state)
            backoff = INITIAL_BACKOFF_SECONDS
        except asyncio.CancelledError:
            raise
        except Exception as error:
            state.connected = False
            LOGGER.warning("Connection stopped: %s: %r", type(error).__name__, error)
            LOGGER.info("Reconnecting in %d seconds", backoff)
            await _push_log("MCP 连接中断：%s，%d 秒后重连" % (type(error).__name__, backoff), "warn")
            await asyncio.sleep(backoff)
            backoff = min(backoff * 2, MAX_BACKOFF_SECONDS)


async def main() -> None:
    registration = start_registration_from_env()
    try:
        await _run_bridge()
    finally:
        if registration is not None:
            await asyncio.to_thread(registration.close)


if __name__ == "__main__":
    if sys.platform == "win32":
        asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        LOGGER.info("Stopped by user")
