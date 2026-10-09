# ESP32-CAM 视觉识别与小智 MCP

这是一个基于 AI Thinker ESP32-CAM 的视觉识别项目。ESP32 负责摄像头取图、补光、图片上传和大模型识别；网页负责设备控制和状态查看；电脑上的 `mcp_server` 负责把小智云端的 MCP 请求转发到 ESP32。

## 工作架构

```text
小智服务端
    │ WebSocket MCP（电脑主动连接）
    ▼
电脑：mcp_server/xiaozhi_mcp_bridge.py
    │ 本地 stdio MCP
    ▼
电脑：mcp_server/camera_mcp.py
    │ HTTP
    ▼
ESP32-CAM：/vision/*、/mcp/*
    │ HTTPS
    ▼
视觉模型服务
```

电脑和 ESP32 在同一个局域网，只能解决“电脑访问 ESP32”的问题。小智服务端通常位于公网，不能直接访问 ESP32 的局域网地址，所以电脑桥接服务需要保持运行。

一次 MCP 调用的方向是：

```text
小智 → WebSocket → 电脑桥接 → HTTP → ESP32
ESP32 → HTTP 响应 → 电脑桥接 → WebSocket → 小智
```

ESP32 不直接实现 MCP 协议，而是提供普通 HTTP 接口。电脑桥接同时负责 WebSocket 长连接、stdio MCP 转发和局域网 HTTP 请求。

## 目录说明

| 路径 | 作用 |
| --- | --- |
| `CameraWebServer.ino` | ESP32 启动、摄像头、Wi-Fi 和 HTTP 服务 |
| `vision.cpp` / `vision.h` | 拍照、补光、上传、识别、自动识别后台任务 |
| `vision_config.h` | 模型、API、分辨率、识别间隔和超时配置 |
| `app_httpd.cpp` | 网页、视觉接口和 MCP 状态接口 |
| `mcp_status.cpp` / `mcp_status.h` | MCP 状态、日志和抓拍事件缓存 |
| `mcp_server/` | 电脑端 MCP 服务和小智 WebSocket 桥接 |
| `build_flash.ps1` | 编译、烧录和可选串口监视 |
| `build_out/` | 编译生成的固件产物 |

## 硬件与网络

当前工程按 AI Thinker ESP32-CAM 配置，使用 ESP32 Arduino Core 2.0.14 和板卡标识 `esp32:esp32:esp32cam`。

烧录接线：

```text
USB-TTL 5V  → ESP32-CAM 5V
USB-TTL GND → ESP32-CAM GND
USB-TTL TX  → ESP32-CAM U0R/RX
USB-TTL RX  → ESP32-CAM U0T/TX
IO0         → GND（仅烧录时）
```

ESP32 需要连接 2.4 GHz Wi-Fi。Wi-Fi 名称和密码当前位于 `CameraWebServer.ino` 的 `ssid` 和 `password` 变量中，修改后必须重新编译烧录。

电脑运行 MCP 桥接时，电脑和 ESP32 必须能互相访问。先确认设备 IP，再在电脑执行：

```powershell
ping <ESP32-IP>
curl.exe --noproxy "*" http://<ESP32-IP>/mcp/status
```

## 固件配置

主要配置在 [vision_config.h](./vision_config.h)：

| 配置 | 当前含义 |
| --- | --- |
| `VISION_API_ENDPOINT` | 视觉模型 API 地址 |
| `VISION_API_KEY` | 编译进固件的 API Key，不能提交或公开 |
| `VISION_MODEL` | 使用的视觉模型 |
| `VISION_DEFAULT_INTERVAL_MS` | 自动识别默认间隔，当前为 10 秒 |
| `VISION_MIN_INTERVAL_MS` | 自动识别最小间隔，当前为 0 |
| `VISION_MAX_INTERVAL_MS` | 自动识别最大间隔，当前为 3600 秒 |
| `VISION_FRAMESIZE_DEFAULT` | 抓拍默认分辨率，当前为 QVGA 320x240 |
| `VISION_TORCH_DUTY_DEFAULT` | 暗光自动补光基准强度 |
| `VISION_HTTP_TIMEOUT_MS` | ESP32 上传和读取模型响应的超时 |

自动识别间隔设为 `0` 时，含义是“本次识别完成后立即进入下一轮”。识别过程仍然串行执行，不会并发启动多个识别；实际频率还受拍照、上传、模型响应和后台任务调度时间影响。

自动补光会在抓拍前先关闭 LED 测量环境亮度，暗光时打开 GPIO4 补光，抓拍后立即关闭。网页上的“抓拍补光”值为基准强度，设为 0 可完全关闭自动补光。

## 固件网页

