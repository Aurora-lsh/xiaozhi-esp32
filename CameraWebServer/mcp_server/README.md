# ESP32-CAM 小智 MCP 桥接

本目录运行电脑端 MCP 服务，把小智云端的 WebSocket MCP 请求转换成 ESP32-CAM 的 HTTP 请求。

电脑端不负责拍照、图片上传或调用视觉模型。真正的流程是：

```text
小智服务端
    │ WebSocket MCP
    ▼
xiaozhi_mcp_bridge.py
    │ 本地 stdio
    ▼
camera_mcp.py
    │ HTTP
    ▼
ESP32-CAM
    │ HTTPS
    ▼
视觉模型服务
```

小智服务端通常在公网，不能直接访问 ESP32 的局域网 IP。电脑桥接主动连接小智服务端，再通过局域网 HTTP 访问 ESP32，因此桥接进程必须保持运行。

## 文件说明

| 文件 | 作用 |
| --- | --- |
| `xiaozhi_mcp_bridge.py` | 维护到小智 endpoint 的 WebSocket，并转发本地 MCP stdio |
| `camera_mcp.py` | 提供 `look_at_camera` 和 `watch_camera` 两个 MCP 工具 |
| `medicine_tools.py` | 药品识别、建档、服药记录、提醒、资料查询共 7 个 MCP 工具 |
| `medicine_store.py` | 药品功能的文件存储、幂等与配置加载 |
| `medicine_config.json` | 药品功能配置（已入库，克隆后即可用；不含密钥） |
| `medicine_config.example.json` | 出厂默认配置，用于覆盖恢复 |
| `medicine_library.json` | 本地药品库（用法、剂量、注意事项） |
| `MEDICINE.md` / `MIGRATION.md` | 药品工具说明 / 迁移到其他电脑的步骤 |
| `esp32_client.py` | ESP32 HTTP 客户端、状态轮询、请求超时和连接重试 |
| `reporting.py` | 把桥接状态和日志回传到 ESP32 网页，失败不影响 MCP 调用 |
| `camera_registration.py` | 校验 key、接收摄像头 IP，原子保存并供父子进程共享 |
| `runtime/camera_address.json` | 最近认证通过的摄像头地址，服务重启后继续使用 |
| `runtime/medicine/` | 药品档案与服药日志（本地记录，迁移时按需带走） |
| `start_mcp.ps1` | 创建虚拟环境、安装依赖并启动桥接 |
| `.env.example` | 配置模板 |
| `.env` | 本机真实配置，不要公开或提交 |
| `mcp_config.json` | 直接以本地 stdio 方式启动 `camera_mcp.py` 的示例配置 |
| `tests/` | 客户端、工具和配置回归测试 |

药品功能的完整说明见 [MEDICINE.md](MEDICINE.md)，换电脑运行见 [MIGRATION.md](MIGRATION.md)。

## 环境要求

- Windows PowerShell；
- Python 3.10 或更高版本，推荐 Python 3.11；
- `uv`（推荐）或可用的 Python 3.10+；
- 电脑和 ESP32 在同一个可互访的局域网；
- 可访问小智 MCP WebSocket endpoint 的网络。

当前启动脚本会优先使用 `uv` 创建 `.venv` 并安装 `requirements.txt`。如果使用本机指定的 Python 3.11，可先设置：

```powershell
$env:PYTHON_EXE = 'D:\Python\cpython-3.11.13-windows-x86_64-none\python.exe'
```

如果 `uv` 不在 PATH，也可以把 `uv.exe` 加入 PATH，或让脚本使用 Python 自带的 `venv` 和 `pip` 回退流程。

## 配置

在本目录创建 `.env`：

```powershell
if (-not (Test-Path .env)) { Copy-Item .env.example .env }
notepad .env
```

配置示例：

```dotenv
MCP_ENDPOINT=wss://api.xiaozhi.me/mcp/?token=...
CAMERA_REGISTRATION_HOST=0.0.0.0
CAMERA_REGISTRATION_PORT=8765
CAMERA_REGISTRATION_KEY=replace-with-the-same-random-key-as-firmware
ESP32_BASE_URL=
ESP32_TIMEOUT_SECONDS=100
ESP32_CONNECT_TIMEOUT_SECONDS=8
WATCH_SAMPLE_INTERVAL_SECONDS=1
```

配置项说明：

| 变量 | 默认值 | 说明 |
| --- | ---: | --- |
| `MCP_ENDPOINT` | 无 | 从小智平台获取的完整 WebSocket MCP endpoint，包含 token |
| `CAMERA_REGISTRATION_HOST` | `0.0.0.0` | 注册接口监听地址，允许局域网设备访问 |
| `CAMERA_REGISTRATION_PORT` | `8765` | 注册接口 TCP 端口，应与固件 `MCP_BRIDGE_PORT` 一致 |
| `CAMERA_REGISTRATION_KEY` | 空 | 固定认证 key，与固件完全一致；为空时关闭注册接口 |
| `CAMERA_STATE_FILE` | `mcp_server/runtime/camera_address.json` | 可选共享地址文件，相对路径以 mcp_server 为基准 |
| `ESP32_BASE_URL` | 空 | 可选旧固件兼容地址；已认证的注册地址优先 |
| `ESP32_TIMEOUT_SECONDS` | `100` | TCP 连接建立后，ESP32 HTTP 读响应及等待本轮识别完成的最长时间 |
| `ESP32_CONNECT_TIMEOUT_SECONDS` | `8` | 建立 TCP 连接的最长等待时间，允许 1 到 60 秒 |
| `WATCH_SAMPLE_INTERVAL_SECONDS` | `1` | `watch_camera` 连续快照之间的间隔，允许 0.1 到 10 秒 |

