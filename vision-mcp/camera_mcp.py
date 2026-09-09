"""Camera MCP server for a Xiaozhi MCP endpoint.

The tool captures one frame from a camera connected to this computer, sends the
JPEG to a configured vision service, and returns a short text description. MCP
uses stdout for JSON-RPC, so all diagnostics are written through logging.
"""

from __future__ import annotations

import base64
from datetime import datetime
import json
import logging
import os
from pathlib import Path
import sys
import tempfile
import threading
import time
from typing import Any

import cv2
from dotenv import load_dotenv
from fastmcp import FastMCP
from openai import OpenAI
import requests


load_dotenv(Path(__file__).with_name(".env"))

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s - %(name)s - %(levelname)s - %(message)s",
    stream=sys.stderr,
)
LOGGER = logging.getLogger("VisionCamera")
MCP = FastMCP("VisionCamera")

_CAMERA_LOCK = threading.Lock()
_MAX_DESCRIPTION_CHARS = 800
_DATA_DIR = Path(__file__).resolve().parent / "data"


def _env_int(name: str, default: int, minimum: int, maximum: int) -> int:
    raw_value = os.getenv(name, str(default))
    try:
        value = int(raw_value)
    except ValueError as error:
        raise RuntimeError(f"{name} must be an integer") from error
    if value < minimum or value > maximum:
        raise RuntimeError(f"{name} must be between {minimum} and {maximum}")
    return value


def _env_float(name: str, default: float, minimum: float, maximum: float) -> float:
    raw_value = os.getenv(name, str(default))
    try:
        value = float(raw_value)
    except ValueError as error:
        raise RuntimeError(f"{name} must be a number") from error
    if value < minimum or value > maximum:
        raise RuntimeError(f"{name} must be between {minimum} and {maximum}")
    return value


def _open_camera(camera_index: int) -> cv2.VideoCapture:
    if sys.platform == "win32":
        camera = cv2.VideoCapture(camera_index, cv2.CAP_DSHOW)
        if camera.isOpened():
            return camera
        camera.release()
    return cv2.VideoCapture(camera_index)


def _capture_jpeg() -> tuple[bytes, int]:
    camera_index = _env_int("CAMERA_INDEX", 0, 0, 32)
    width = _env_int("CAMERA_WIDTH", 640, 1, 7680)
    height = _env_int("CAMERA_HEIGHT", 480, 1, 4320)
    warmup_frames = _env_int("CAMERA_WARMUP_FRAMES", 3, 1, 60)
    jpeg_quality = _env_int("CAMERA_JPEG_QUALITY", 75, 10, 100)

    with _CAMERA_LOCK:
        camera = _open_camera(camera_index)
        try:
            if not camera.isOpened():
                raise RuntimeError(f"cannot open camera index {camera_index}")

            camera.set(cv2.CAP_PROP_FRAME_WIDTH, width)
            camera.set(cv2.CAP_PROP_FRAME_HEIGHT, height)

            ok = False
            frame = None
            for _ in range(warmup_frames):
                ok, frame = camera.read()
            if not ok or frame is None:
                raise RuntimeError("camera did not return a frame")

            encoded, jpeg = cv2.imencode(
                ".jpg",
                frame,
                [cv2.IMWRITE_JPEG_QUALITY, jpeg_quality],
            )
            if not encoded:
                raise RuntimeError("failed to encode the camera frame as JPEG")
            return jpeg.tobytes(), camera_index
        finally:
            camera.release()


def _save_captured_photo(jpeg: bytes, camera_index: int) -> Path:
    _DATA_DIR.mkdir(parents=True, exist_ok=True)
    timestamp = datetime.now().astimezone().strftime("%Y%m%d_%H%M%S_%f")
    photo_path = _DATA_DIR / f"camera_{camera_index}_{timestamp}.jpg"
    photo_path.write_bytes(jpeg)
    return photo_path


