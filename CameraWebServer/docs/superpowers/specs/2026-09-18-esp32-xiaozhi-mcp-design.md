# ESP32-CAM Xiaozhi MCP 设计

## 目标

让小智通过参考工程使用的 WebSocket MCP 接入点调用当前 ESP32-CAM 的拍照识别能力，同时保留现有固件负责 DeepSeek 图片识别的链路。提供 `look_at_camera` 单次识别和 `watch_camera` 连续观察两个工具。

## 架构

电脑端运行 `xiaozhi_mcp_bridge.py`，从 `.env` 读取 `MCP_ENDPOINT`，把 WebSocket JSON-RPC 转发给本地 stdio MCP 子进程。`camera_mcp.py` 使用 HTTP 调用 ESP32 的 `/vision/trigger_wait`、`/vision/status` 和 `/vision/image` 接口，不访问电脑本地摄像头，也不携带 DeepSeek 密钥。

`look_at_camera` 触发一次拍照识别并返回短文本。`watch_camera` 在 1 到 10 秒窗口内按采样间隔重复调用单次接口，返回采样结果列表和最后一次识别结果；它表达连续观察语义，但不伪装成 ESP32 原生视频理解。

## 配置

`.env.example` 保留参考工程的 `MCP_ENDPOINT` 配置方式，新增 `ESP32_BASE_URL`、请求超时、观察采样间隔和图片保存目录配置。DeepSeek endpoint、模型和 key 继续由固件的 `vision_config.h` 管理。

## 错误处理与验证

MCP 工具返回结构化的 `success`、`description`、`samples` 或 `error` 字段；诊断写 stderr，避免污染 stdio JSON-RPC。测试覆盖响应解析、单次调用、观察时长限制和异常返回。验证包括 Python 语法、MCP 工具枚举、ESP32 编译和真实 `/vision/trigger_wait` 调用。