两个 ESP32 超时的区别：

- 连接超时解决“电脑暂时连不上设备”的问题；
- 识别超时解决“已经连上设备，但识别或模型响应很慢”的问题。

## 摄像头地址自动注册

桥接服务不提供设备配置页面，仅接收 ESP32-CAM 的 IP 注册并代理 MCP 摄像头请求。首次 Wi-Fi 配置由 ESP32-CAM 自带的 `ESP32-CAM-7` 无密码热点完成；连接热点后打开 `http://192.168.4.1/config`。

在固件私有文件 `mcp_registration_config.h` 中配置相同的 `MCP_REGISTRATION_KEY`，以及注册端口 `MCP_BRIDGE_PORT`。本机这两处已填入同一个随机 key；已有配置无需再次复制模板。

桥接电脑地址通过 ESP32 串口配置，115200 波特率，每行发送一条：

```text
mcp_ip=192.168.240.247
mcp_ip?
reboot
```

将示例 IP 替换为电脑在设备所在网络中的真实地址。设置写入 NVS，重启应用，掉电保留。电脑 IP 改变后重复这三行，无需重编固件。固定 key 和注册端口由编译配置提供。

固件向 `http://<电脑IP>:8765/camera/register` 发送 POST，请求头 `X-Camera-Key` 携带 key，请求体为 `{"ip":"192.168.1.20"}`。返回 200 表示已保存地址；403 表示 key 不一致；400 表示数据无效；503 表示暂时无法保存或服务繁忙。失败后 5 秒重试，成功后 60 秒重报，Wi-Fi 重连或设备 IP 变化会立即再次上报。

监听服务在小智 WebSocket 重连期间继续运行；MCP 请求和桥接状态回传都在每次请求前读取最新地址，无需重启进程。桥接重启后保留最新地址；修改 key 会使旧记录失效。未收到上报且没有兼容地址时，MCP 工具返回等待注册的说明。

电脑需允许局域网设备访问 TCP 8765。注册服务最多处理 4 个并发连接，每个请求有 3 秒总时限。仅启动一个桥接实例；直接启动 `camera_mcp.py` 时也会启动同样的注册监听，不能与桥接同时占用此端口。

连接阶段只自动重试一次，而且只重试确定尚未发出的请求。读超时、响应中途断开和已经发出的 `/vision/trigger_wait` 请求不会重试，避免一次语音调用触发多次拍照。

## 启动

从本目录执行：

```powershell
.\start_mcp.ps1
```

也可以从工程根目录执行：

```powershell
.\mcp_server\start_mcp.ps1
```

启动脚本会：

1. 检查 `.env`；
2. 使用 `uv` 或 Python 创建 `.venv`；
3. 安装或同步 `requirements.txt`；
4. 启动 `xiaozhi_mcp_bridge.py`；
5. 在桥接连接成功后启动本地 `camera_mcp.py` 子进程。

看到以下日志表示 WebSocket 已连接：

```text
Connected to the Xiaozhi MCP endpoint
```

停止服务：在运行窗口按 `Ctrl+C`。不要同时启动两个桥接进程，否则可能出现重复连接、状态显示相互覆盖或 MCP 请求分流。

## MCP 工具

### `look_at_camera(question)`

拍摄一张照片并等待 ESP32 返回识别结果。

示例问题：

```text
画面里有什么？
桌上有没有杯子？
现在环境是明亮还是昏暗？
```

问题必须是 1 到 1000 个字符的非空字符串。固件使用带有“30 字以内回答”约束的提示词，返回结果通常是简短中文描述。

### `watch_camera(question, duration_seconds=3)`

在 1 到 10 秒窗口内串行执行多次拍照识别，用于观察连续变化。它不是视频流，也不能可靠判断非常快速的动作。

每次采样都要等待 ESP32 完成一次真实拍照和模型识别，因此：

- 单次识别耗时超过观察窗口时，可能只有 1 次采样；
- `sample_count < 2` 时，返回结果会标记无法比较变化；
- `WATCH_SAMPLE_INTERVAL_SECONDS` 只控制电脑端连续调用间隔，不是 ESP32 网页上的自动识别间隔；
- ESP32 网页自动识别间隔由固件控制，允许 0 到 3600 秒。

## ESP32 网页状态

桥接服务每 5 秒把连接状态和脱敏后的 endpoint 上报给 ESP32 的 `/mcp/report`。子进程日志也会转发到设备网页。

打开：

```text
http://<ESP32-IP>/
```

