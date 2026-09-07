"""Bridge a local stdio MCP server to a Xiaozhi MCP WebSocket endpoint."""

from __future__ import annotations

import asyncio
import logging
import os
from pathlib import Path
import sys

from dotenv import load_dotenv
import websockets


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


async def _websocket_to_process(websocket, process: asyncio.subprocess.Process) -> None:
    assert process.stdin is not None
    async for message in websocket:
        if isinstance(message, bytes):
            message = message.decode("utf-8")
        process.stdin.write((message + "\n").encode("utf-8"))
        await process.stdin.drain()
    raise RuntimeError("Xiaozhi MCP WebSocket closed")


async def _process_to_websocket(process: asyncio.subprocess.Process, websocket) -> None:
    assert process.stdout is not None
    while True:
        line = await process.stdout.readline()
        if not line:
            raise RuntimeError("camera MCP process stopped")
        await websocket.send(line.decode("utf-8").rstrip("\r\n"))


async def _log_process_stderr(process: asyncio.subprocess.Process) -> None:
    assert process.stderr is not None
    while True:
        line = await process.stderr.readline()
        if not line:
            return
        LOGGER.info("[camera-mcp] %s", line.decode("utf-8", errors="replace").rstrip())


async def _stop_process(process: asyncio.subprocess.Process) -> None:
    if process.returncode is not None:
        return
    process.terminate()
    try:
        await asyncio.wait_for(process.wait(), timeout=5)
    except asyncio.TimeoutError:
        process.kill()
        await process.wait()


async def _run_connected(endpoint: str) -> None:
    LOGGER.info("Connecting to the Xiaozhi MCP endpoint")
    async with websockets.connect(
        endpoint,
        ping_interval=20,
        ping_timeout=20,
        max_size=2 * 1024 * 1024,
    ) as websocket:
        LOGGER.info("Connected to the Xiaozhi MCP endpoint")
        process = await asyncio.create_subprocess_exec(
            sys.executable,
            str(BASE_DIR / "camera_mcp.py"),
            stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
            cwd=str(BASE_DIR),
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
            LOGGER.warning("Connection stopped: %s", error)
            LOGGER.info("Reconnecting in %d seconds", backoff)
            await asyncio.sleep(backoff)
            backoff = min(backoff * 2, MAX_BACKOFF_SECONDS)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        LOGGER.info("Stopped by user")

