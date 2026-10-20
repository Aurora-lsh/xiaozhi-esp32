"""Expose fresh ESP32 recognition results through stdio MCP.

Tools intentionally use the firmware's fixed prompt. The caller's question is
returned as context for Xiaozhi, not silently represented as a vision-model input.
"""

import logging
import os
from pathlib import Path
import threading
import time

from dotenv import load_dotenv

try:
    from .esp32_client import Esp32VisionClient
    from .camera_registration import resolve_camera_url, start_registration_from_env
    from .medicine_tools import medicine_tools
except ImportError:
    from esp32_client import Esp32VisionClient
    from camera_registration import resolve_camera_url, start_registration_from_env
    from medicine_tools import medicine_tools

BASE_DIR = Path(__file__).resolve().parent
load_dotenv(BASE_DIR / ".env", encoding="utf-8-sig")
LOGGER = logging.getLogger("ESP32Camera")
_CLIENT = Esp32VisionClient(
    resolve_camera_url(),
    float(os.getenv("ESP32_TIMEOUT_SECONDS", "100")),
    # 手机热点会出现数秒级瞬时黑洞，连接超时放宽到 8 秒（旧值写死 3 秒）。
    float(os.getenv("ESP32_CONNECT_TIMEOUT_SECONDS", "8")),
    base_url_provider=resolve_camera_url,
)
_LOCK = threading.Lock()
_NOTE = "视觉模型使用每次请求的问题，并要求最终回答不超过30字。"
_INTERVAL = float(os.getenv("WATCH_SAMPLE_INTERVAL_SECONDS", "1"))
if not 0.1 <= _INTERVAL <= 10:
    raise ValueError("WATCH_SAMPLE_INTERVAL_SECONDS must be between 0.1 and 10")


def _capture(question):
    prompt = "请在30字内回答：" + question.strip()
    # source=mcp makes the firmware pair this capture with its reply and show
    # both on the web page under "机器人抓拍与识别回复".
    result = _CLIENT.trigger_wait(prompt=prompt, source="mcp")
    return {
        "success": True,
        "description": result["message"].strip()[:800],
        "duration_ms": result.get("durationMs", 0),
    }


def _validate_question(question):
    if not isinstance(question, str) or not question.strip() or len(question) > 1000:
        raise ValueError("question 必须是 1 到 1000 字的非空字符串")


def look_at_camera(question: str) -> dict:
    """查看 ESP32 摄像头前的物体或环境，拍照并返回简短中文描述。

    question 为小智需要回答的问题。本工具提供固定提示词生成的画面描述，
    不保证包含问题所需细节。黑暗图片也会上传，不开闪光灯。
    """
    try:
        _validate_question(question)
    except ValueError as error:
        return {"success": False, "error": str(error)}
    if not _LOCK.acquire(blocking=False):
        return {"success": False, "error": "摄像头正在处理另一项 MCP 调用，请稍后重试"}
    start = time.monotonic()
    try:
        result = _capture(question)
        result.update(question=question, prompt_note=_NOTE)
        # The firmware already stores the JPEG and shows it on the vision page.
        address = resolve_camera_url()
        result["web_url"] = address + "/vision" if address else ""
        LOGGER.info("look_at_camera completed in %.2fs", time.monotonic() - start)
        return result
    except Exception as error:
        LOGGER.warning("look_at_camera failed: %s", error)
        return {"success": False, "error": str(error)[:400]}
    finally:
        _LOCK.release()


def watch_camera(question: str, duration_seconds: int = 3) -> dict:
    """在 1 到 10 秒窗口内串行拍照识别，返回带时间的画面描述（默认 3 秒）。

    这是稀疏快照观察，不是视频理解，不能可靠判断快速动作。
    单次识别可能超出观察窗口，总耗时包含最后一次识别等待；不足两次时明确标记。
    question 供小智结合各次描述回答，视觉模型仍使用固定提示词。
    """
    try:
        _validate_question(question)
        if type(duration_seconds) is not int or not 1 <= duration_seconds <= 10:
            raise ValueError("duration_seconds 必须为 1 到 10 的整数")
    except ValueError as error:
        return {"success": False, "error": str(error)}
    if not _LOCK.acquire(blocking=False):
        return {"success": False, "error": "摄像头正在处理另一项 MCP 调用，请稍后重试"}
    start = time.monotonic()
    samples = []
    failure = None
    try:
        # Ten results bounds the response even when the device responds instantly.
        while len(samples) < 10:
            offset = time.monotonic() - start
            if samples and offset >= duration_seconds:
                break
            try:
                sample = _capture(question)
            except Exception as error:
                failure = str(error)[:400]
                break
            sample["requested_at_seconds"] = round(offset, 3)
            sample["completed_at_seconds"] = round(time.monotonic() - start, 3)
            samples.append(sample)
            wait = max(0.0, start + offset + _INTERVAL - time.monotonic())
            remaining = start + duration_seconds - time.monotonic()
            if remaining <= 0:
                break
            time.sleep(min(wait, remaining))
        result = {
            "success": bool(samples) and failure is None,
            "partial": bool(samples) and failure is not None,
            "question": question,
            "mode": "serial_snapshots",
            "requested_duration_seconds": duration_seconds,
            "elapsed_seconds": round(time.monotonic() - start, 3),
            "sample_count": len(samples),
            "samples": samples,
            "last_result": samples[-1]["description"] if samples else "",
            "enough_samples_for_comparison": len(samples) >= 2,
            "prompt_note": _NOTE,
        }
        if len(samples) < 2:
            result["observation_note"] = "采样不足两次，无法判断变化或动作。"
        if failure:
            result["error"] = failure
        LOGGER.info("watch_camera samples=%d elapsed=%.2fs success=%s",
                    len(samples), result["elapsed_seconds"], result["success"])
        return result
    finally:
        _LOCK.release()


def create_server():
    from fastmcp import FastMCP
    server = FastMCP("ESP32VisionCamera")
    # Explicit registration keeps the underlying functions directly testable.
    server.tool()(look_at_camera)
    server.tool()(watch_camera)
    for tool in medicine_tools():
        server.tool()(tool)
    return server


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO)
    # show_banner=False：FastMCP 的 ASCII 横幅会走 stderr，被桥接逐行转发到
    # 固件日志环形缓冲，把真正有用的日志挤掉，所以在固件场景下关闭它。
    registration = None
    try:
        if os.getenv("CAMERA_REGISTRATION_MANAGED_BY_BRIDGE") != "1":
            registration = start_registration_from_env()
        create_server().run(transport="stdio", show_banner=False)
    finally:
        if registration is not None:
            registration.close()
