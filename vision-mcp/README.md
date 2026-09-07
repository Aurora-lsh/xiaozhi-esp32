# Xiaozhi Vision MCP

这个目录提供一个运行在电脑、树莓派或其他 Linux/Windows 主机上的视觉 MCP 服务。
它调用连接到主机的摄像头拍照，把 JPEG 交给视觉服务分析，再把简短的文字结果通过
小智 MCP 接入点返回给大模型。

```text
小智大模型 <-> MCP_ENDPOINT <-> xiaozhi_mcp_bridge.py
                                  <-> camera_mcp.py
                                  <-> 摄像头 + 视觉服务
```

## 适用范围

- 本目录读取的是运行 Python 程序这台主机的摄像头，不是 ESP32 上的摄像头。
- 如果摄像头直接连接 ESP32，请使用固件已有的 `self.camera.take_photo` 和
  `main/boards/common/esp32_camera.cc`。
- MCP 只负责工具发现和调用；实际图像理解由 `VISION_URL` 指向的视觉服务完成。

## 安装

建议使用 Python 3.10 或更高版本。在本目录执行：

```powershell
python -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install -r requirements.txt
Copy-Item .env.example .env
```

然后编辑 `.env`。`MCP_ENDPOINT` 来自 xiaozhi.me 对应智能体的 MCP 接入点。
不要把真实接入点或令牌提交到 Git；`.env` 已加入 `.gitignore`。

## 小米 MiMo 配置（默认）

本项目默认使用小米 MiMo 多模态 API。登录小米 MiMo 开放平台创建 API Key，
然后在 `.env` 中填写：

- [MiMo 图片理解官方教程](https://mimo.mi.com/docs/zh-CN/quick-start/usage-guide/multimodal-understanding/image-understanding)
- [MiMo 视频理解官方教程](https://mimo.mi.com/docs/zh-CN/quick-start/usage-guide/multimodal-understanding/video-understanding)

```dotenv
VISION_API_STYLE=mimo
MIMO_API_KEY=your-api-key
MIMO_BASE_URL=https://api.xiaomimimo.com/v1
MIMO_MODEL=mimo-v2.5
MIMO_MAX_COMPLETION_TOKENS=512
MIMO_VIDEO_ANALYSIS_FPS=2
MIMO_MEDIA_RESOLUTION=default
```

`look_at_camera` 把 JPEG 转成 Base64 `image_url`；`watch_camera` 录制 1～10 秒 MP4，
转成 Base64 `video_url`，默认让 MiMo 每秒分析 2 帧。两个工具都只向小智返回简短
文字，不会把 Base64 图片或视频塞进 MCP 返回结果。

每次调用 `look_at_camera` 拍到的 JPEG 都会先保存到 `vision-mcp/data/`，文件名包含
摄像头索引和本地时间，例如 `camera_0_20260908_153000_123456.jpg`。即使视觉接口
调用失败，已经拍摄的照片也会保留。`data/` 中的图片已被 Git 忽略；程序不会自动
删除历史照片，请按需手动清理。

## 其他视觉接口配置

### multipart

默认模式与 ESP32 固件的视觉上传方式接近：

```dotenv
VISION_API_STYLE=multipart
VISION_URL=https://your-service.example/analyze
VISION_TOKEN=optional-token
```

工具会发送 `multipart/form-data`：

- `question`：用户针对画面提出的问题。
- `file`：文件名为 `camera.jpg` 的 JPEG 图片。
- 如果配置了令牌，会发送 `Authorization: Bearer <token>`。

视觉服务可以返回纯文本，也可以返回包含 `description`、`answer`、`result`、`text`
或 `content` 字段的 JSON。

### OpenAI-compatible

如果视觉服务兼容 Chat Completions 的图片输入格式：

```dotenv
VISION_API_STYLE=openai_compatible
VISION_URL=https://your-service.example/v1/chat/completions
VISION_TOKEN=your-token
VISION_MODEL=your-vision-model
```

## 运行

先确认系统中的默认摄像头索引；通常内置或第一个 USB 摄像头是 `CAMERA_INDEX=0`。
然后运行：

```powershell
python xiaozhi_mcp_bridge.py
```

出现以下两类日志说明桥接已建立且本地 MCP Server 已启动：

```text
Connected to the Xiaozhi MCP endpoint
[camera-mcp] ... Processing request of type ListToolsRequest
```

可以对小智说：

- “看一下摄像头前面有什么。”
- “帮我识别桌上的元件。”
- “看看门口有没有人。”
- “观察三秒钟，看看这个物体是怎么移动的。”

静态观察使用 `look_at_camera`；动作或时序观察使用 `watch_camera`。

## 与官方 mcp-calculator 的关系

`camera_mcp.py` 对应示例中的 `calculator.py`，`xiaozhi_mcp_bridge.py` 对应
`mcp_pipe.py`。目录中的 `mcp_config.json` 也可以交给官方 `mcp_pipe.py` 使用。

由于小智 MCP 返回内容通常较短，本工具不会通过 MCP 返回原始图片或 Base64，
而是返回最多 800 个字符的视觉分析结果。图片只发送到你配置的 `VISION_URL`，不会
保存在本地文件中。
