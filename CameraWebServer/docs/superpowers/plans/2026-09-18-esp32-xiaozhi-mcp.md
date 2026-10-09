# ESP32-CAM Xiaozhi MCP Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a local Xiaozhi MCP bridge that exposes the ESP32-CAM's existing vision recognition as `look_at_camera` and `watch_camera` tools.

**Architecture:** A Python stdio MCP server calls the ESP32 HTTP vision endpoints. A Python WebSocket bridge forwards that stdio protocol to the configured Xiaozhi MCP endpoint. The ESP32 remains responsible for capture and DeepSeek API calls.

**Tech Stack:** Python 3.9+, FastMCP, requests, websockets, python-dotenv, ESP32 Arduino firmware.

---

### Task 1: Add MCP configuration and HTTP client helpers

**Files:**
- Create: `mcp_server/.env.example`
- Create: `mcp_server/requirements.txt`
- Create: `mcp_server/esp32_client.py`
- Create: `mcp_server/tests/test_esp32_client.py`

- [ ] Write tests for successful JSON parsing, HTTP failure conversion, and status/image metadata extraction.
- [ ] Run `python -m unittest discover -s mcp_server/tests -v` and confirm the new tests fail because the client module is absent.
- [ ] Implement a small `Esp32VisionClient` with bounded timeouts, `trigger_wait()`, `status()`, and `download_image()` methods.
- [ ] Run the tests again and confirm they pass.

### Task 2: Implement stdio MCP tools

**Files:**
- Create: `mcp_server/camera_mcp.py`
- Create: `mcp_server/tests/test_camera_mcp.py`

- [ ] Write tests for `look_at_camera` success/error results and `watch_camera` duration validation and sampling aggregation.
- [ ] Run the tests and confirm they fail before implementation.
- [ ] Implement both FastMCP tools using `Esp32VisionClient`; cap returned descriptions at 800 characters and write diagnostics to stderr.
- [ ] Run the tests and confirm they pass.

### Task 3: Add Xiaozhi WebSocket bridge and startup documentation

**Files:**
- Create: `mcp_server/xiaozhi_mcp_bridge.py`
- Create: `mcp_server/mcp_config.json`
- Create: `mcp_server/start_mcp.ps1`
- Create: `mcp_server/README.md`

- [ ] Port the reference bridge's reconnecting WebSocket/stdin forwarding behavior, preserving UTF-8 and stderr diagnostics.
- [ ] Add a PowerShell startup script that checks dependencies and launches the bridge.
- [ ] Document `.env` creation, ESP32 URL, MCP endpoint, and example Xiaozhi prompts.
- [ ] Run `python -m py_compile` on all Python files and inspect the generated tool configuration.

### Task 4: Limit ESP32 prompt to 30 Chinese characters and build

**Files:**
- Modify: `vision_config.h`

- [ ] Change `VISION_DEFAULT_PROMPT` to request no more than 30 Chinese characters.
- [ ] Compile the Arduino sketch with `build_flash.ps1 -CompileOnly`.
- [ ] Verify the compiled firmware artifact exists and report the final MCP and firmware validation results.
