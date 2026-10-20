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

ESP32 联网后主动向电脑的 `POST /camera/register` 上报自身 IP，电脑校验固定 key 后保存地址。摄像头 IP 变化后无需修改电脑配置；电脑 IP 变化时，通过 ESP32 串口修改桥接 IP 即可。

## 目录说明

| 路径 | 作用 |
| --- | --- |
| `CameraWebServer.ino` | ESP32 启动、摄像头、Wi-Fi 和 HTTP 服务 |
| `vision.cpp` / `vision.h` | 拍照、补光、上传、识别、自动识别后台任务 |
| `vision_config.h` | 模型、API、分辨率、识别间隔和超时配置 |
| `app_httpd.cpp` | 网页、视觉接口和 MCP 状态接口 |
| `mcp_status.cpp` / `mcp_status.h` | MCP 状态、日志和抓拍事件缓存 |
| `device_config.h` | 网页和串口共享网络配置、校验、存储与重启接口 |
| `mcp_server/` | 电脑端 MCP 服务和小智 WebSocket 桥接 |
| `mcp_server/medicine_*.py` | 药品识别、建档、服药记录、提醒与资料查询工具 |
| `build_flash.ps1` | 编译、烧录和可选串口监视 |
| `mcp_registration.cpp` / `mcp_registration.h` | 带 key 的 IP 上报后台任务 |
| `mcp_registration_config.h` | 本机私有默认桥接 IP、端口和固定 key |
| `temporary/` | 编译产物、缓存和历史临时记录，详见目录内 README |

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

ESP32 需要连接 2.4 GHz Wi-Fi。开机先尝试 NVS 已保存的 Wi-Fi（首次启动则尝试 `CameraWebServer.ino` 中的默认值）；约 20 秒仍未连接时会开启 `ESP32-CAM-7` 临时热点。Wi-Fi 配置保存到 NVS，掉电保留，无需为修改 Wi-Fi 重新编译。

电脑运行 MCP 桥接时，电脑和 ESP32 必须能互相访问。先确认设备 IP，再在电脑执行：

```powershell
ping <ESP32-IP>
curl.exe --noproxy "*" http://<ESP32-IP>/mcp/status
```

## 串口设置 Wi-Fi

串口波特率为 **115200**。每条命令以换行结束，支持 LF、CR 和 CRLF：

| 命令 | 作用 |
| --- | --- |
| `ssid=MyWiFi` | 设置并保存 Wi-Fi 名称（1–32 字节） |
| `pwd=MyPassword123` | 设置并保存密码（最多 64 字节）；`pwd=` 为空密码，用于开放网络 |
| `ssid?` | 查询配置中的 Wi-Fi 名称 |
| `pwd?` | 查询配置中的密码，**明文显示** |
| `mcp_ip=192.168.1.100` | 保存 MCP 桥接电脑的 IPv4 地址，重启应用 |
| `mcp_ip?` | 查询配置中的桥接 IP |
| `wifi_config` | 手动开启 ESP32-CAM-7 开放配置热点，访问 `http://192.168.4.1/config` |
| `reboot` | 重启设备，使用已保存的配置连接 |
| `help` | 查看串口命令帮助，原有识别和诊断命令保留 |

设备初始化完成后，串口会在启动日志末尾一次性输出完整命令列表；初始化失败时也会输出同一列表，便于直接修改 Wi-Fi 或桥接地址。发送 `help` 可再次显示，启动阶段不会重复输出。

若当前设备已连接 Wi-Fi，发送 `wifi_config` 也可以主动进入开放热点配置模式；保存并重启后，STA 成功连接会自动关闭热点。

正常联网初始化完成时，仍输出 `ESP32-CAM网页服务地址: http://<设备IP>/vision`，以及 `MCP上位机服务IP: <桥接电脑IP>:8765`，然后显示完整串口指令。

示例（逐行发送，不要给值加引号）：

```text
ssid=MyWiFi
pwd=MyPassword123
ssid?
pwd?
reboot
```

设置成功返回 `CMD: ssid saved ...` 或 `CMD: pwd saved ...`；查询返回 `WIFI: ssid=...` 或 `WIFI: pwd=...`。命令关键字不区分大小写，值保留大小写、空格和 `=`，因此不要在值后附加多余空格。长度按 UTF-8 字节计算。