def _record_video(duration_seconds: int) -> tuple[bytes, int]:
    if duration_seconds < 1 or duration_seconds > 10:
        raise RuntimeError("duration_seconds must be between 1 and 10")

    camera_index = _env_int("CAMERA_INDEX", 0, 0, 32)
    width = _env_int("CAMERA_WIDTH", 640, 1, 7680)
    height = _env_int("CAMERA_HEIGHT", 480, 1, 4320)
    warmup_frames = _env_int("CAMERA_WARMUP_FRAMES", 3, 1, 60)
    record_fps = _env_int("CAMERA_RECORD_FPS", 10, 1, 60)

    temporary = tempfile.NamedTemporaryFile(suffix=".mp4", delete=False)
    video_path = Path(temporary.name)
    temporary.close()

    with _CAMERA_LOCK:
        camera = _open_camera(camera_index)
        writer = None
        frame_count = 0
        try:
            if not camera.isOpened():
                raise RuntimeError(f"cannot open camera index {camera_index}")

            camera.set(cv2.CAP_PROP_FRAME_WIDTH, width)
            camera.set(cv2.CAP_PROP_FRAME_HEIGHT, height)
            for _ in range(warmup_frames):
                camera.read()

            ok, first_frame = camera.read()
            if not ok or first_frame is None:
                raise RuntimeError("camera did not return a frame")

            frame_height, frame_width = first_frame.shape[:2]
            writer = cv2.VideoWriter(
                str(video_path),
                cv2.VideoWriter_fourcc(*"mp4v"),
                float(record_fps),
                (frame_width, frame_height),
            )
            if not writer.isOpened():
                raise RuntimeError("failed to create an MP4 video encoder")

            total_frames = duration_seconds * record_fps
            next_frame_time = time.monotonic()
            frame = first_frame
            for frame_number in range(total_frames):
                if frame_number > 0:
                    ok, frame = camera.read()
                    if not ok or frame is None:
                        raise RuntimeError("camera stopped returning frames")
                writer.write(frame)
                frame_count += 1
                next_frame_time += 1.0 / record_fps
                time.sleep(max(0.0, next_frame_time - time.monotonic()))
        finally:
            if writer is not None:
                writer.release()
            camera.release()

    try:
        if frame_count == 0 or not video_path.exists():
            raise RuntimeError("recorded video is empty")
        video = video_path.read_bytes()
        if not video:
            raise RuntimeError("recorded video is empty")
        if (len(video) * 4 + 2) // 3 > 50 * 1024 * 1024:
            raise RuntimeError("recorded video exceeds MiMo's 50 MB Base64 limit")
        return video, camera_index
    finally:
        video_path.unlink(missing_ok=True)


def _authorization_headers() -> dict[str, str]:
    token = os.getenv("VISION_TOKEN", "").strip()
    return {"Authorization": f"Bearer {token}"} if token else {}


def _extract_description(payload: Any) -> str:
    if isinstance(payload, str):
        return payload
    if isinstance(payload, dict):
        for key in ("description", "answer", "result", "text", "content"):
            value = payload.get(key)
            if isinstance(value, str):
                return value
    return json.dumps(payload, ensure_ascii=False, separators=(",", ":"))


def _analyze_multipart(jpeg: bytes, question: str) -> str:
    url = os.getenv("VISION_URL", "").strip()
    if not url:
        raise RuntimeError("VISION_URL is not configured")

    timeout = _env_int("VISION_TIMEOUT_SECONDS", 30, 1, 300)
    response = requests.post(
        url,
        headers=_authorization_headers(),
        data={"question": question},
        files={"file": ("camera.jpg", jpeg, "image/jpeg")},
        timeout=timeout,
    )
    response.raise_for_status()

    try:
        payload: Any = response.json()
    except requests.JSONDecodeError:
        payload = response.text
    return _extract_description(payload)


