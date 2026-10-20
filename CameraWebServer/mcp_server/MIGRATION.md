# 药品功能迁移到另一台电脑

整套药品功能只依赖三个东西：本目录的 Python 代码、`medicine_config.json`、以及数据目录（默认 `runtime/medicine/`）。硬件（ESP32-CAM + 小智机器人）和云端模型不需要改动。

## 一、要带走的东西

| 内容 | 路径 | 说明 |
|---|---|---|
| 桥接与药品代码 | `mcp_server/`（除 `.venv/`、`__pycache__/`） | 代码本身不含本机路径 |
| 本机配置 | `mcp_server/.env` | **含小智 endpoint token，按密钥对待** |
| 药品配置 | `mcp_server/medicine_config.json` | 一般可直接复用 |
| 药品库 | `mcp_server/medicine_library.json` | 换成自己家常用药时改这里 |
| 历史记录 | `mcp_server/runtime/medicine/` | 药品档案、服药日志、提醒；不要就跳过 |

到达新电脑后，如果新电脑的目录位置不同，只需要改 `medicine_config.json` 里的相对路径（默认都是相对 `mcp_server/` 解析，通常不用改）。

## 二、在新电脑上的步骤

1. 确认 Python 3.10+，推荐 3.11。`.venv/` 不要拷贝，必须重建。
2. 在本目录执行 `.\start_mcp.ps1`，脚本会创建虚拟环境并安装 `requirements.txt`。
3. 把 ESP32-CAM 的注册地址指向新电脑：串口 115200 发送 `mcp_ip=<新电脑局域网IP>`，再发 `reboot`。电脑 IP 变了就必须重做这一步。
4. 启动后确认日志出现 `Connected to the Xiaozhi MCP endpoint`。
5. 在新电脑上验证数据目录可写：
   ```powershell
   .\mcp_server\.venv\Scripts\python.exe -B -c "from medicine_store import get_store; s=get_store(); print(s.data_dir, s.archive_path)"
   ```
   在 `mcp_server` 目录下执行，应打印 `runtime/medicine` 及其下的 `medicines.json`。

## 三、Windows 防火墙

新电脑首次运行需要允许局域网访问注册端口（默认 8765），否则 ESP32 无法上报 IP，工具会返回“等待 ESP32-CAM 上报 IP 地址”。

## 四、常见迁移问题

| 现象 | 原因 | 处理 |
|---|---|---|
| 工具返回“等待 ESP32-CAM 上报 IP 地址” | 注册未到达 | 检查防火墙、`mcp_ip` 是否为新电脑地址、两端 key 是否一致 |
| 记录为空但文件存在 | `data_dir` 指到了别处 | 用上面的命令打印实际 `data_dir` |
| 提示 `medicines.json 格式错误` | 拷贝过程中文件损坏 | 用备份替换；不要手工改坏 JSON |
| 报 `.env` 相关错误 | 未拷贝或未填 `MCP_ENDPOINT` | `Copy-Item .env.example .env` 后填入 endpoint |
| 端口 8765 被占用 | 同时开了两个桥接 | 只保留一个桥接进程 |

## 五、不要做的事

- 不要把 `.env`、`medicine_config.json` 里的 token 提交到公开仓库。
- 不要在两台电脑上同时运行桥接指向同一个 endpoint，会出现重复连接和请求分流。
- 不要手工编辑 `medication_logs.jsonl` 的中间行；追加写文件一旦改坏会让该行被跳过。