配置设置后立即保存，但**不会立即重连**；两项设置完毕后发送 `reboot`，或手动复位/重新上电。查询显示的是待使用的配置，不一定是当前连接的网络。Wi-Fi 连接失败后仍可通过串口修改配置（启动连接最多等待 20 秒）。

密码会存储到设备并在 `pwd?` 中明文返回，串口日志不要公开。普通复位/断电不会清除配置；**整片擦除 Flash 会清除配置**。本项目烧录脚本默认整片擦除，再次烧录如需保留配置，请加 `-NoErase`，且不要更改 NVS 分区位置。

## 首次 Wi-Fi 配置热点

首次启动或保存的 Wi-Fi 连不上时，ESP32-CAM 在等待约 20 秒后会开启**无密码热点 `ESP32-CAM-7`**。用手机或电脑连接它，然后打开 `http://192.168.4.1/config`。在页面填写 Wi-Fi 名称、密码和 MCP 上位机 IP，保存后点击“重启设备”。

密码留空表示保留原密码；切换到开放 Wi-Fi 时勾选“清空密码”。配置与串口设置写入同一份 NVS，掉电保留。连接上 Wi-Fi 后会自动关闭开放热点，并向电脑上位机上报摄像头 IP。

电脑上位机只负责接收摄像头的 IP 注册和转发 MCP 请求，不提供设备配置页面。电脑 IP 变化时可在已联网设备网页配置，也可继续通过串口命令 `mcp_ip=<电脑IPv4>` 修改。

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
if (-not (Test-Path .env)) { Copy-Item .env.example .env }
notepad .env
.\start_mcp.ps1
```

启动成功后，日志中应看到 `Connected to the Xiaozhi MCP endpoint`。电脑桥接进程必须保持运行；停止服务时在该窗口按 `Ctrl+C`。

## 药品与用药功能

电脑端在原有 `look_at_camera` / `watch_camera` 之外新增 7 个 MCP 工具，把“识别—建档—记录—查询—提醒—讲解”做成纯电脑端的本地能力。**不需要修改固件，也不需要重新烧录。**

启用方式：把示例配置复制为正式配置即可，代码会自动加载。

```powershell
cd .\mcp_server
Copy-Item medicine_config.example.json medicine_config.json
```

工具一览：

| 工具 | 作用 |
| --- | --- |
| `identify_medicine` | 拍药盒，返回药名与规格，并与本地药品库匹配 |
| `remember_medicine` | 建/更新药品档案，可同时记住用户口述的用法、剂量、频次 |
| `log_medicine_taken` | 写一条服药记录，时间窗内重复提交自动去重 |
| `query_medicine_records` | 读回服药记录，供语音播报 |
| `set_medicine_reminder` | 保存提醒计划 |
| `check_medicine_reminders` | 查询到点、已过时和即将到点的提醒 |
| `get_medicine_info` | 取用法、剂量、注意事项（药品库优先，其次用户口述） |

三条必须遵守的语义边界：

1. **识别不等于服药。** `identify_medicine` 只读摄像头，看到药盒不会产生服药记录。
2. **只有 `log_medicine_taken` 写服药记录。** 用户明确说“吃过了”才调用。
3. **提醒不会主动说话。** MCP 工具是被动调用的，到点提醒要靠对话中查询 `check_medicine_reminders`。

药品库是人工维护的白名单（`mcp_server/medicine_library.json`）。库外药品若想让机器人答出用量，必须在建档时把用户口述的方案一起记下：

```text
用户：记住感冒灵，一天吃三次
→ remember_medicine(name="感冒灵颗粒", frequency="一日3次", dose="1袋")

用户：这个一天吃几次？
→ get_medicine_info("感冒灵颗粒")
   { "found": true, "source": "用户口述", "medicine": { "frequency": "一日3次", ... } }