def _analyze_openai_compatible(jpeg: bytes, question: str) -> str:
    url = os.getenv("VISION_URL", "").strip()
    model = os.getenv("VISION_MODEL", "").strip()
    if not url:
        raise RuntimeError("VISION_URL is not configured")
    if not model:
        raise RuntimeError("VISION_MODEL is required for openai_compatible mode")

    timeout = _env_int("VISION_TIMEOUT_SECONDS", 30, 1, 300)
    encoded_image = base64.b64encode(jpeg).decode("ascii")
    response = requests.post(
        url,
        headers={
            **_authorization_headers(),
            "Content-Type": "application/json",
        },
        json={
            "model": model,
            "messages": [
                {
                    "role": "user",
                    "content": [
                        {"type": "text", "text": question},
                        {
                            "type": "image_url",
                            "image_url": {
                                "url": f"data:image/jpeg;base64,{encoded_image}"
                            },
                        },
                    ],
                }
            ],
            "max_tokens": 300,
        },
        timeout=timeout,
    )
    response.raise_for_status()
    payload = response.json()

    try:
        content = payload["choices"][0]["message"]["content"]
    except (KeyError, IndexError, TypeError) as error:
        raise RuntimeError("vision service returned an unexpected response") from error

    if isinstance(content, str):
        return content
    if isinstance(content, list):
        text_parts = [
            item.get("text", "")
            for item in content
            if isinstance(item, dict) and isinstance(item.get("text"), str)
        ]
        if text_parts:
            return "\n".join(text_parts)
    raise RuntimeError("vision response did not contain text")


def _mimo_client() -> OpenAI:
    api_key = os.getenv("MIMO_API_KEY", "").strip()
    if not api_key:
        raise RuntimeError("MIMO_API_KEY is not configured")
    base_url = os.getenv("MIMO_BASE_URL", "https://api.xiaomimimo.com/v1").strip()
    timeout = _env_int("VISION_TIMEOUT_SECONDS", 30, 1, 300)
    return OpenAI(api_key=api_key, base_url=base_url, timeout=timeout)


def _mimo_completion(media_content: dict[str, Any], question: str) -> str:
    model = os.getenv("MIMO_MODEL", "mimo-v2.5").strip()
    max_tokens = _env_int("MIMO_MAX_COMPLETION_TOKENS", 1024, 1, 4096)
    completion = _mimo_client().chat.completions.create(
        model=model,
        messages=[
            {
                "role": "system",
                "content": (
                    "You are a camera vision assistant. Answer the user's question "
                    "from the provided camera media. Respond in concise Chinese."
                ),
            },
            {
                "role": "user",
                "content": [
                    media_content,
                    {"type": "text", "text": question},
                ],
            },
        ],
        max_completion_tokens=max_tokens,
    )

    if not completion.choices:
        LOGGER.error(
            "MiMo returned no choices: request_id=%r model=%r usage=%r",
            completion.id,
            completion.model,
            completion.usage,
        )
        raise RuntimeError("MiMo response did not contain any choices")

    choice = completion.choices[0]
    content = choice.message.content
    reasoning_content = getattr(choice.message, "reasoning_content", None)
    LOGGER.info(
        "MiMo response: request_id=%r model=%r finish_reason=%r content=%r "
        "usage=%r",
        completion.id,
        completion.model,
        choice.finish_reason,
        content,
        completion.usage,
    )
    if reasoning_content:
        LOGGER.debug("MiMo reasoning_content=%r", reasoning_content)
    if not isinstance(content, str) or not content.strip():
        if reasoning_content:
            LOGGER.warning(
                "MiMo returned reasoning but no final text: reasoning_content=%r",
                reasoning_content,
            )
        raise RuntimeError(
            "MiMo response did not contain text "
            f"(finish_reason={choice.finish_reason!r})"
        )
    return content.strip()


def _analyze_mimo_image(jpeg: bytes, question: str) -> str:
    encoded_image = base64.b64encode(jpeg).decode("ascii")
    return _mimo_completion(
        {
            "type": "image_url",
            "image_url": {"url": f"data:image/jpeg;base64,{encoded_image}"},
        },
        question,
    )