网页的“小智 MCP 状态”区域可以看到：

- 当前 WebSocket 是否已连接；
- 最近连接、断开和重连日志；
- 桥接上报次数；
- 小智触发的抓拍图片和对应识别回复。

状态接口：

| 接口 | 说明 |
| --- | --- |
| `GET /mcp/status` | 当前连接状态、endpoint、上报次数和缓存数量 |
| `GET /mcp/log` | 固件缓存的最近日志，最多 24 条 |
| `GET /mcp/events` | 最近抓拍事件，最多 5 条 |
| `GET /mcp/event_image?seq=N` | 获取指定事件的 JPEG |
| `GET /vision/status` | 视觉识别状态和最近一次结果 |

桥接状态上报是 best-effort：ESP32 暂时不可达时只记录 debug 信息，不会改变 MCP 请求本身的结果。

## 网络检查

先确认串口出现 `MCP: registered camera IP ...`，并查看 `runtime/camera_address.json` 中的摄像头 IP；旧固件兼容模式下则使用 `ESP32_BASE_URL`：

```powershell
ping 192.168.137.135
curl.exe --noproxy "*" http://192.168.137.135/mcp/status
curl.exe --noproxy "*" http://192.168.137.135/vision/status
```

如果设备重连热点，IP 可能变化。应以 ESP32 串口打印的 `Camera Ready! Use 'http://...'` 地址为准，并更新 `.env`。

建议电脑和 ESP32 使用普通 2.4 GHz 路由器。手机热点的省电调度、WPA3-H2E 和客户端隔离可能造成数秒级丢包或黑洞。若可以选择，WPA2-PSK (AES) 通常比 WPA3 更适合 ESP32-CAM。

## 常见故障

### `start_mcp.ps1` 报 Python 或依赖错误

确认 Python 版本：

```powershell
python --version
uv --version
```

需要 Python 3.10 或更高版本。已有 Python 3.11 时，设置 `PYTHON_EXE` 后重新运行启动脚本。

### 日志显示 `ESP32 请求失败或超时`

按顺序检查：

1. ESP32 是否已通电并连接当前热点；
2. 串口 `mcp_ip?` 是否是电脑当前地址，两端 key 是否一致；
3. `ping` 和 `/mcp/status` 是否可达；
4. Windows 防火墙或热点是否启用了客户端隔离；
5. 是否有浏览器预览、识别请求或其他程序占满设备连接。

该错误只表示当前 HTTP 请求没有拿到有效响应，不代表识别请求可以安全重发。客户端因此不会重复触发已发出的拍照请求。

### MCP 显示未连接

“MCP 未连接”通常表示电脑到小智 WebSocket endpoint 的连接断开，不一定表示 ESP32 离线。分别检查电脑到小智 endpoint 和电脑到 ESP32 两条链路。网页上的“小智 MCP 状态”会显示桥接最近一次上报的连接状态和日志。

### 识别很慢或失败

识别链路包含摄像头抓拍、JPEG、TLS 建连、图片上传、模型排队和响应解析。当前默认抓拍分辨率为 QVGA 320x240，可降低图片体积，但不能消除模型服务或 Wi-Fi 的延迟。查看 ESP32 串口日志中的 `Vision:` 行可以区分抓拍失败、上传失败和模型响应失败。

### 识别过程中网页卡顿

固件的 `/vision/trigger_wait` 只短暂等待，识别未完成时由电脑客户端轮询 `/vision/status`。浏览器预览和 MCP 调用仍会竞争 ESP32 的有限 HTTP socket 与摄像头资源；弱网环境下建议关闭实时预览，并避免同时启动多个桥接实例。

## 本地测试

在工程根目录执行：

```powershell
$env:PYTHONPATH = (Get-Location).Path
& .\mcp_server\.venv\Scripts\python.exe -B -m unittest discover -s mcp_server/tests -v
& .\mcp_server\.venv\Scripts\python.exe -B tests/run_serial_wifi_tests.py
& .\mcp_server\.venv\Scripts\python.exe -B tests/run_registration_tests.py
& .\mcp_server\.venv\Scripts\python.exe -B tests/run_device_http_tests.py
node tests/web_network_config_tests.cjs
```

直接检查 Python 语法：

```powershell
& .\mcp_server\.venv\Scripts\python.exe -B -c "import ast,pathlib; [ast.parse(p.read_text(encoding='utf-8-sig')) for p in pathlib.Path('mcp_server').glob('*.py')]"
```

固件编译验证应在工程根目录执行：

```powershell
powershell -ExecutionPolicy Bypass -File .\build_flash.ps1 -Port COM9 -CompileOnly
```

## 安全注意事项

- `.env` 中的 `MCP_ENDPOINT` 包含访问 token，不能公开；
- ESP32 的 `VISION_API_KEY` 位于固件源码并会被编译进固件，泄露后应立即撤销；
- ESP32 HTTP 接口当前没有身份认证，不要把设备端口直接暴露到公网；
- MCP 输出使用 stdout，诊断日志使用 stderr，不要把控制台日志混入 stdio JSON-RPC 通道。