```

查询优先级固定为**本地药品库 → 用户口述 → 明确说没有**。库内资料永远优先，不会被用户口述覆盖；来源是“用户口述”时必须说明这是用户告诉的，不能讲成说明书内容。

数据存放在 `mcp_server/runtime/medicine/`（已被 `.gitignore` 忽略），换电脑的搬运步骤见 [mcp_server/MIGRATION.md](./mcp_server/MIGRATION.md)。完整说明见 [mcp_server/MEDICINE.md](./mcp_server/MEDICINE.md)。

## 动态地址注册配置

本机已配对 `mcp_server/.env` 中的 `CAMERA_REGISTRATION_KEY` 和 `mcp_registration_config.h` 中的 `MCP_REGISTRATION_KEY`。桥接默认监听 `0.0.0.0:8765`，当前固件默认桥接 IP 为 `192.168.240.247`；该默认值只在设备未保存地址时使用。

在 115200 波特率的串口监视器中逐行发送：

```text
mcp_ip=192.168.240.247
mcp_ip?
reboot
```

电脑更换网络后，将第一行替换为电脑在 ESP32 所在局域网中的新 IPv4 地址。配置保存在 NVS，掉电保留；查询显示待应用的地址，重启后上报任务使用新地址。烧录时使用 `-NoErase` 可保留 Wi-Fi 和桥接 IP。

迁移到另一台电脑时，将 `mcp_registration_config.h.example` 复制为 `mcp_registration_config.h`，并填写与服务端一致的固定 key 和端口。IP 可随后通过串口设置，无需重新编译。服务端 `ESP32_BASE_URL` 留空即可完全使用自动注册，也可填写地址兼容旧固件。

设备联网和 IP 变化时立即上报，失败后每 5 秒重试，成功后每 60 秒重报。HTTP 请求由独立任务执行，栈大小为 4096 字节，连接和响应超时各 1.5 秒；桥接重启后最多等待一轮重报即可自动恢复。电脑需允许 ESP32 访问 TCP 8765，双方应位于可互访的局域网。

## 安全注意事项

- `VISION_API_KEY` 会被编译进固件，不能把包含真实密钥的源码公开或提交到公共仓库；密钥泄露后应立即撤销并重新生成。
- `mcp_server/.env` 包含小智 MCP endpoint 和 token，不能公开分享。
- `mcp_server/runtime/` 下的药品档案与服药记录属于个人健康数据，已被 `.gitignore` 忽略，不要提交。
- 当前 ESP32 HTTP 接口没有身份认证，只应在可信局域网使用，不要直接映射到公网。
- 如需远程访问，应使用 VPN 或受控反向隧道，并在网关层增加认证。

## 常见故障

### 网页打不开或 MCP 显示未连接

确认电脑和 ESP32 在同一局域网，并检查：

```powershell
ping <ESP32-IP>
curl.exe --noproxy "*" http://<ESP32-IP>/mcp/status
```

摄像头 IP 变化后会自动上报并更新桥接地址。若电脑 IP 变化，通过串口发送 `mcp_ip=新的电脑IP`，再发送 `reboot`。若一直等待注册，检查两端 key、桥接监听端口及电脑防火墙入站设置。

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
& .\mcp_server\.venv\Scripts\python.exe -B -m unittest discover -s mcp_server/tests -v
& .\mcp_server\.venv\Scripts\python.exe -B tests/run_serial_wifi_tests.py
& .\mcp_server\.venv\Scripts\python.exe -B tests/run_registration_tests.py
& .\mcp_server\.venv\Scripts\python.exe -B tests/run_device_http_tests.py
node tests/web_network_config_tests.cjs
```

只跑药品相关用例：

```powershell
& .\mcp_server\.venv\Scripts\python.exe -B -m unittest mcp_server.tests.test_medicine_store mcp_server.tests.test_medicine_tools -v
```

> 若系统 `TMP` 指向不可写目录，先设置 `$env:TMP` / `$env:TEMP` 到可写路径，否则 `tempfile.TemporaryDirectory` 会在清理阶段报 `PermissionError`，这与代码逻辑无关。药品测试自身使用仓库内 `.test-tmp/`，可直接删除。

固件只编译验证：

```powershell
powershell -ExecutionPolicy Bypass -File .\build_flash.ps1 -Port COM9 -CompileOnly
```