def _analyze_mimo_video(video: bytes, question: str) -> str:
    encoded_video = base64.b64encode(video).decode("ascii")
    analysis_fps = _env_float("MIMO_VIDEO_ANALYSIS_FPS", 2.0, 0.1, 10.0)
    media_resolution = os.getenv("MIMO_MEDIA_RESOLUTION", "default").strip().lower()
    if media_resolution not in ("default", "max"):
        raise RuntimeError("MIMO_MEDIA_RESOLUTION must be 'default' or 'max'")
    return _mimo_completion(
        {
            "type": "video_url",
            "video_url": {"url": f"data:video/mp4;base64,{encoded_video}"},
            "fps": analysis_fps,
            "media_resolution": media_resolution,
        },
        question,
    )


def _analyze_image(jpeg: bytes, question: str) -> str:
    api_style = os.getenv("VISION_API_STYLE", "mimo").strip().lower()
    if api_style == "mimo":
        return _analyze_mimo_image(jpeg, question)
    if api_style == "multipart":
        return _analyze_multipart(jpeg, question)
    if api_style == "openai_compatible":
        return _analyze_openai_compatible(jpeg, question)
    raise RuntimeError(
        "VISION_API_STYLE must be 'mimo', 'multipart', or 'openai_compatible'"
    )


@MCP.tool()
def look_at_camera(question: str) -> dict[str, Any]:
    """当用户要求查看、观察或识别摄像头前的物体、人物或环境时调用此工具。

    参数 question 是用户希望根据当前画面回答的具体问题。工具会立即拍摄一张
    照片、调用视觉服务，并返回简短的中文文字描述。
    """

    saved_photo: Path | None = None
    try:
        jpeg, camera_index = _capture_jpeg()
        saved_photo = _save_captured_photo(jpeg, camera_index)
        LOGGER.info("Captured JPEG from camera %d (%d bytes)", camera_index, len(jpeg))
        LOGGER.info("Saved captured photo to %s", saved_photo)
        description = _analyze_image(jpeg, question).strip()
        if not description:
            raise RuntimeError("vision service returned an empty description")
        return {
            "success": True,
            "camera_index": camera_index,
            "saved_file": str(saved_photo),
            "description": description[:_MAX_DESCRIPTION_CHARS],
        }
    except Exception as error:  # The MCP caller needs a concise, actionable failure.
        LOGGER.exception("Camera vision request failed")
        result: dict[str, Any] = {"success": False, "error": str(error)[:400]}
        if saved_photo is not None:
            result["saved_file"] = str(saved_photo)
        return result


@MCP.tool()
def watch_camera(question: str, duration_seconds: int = 3) -> dict[str, Any]:
    """当用户需要观察动作、变化或一段连续场景时，录制摄像头视频并调用此工具。

    参数 question 是针对视频画面提出的问题；duration_seconds 是录制时长，允许
    1 到 10 秒，默认 3 秒。静态物体优先使用 look_at_camera，只有需要理解动作或
    时序变化时才使用本工具。
    """

    try:
        if os.getenv("VISION_API_STYLE", "mimo").strip().lower() != "mimo":
            raise RuntimeError("video understanding currently requires VISION_API_STYLE=mimo")
        video, camera_index = _record_video(duration_seconds)
        LOGGER.info(
            "Recorded %d-second video from camera %d (%d bytes)",
            duration_seconds,
            camera_index,
            len(video),
        )
        description = _analyze_mimo_video(video, question).strip()
        return {
            "success": True,
            "camera_index": camera_index,
            "duration_seconds": duration_seconds,
            "description": description[:_MAX_DESCRIPTION_CHARS],
        }
    except Exception as error:
        LOGGER.exception("Camera video vision request failed")
        return {"success": False, "error": str(error)[:400]}


if __name__ == "__main__":
    MCP.run(transport="stdio")