烧录并启动后，串口会打印设备 IP。打开：

```text
http://<ESP32-IP>/
```

网页功能包括：

- 实时预览和预览补光；
- 手动拍照并识别；
- 自动识别开关和 0 到 3600 秒间隔；
- 抓拍补光强度、分辨率和 JPEG 画质；
- 最近识别结果、耗时、环境亮度和图片；
- 小智 MCP 连接状态、日志和抓拍事件。

常用 HTTP 接口：

| 接口 | 作用 |
| --- | --- |
| `GET /vision/status` | 获取识别状态、间隔、最近结果和图片信息 |
| `GET /vision/trigger` | 投递一次手动识别 |
| `GET /vision/trigger_wait?prompt=...&source=mcp` | 投递并短暂等待识别结果 |
| `GET /vision/config?enabled=1&interval=0` | 开启自动识别并设置 0 秒间隔 |
| `GET /vision/image` | 获取最近一次识别图片 |
| `GET /vision/preview` | 获取网页预览图片 |
| `GET /mcp/status` | 获取电脑桥接连接状态 |
| `GET /mcp/log` | 获取桥接日志 |
| `GET /mcp/events` | 获取小智触发的抓拍事件 |
| `GET /mcp/event_image?seq=N` | 获取指定抓拍事件的图片 |

## 编译与烧录

确认 USB-TTL 使用的串口后，在工程根目录执行：

```powershell
powershell -ExecutionPolicy Bypass -File .\build_flash.ps1 -Port COM9
```

只编译不烧录：

```powershell
powershell -ExecutionPolicy Bypass -File .\build_flash.ps1 -Port COM9 -CompileOnly
```

跳过编译、直接烧录已有产物：

```powershell
powershell -ExecutionPolicy Bypass -File .\build_flash.ps1 -Port COM9 -SkipCompile
```

烧录完成后断开 IO0-GND、按一下 RST，再打开设备网页。烧录期间不要同时打开串口监视器或其他占用 COM 口的程序。

## 启动小智 MCP 桥接

详细说明见 [mcp_server/README.md](./mcp_server/README.md)。最短流程：

```powershell
cd .\mcp_server
Copy-Item .env.example .env
notepad .env
.\start_mcp.ps1
```

启动成功后，日志中应看到 `Connected to the Xiaozhi MCP endpoint`。电脑桥接进程必须保持运行；停止服务时在该窗口按 `Ctrl+C`。

## 安全注意事项

- `VISION_API_KEY` 会被编译进固件，不能把包含真实密钥的源码公开或提交到公共仓库；密钥泄露后应立即撤销并重新生成。
- `mcp_server/.env` 包含小智 MCP endpoint 和 token，不能公开分享。
- 当前 ESP32 HTTP 接口没有身份认证，只应在可信局域网使用，不要直接映射到公网。
- 如需远程访问，应使用 VPN 或受控反向隧道，并在网关层增加认证。

## 常见故障

### 网页打不开或 MCP 显示未连接

确认电脑和 ESP32 在同一局域网，并检查：

```powershell
ping <ESP32-IP>
curl.exe --noproxy "*" http://<ESP32-IP>/mcp/status
```

如果电脑切换了热点，ESP32 的 IP 可能已经变化，需要同步修改 `mcp_server/.env` 的 `ESP32_BASE_URL`。

### 日志出现“ESP32 请求失败或超时”

先检查设备 IP 和 Wi-Fi 链路。电脑端默认使用 8 秒连接超时，并且只对“请求尚未发出”的连接失败重试一次；已经发出的识别请求不会重试，避免重复拍照。

手机热点出现瞬时断流时，优先使用普通 2.4 GHz 路由器，或将热点加密改为 WPA2-PSK (AES)。

### 识别失败或耗时很长

检查 ESP32 串口日志中的上传重试、HTTP 状态码、图片大小和模型响应。当前图片默认是 QVGA 320x240，识别速度仍取决于 Wi-Fi、TLS 和模型服务响应时间。

### 烧录失败

确认 IO0 已接 GND、串口号正确、没有其他程序占用串口，并按脚本提示使用 5V 供电。烧录完成后必须断开 IO0-GND 并复位。

## 开发验证

电脑端 MCP 测试：

```powershell
$env:PYTHONPATH = (Get-Location).Path
& .\mcp_server\.venv\Scripts\python.exe -B -m unittest mcp_server.tests.test_esp32_client mcp_server.tests.test_camera_mcp mcp_server.tests.test_firmware_interval_config -v
```

固件只编译验证：

```powershell
powershell -ExecutionPolicy Bypass -File .\build_flash.ps1 -Port COM9 -CompileOnly
```
