# 药品与用药工具

本目录在原有 `look_at_camera` / `watch_camera` 之外，新增 7 个 MCP 工具，把“识别—建档—记录—查询—提醒—讲解”做成电脑端的本地能力。所有数据都存在本机文件里，不依赖机器人固件改动，也不需要数据库服务。

## 一、三条硬边界

这三条决定了测试怎么判、演示怎么讲，不能混用：

1. **识别不写记录。** `identify_medicine` 只读摄像头，看到药盒不会产生任何服药记录。
2. **只有 `log_medicine_taken` 会写服药记录。** 用户明确说“吃过了”才调用；识别成功、提醒到点都不算。
3. **提醒不会主动说话。** MCP 工具是被动调用的，`set_medicine_reminder` 只存计划；到点提醒要靠对话调用 `check_medicine_reminders` 拉取。

## 二、工具清单

| 工具 | 参数 | 作用 |
|---|---|---|
| `identify_medicine` | `hint` 可选 | 拍药盒，返回药名与规格，并与本地药品库匹配 |
| `remember_medicine` | `name`、`spec`、`note`、`usage`、`dose`、`frequency`、`cautions` | 建/更新药品档案；后四个字段保存用户口述的用药方案 |
| `log_medicine_taken` | `name`、`dose`、`taken_at` | 写一条服药记录，窗口内重复提交返回 `duplicate:true` |
| `query_medicine_records` | `date`、`name`、`limit` | 读回服药记录，供播报与网页展示 |
| `set_medicine_reminder` | `name`、`at`、`repeat_daily` | 存提醒计划（`at` 为 `HH:MM`） |
| `check_medicine_reminders` | `now` 可选 | 返回到点/已过时/即将到点三类提醒 |
| `get_medicine_info` | `name`、`spec` | 取用法、剂量、注意事项（药品库优先，其次用户口述） |

所有工具都返回带 `success` 的 JSON；失败时 `error` 是可直接对用户复述的中文。多数工具还附带 `instruction` 字段，用于约束模型该怎么说、什么时候不该编造。

### 药品不在药品库时怎么办

**“服药流水”不等于“用药方案”。** `log_medicine_taken` 记的是“某时刻吃了一次”，不会推导出“一天吃几次”。所以库外药品想要能回答用量，必须在建档时把用户口述的方案一起记下来：

```
用户：记住感冒灵，一天吃三次
→ remember_medicine(name="感冒灵颗粒", frequency="一日3次", dose="1袋")

用户：这是什么药，一天吃几次
→ get_medicine_info("感冒灵颗粒")
   { "found": true, "source": "用户口述",
     "medicine": { "name": "感冒灵颗粒", "dose": "1袋", "frequency": "一日3次" } }
```

`get_medicine_info` 的查询顺序固定为：

1. **本地药品库**（`source: "本地药品库"`）——人工维护，可当事实讲；
2. **档案中用户口述的内容**（`source: "用户口述"`）——必须说“你之前告诉我是……”，不能讲成说明书；
3. **都没有**（`found: false`）——如实说资料不足，建议看说明书或问医生药师。

库内药品永远优先，用户口述不会覆盖可靠资料。口述字段记录在档案的 `manual_fields` 里，便于区分。

### 识别提示词与 30 字限制

原 `look_at_camera` 使用“请在30字内回答”的通用提示词。`identify_medicine` 改用 `medicine_config.json` 里的 `identify_prompt`（默认“只回答药盒上的药品名称和规格，最多15个字”），因此**两个工具互不影响**。固件侧 `VISION_MESSAGE_MAX` 为 400 字节、`VISION_MAX_TOKENS` 为 80，单次识别仍是短文本，讲解放不进去——这也是讲解与问答要单独走 `get_medicine_info` 的原因。

## 三、配置文件

首次使用把示例复制成正式配置：

```powershell
Copy-Item medicine_config.example.json medicine_config.json
```

| 字段 | 默认 | 说明 |
|---|---|---|
| `enabled` | `true` | `false` 时所有写操作直接报错，便于临时关闭演示 |
| `data_dir` | `runtime/medicine` | 档案/日志/提醒的存放目录，相对 `mcp_server/` |
| `library_file` | `medicine_library.json` | 本地药品库 |
| `timezone_offset_minutes` | `480` | 北京时间；换时区改这里 |
| `identify_prompt` | 见上 | 识别专用提示词 |
| `identify_max_chars` | `15` | 未匹配到库时截断药名长度 |
| `duplicate_window_seconds` | `90` | 同药同剂量在该窗口内重复提交算重复 |
| `default_dose` | `1` | 用户没说剂量时的默认值 |
| `query_limit` / `query_limit_max` | `20` / `100` | 查询默认条数与上限 |
| `reminder_grace_minutes` | `120` | 超过该分钟数仍未服药，归入“已过时” |

配置文件改动后**无需重启桥接**（按 mtime 自动重载）。注意：`runtime/` 已在 `.gitignore` 中，记录不会被提交。

## 四、数据文件

| 文件 | 形式 | 内容 |
|---|---|---|
| `runtime/medicine/medicines.json` | 整体重写 + `os.replace` 原子替换 | 药品档案与提醒计划 |
| `runtime/medicine/medication_logs.jsonl` | 追加写 | 每次服药一行，坏行会被跳过并告警 |

换电脑的做法见 [MIGRATION.md](MIGRATION.md)。

## 五、本地测试

在工程根目录（`CameraWebServer/`）执行：

```powershell
$env:PYTHONPATH = (Get-Location).Path
& .\mcp_server\.venv\Scripts\python.exe -B -m unittest discover -s mcp_server/tests -v
```

> 若系统 `TMP` 指向不可写位置（部分受限环境如此），先把 `$env:TMP` / `$env:TEMP` 指到一个可写目录，否则 `tempfile.TemporaryDirectory` 会在 `tearDown` 阶段报 `PermissionError`，与代码逻辑无关。测试自身使用仓库内 `.test-tmp/`，可直接删除。

只跑药品相关用例：

```powershell
& .\mcp_server\.venv\Scripts\python.exe -B -m unittest mcp_server.tests.test_medicine_store mcp_server.tests.test_medicine_tools -v
```

## 六、已知限制

- 记录存在运行桥接的电脑上，换电脑需按迁移文档搬运 `runtime/medicine/`。
- 提醒不能主动播报，只能在对话中查询。
- 药品库是人工维护的白名单；库外药品若用户没有口述用法，只能回答药名，不提供用法和注意事项。不会从服药记录反推“一天几次”。
- 识别耗时取决于网络与模型，实测为 20–30 秒量级，演示需留出等待。
