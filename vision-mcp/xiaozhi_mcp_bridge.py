"""Bridge a local stdio MCP server to a Xiaozhi MCP WebSocket endpoint."""

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


BASE_DIR = Path(__file__).resolve().parent
load_dotenv(BASE_DIR / ".env")

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s - %(name)s - %(levelname)s - %(message)s",
    stream=sys.stderr,
)
LOGGER = logging.getLogger("XIAOZHI_MCP_BRIDGE")

INITIAL_BACKOFF_SECONDS = 1
MAX_BACKOFF_SECONDS = 600


def _write_process_line(stream: BinaryIO, data: bytes) -> None:
    stream.write(data)
    stream.flush()


async def _websocket_to_process(
    websocket, process: subprocess.Popen[bytes]
) -> None:
    assert process.stdin is not None
    async for message in websocket:
        if isinstance(message, bytes):
            message = message.decode("utf-8")
        await asyncio.to_thread(
            _write_process_line, process.stdin, (message + "\n").encode("utf-8")
        )
    raise RuntimeError("Xiaozhi MCP WebSocket closed")


async def _process_to_websocket(
    process: subprocess.Popen[bytes], websocket
) -> None:
    assert process.stdout is not None
    while True:
        line = await asyncio.to_thread(process.stdout.readline)
        if not line:
            raise RuntimeError("camera MCP process stopped")
        await websocket.send(line.decode("utf-8").rstrip("\r\n"))


async def _log_process_stderr(process: subprocess.Popen[bytes]) -> None:
    assert process.stderr is not None
    while True:
        line = await asyncio.to_thread(process.stderr.readline)
        if not line:
            return
        LOGGER.info("[camera-mcp] %s", line.decode("utf-8", errors="replace").rstrip())


async def _stop_process(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    process.terminate()
    try:
        await asyncio.to_thread(process.wait, 5)
    except subprocess.TimeoutExpired:
        process.kill()
        await asyncio.to_thread(process.wait)


async def _run_connected(endpoint: str) -> None:
    LOGGER.info("Connecting to the Xiaozhi MCP endpoint")
    # The legacy asyncio client avoids a websockets 15+ cleanup bug that can
    # mask an early TLS reset with "recv_messages" AttributeError.
    async with connect(
        endpoint,
        ping_interval=20,
        ping_timeout=20,
        max_size=2 * 1024 * 1024,
    ) as websocket:
        LOGGER.info("Connected to the Xiaozhi MCP endpoint")
        child_env = os.environ.copy()
        child_env["PYTHONIOENCODING"] = "utf-8"
        child_env["PYTHONUTF8"] = "1"
        process = subprocess.Popen(
            [sys.executable, str(BASE_DIR / "camera_mcp.py")],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            cwd=str(BASE_DIR),
            env=child_env,
        )

        tasks = [
            asyncio.create_task(_websocket_to_process(websocket, process)),
            asyncio.create_task(_process_to_websocket(process, websocket)),
            asyncio.create_task(_log_process_stderr(process)),
        ]
        try:
            done, pending = await asyncio.wait(
                tasks[:2], return_when=asyncio.FIRST_COMPLETED
            )
            for task in pending:
                task.cancel()
            for task in done:
                task.result()
        finally:
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            await _stop_process(process)


async def main() -> None:
    endpoint = os.getenv("MCP_ENDPOINT", "").strip()
    if not endpoint:
        raise RuntimeError("MCP_ENDPOINT is not configured in vision-mcp/.env")

    backoff = INITIAL_BACKOFF_SECONDS
    while True:
        try:
            await _run_connected(endpoint)
            backoff = INITIAL_BACKOFF_SECONDS
        except asyncio.CancelledError:
            raise
        except Exception as error:
            LOGGER.warning(
                "Connection stopped: %s: %r", type(error).__name__, error
            )
            LOGGER.info("Reconnecting in %d seconds", backoff)
            await asyncio.sleep(backoff)
            backoff = min(backoff * 2, MAX_BACKOFF_SECONDS)


if __name__ == "__main__":
    if sys.platform == "win32":
        # Proactor TLS handshakes can be reset by some Windows TUN adapters.
        # Pipe I/O uses regular Popen + worker threads, so Selector is safe here.
        asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        LOGGER.info("Stopped by user")
