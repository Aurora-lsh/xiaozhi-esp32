# ESP32-CAM Vision Recognition Implementation Summary

## 实现日期
2026-09-14

## 功能概述
在 CameraWebServer 基础上添加了大模型视觉识别功能，支持手动和自动模式。

## 新增文件

### 1. vision_config.h
- API 配置：端点、API Key、模型选择
- 默认间隔时间：10秒
- 默认提示词配置
- **警告**：API Key 已暴露在聊天记录中，建议部署后立即撤销并重新生成

### 2. vision.h
- 视觉模块的公共接口
- VisionState 结构体定义
- 函数声明：初始化、手动触发、配置自动模式、获取状态

### 3. vision.cpp
- 核心实现文件
- 功能模块：
  - 摄像头抓取 JPEG 图片
  - Base64 编码
  - 构造 OpenAI 兼容的 JSON 请求
  - HTTPS 请求发送（使用 WiFiClientSecure）
  - JSON 响应解析（简单字符串解析，未使用 ArduinoJson）
  - FreeRTOS 后台任务实现自动识别
  - 互斥锁防止手动/自动并发
  - 错误处理和状态管理

## 修改的文件

### 1. app_httpd.cpp
- 添加 `#include "vision.h"`
- 增加 `max_uri_handlers` 从 16 到 20
- 新增 4 个 HTTP handlers：
  - `/vision` - 视觉识别控制页面（完整的 HTML/CSS/JavaScript）
  - `/vision/status` - 获取状态和最新识别结果（JSON API）
  - `/vision/trigger` - 手动触发识别（JSON API）
  - `/vision/config` - 配置自动模式和间隔（JSON API）

### 2. CameraWebServer.ino
- 添加 `#include "vision.h"`
- 修复 WiFi 连接失败消息的转义字符（`\\n` -> `\n`）
- 在 setup() 末尾添加 `visionInit()` 调用
- 添加 Vision Recognition 就绪提示信息

## API 配置

### 端点
- URL: `https://api.kakouai.com/v1/chat/completions`
- 协议: HTTPS (TLS 验证已禁用，使用 `client.setInsecure()`)

### 可用模型
- gpt-5.6-sol
- gpt-5.6-terra
- gpt-5.6-luna (默认)

### 安全说明
- 当前使用 `client.setInsecure()` 跳过证书验证
- 生产环境应安装 CA 证书并启用验证
- API Key 编译到固件中，无运行时加密

## Web 界面功能

### 手动控制
- "📸 Capture & Recognize" 按钮
- 处理中时按钮禁用
- 实时状态反馈

### 自动模式
- 开关控制
- 间隔时间设置（0-3600秒；0 表示识别完成后立即进入下一轮）
- 后台定时执行

### 状态显示
- 当前状态：Idle / Processing
- 请求计数
- 错误计数

### 结果显示
- 聊天消息框显示识别结果
- 包含时间戳
- 新消息自动添加到顶部
- 2秒刷新频率

## 技术实现细节

### 内存管理
- FreeRTOS 任务栈：8192 字节
- 最大响应大小：4096 字节
- 最后消息缓冲：512 字节
- Base64 编码使用 String 类（可能需要大量堆内存）

### 并发控制
- 使用 FreeRTOS 互斥锁
- 防止手动和自动请求重叠
- 非阻塞式触发（返回立即，处理在后台）

### 错误处理
- 摄像头捕获失败
- Base64 编码失败
- HTTPS 连接失败
- 非 2xx HTTP 响应
- JSON 解析失败
- 所有错误记录到串口和 lastMessage

### JSON 解析
- 使用简单字符串搜索，未依赖 ArduinoJson
- 解析 "content" 字段
- 处理基本转义字符：`\n`, `\"`, `\\`
- 生产环境建议使用 ArduinoJson 库

## 依赖库
必需的 Arduino 库：
- ESP32 Camera (内置)
- WiFiClientSecure (内置)
- HTTPClient (内置)
- base64 (需要安装，或使用内置的 mbedtls base64)

## 分区表
当前分区表已验证无重叠：
```
nvs      : 0x9000  -> 0xe000   (20KB)
otadata  : 0xe000  -> 0x10000  (8KB)
app0     : 0x10000 -> 0x3d0000 (3.75MB)
fr       : 0x3d0000 -> 0x3f0000 (128KB)
coredump : 0x3f0000 -> 0x400000 (64KB)
```

## 上传建议
首次上传新分区表时，建议在 Arduino IDE 中设置：
- 工具 → Erase All Flash Before Sketch Upload → Enabled

## 编译验证状态
- ❌ 未能在本地编译验证（arduino-cli 不可用）
- ✅ 语法结构检查通过
- ✅ 括号匹配检查通过
- ✅ 头文件引用检查通过
- ⚠️ 需要在实际硬件上测试

## 未验证的功能
由于缺少硬件和编译环境，以下功能未经验证：
1. 实际编译是否成功
2. Base64 库的可用性和正确性
3. HTTPS 请求是否能正常发送
4. API 响应格式是否与解析代码匹配
5. 内存使用是否在 ESP32 限制内
6. FreeRTOS 任务是否稳定运行
7. Web 界面在实际设备上的表现

## 潜在问题和改进建议

### 1. Base64 编码
- 当前使用 `base64::encode()` 可能需要额外安装库
- 建议使用 ESP32 内置的 mbedtls base64 函数
- QVGA JPEG 图片 base64 后可能超过 20KB

### 2. 内存使用
- String 类的大量使用可能导致堆碎片
- 建议使用固定缓冲区或 ArduinoJson 的 StaticJsonDocument

### 3. JSON 构造
- 当前使用字符串拼接，效率较低
- 建议使用 ArduinoJson 序列化

### 4. 错误恢复
- 自动模式在错误后继续运行，可能需要添加退避策略
- 建议添加连续错误阈值，超过后暂停自动模式

### 5. 证书验证
- 生产环境应该添加 CA 证书
- 可以使用 `client.setCACert()` 设置证书

## 使用方法

### 1. 上传固件
```
1. 打开 Arduino IDE
2. 选择开发板：ESP32 Wrover Module 或 AI Thinker ESP32-CAM
3. 选择分区方案：Huge APP (3MB No OTA/1MB SPIFFS) 或使用自定义 partitions.csv
4. 首次上传：工具 → Erase All Flash Before Sketch Upload → Enabled
5. 编译并上传
```

### 2. 连接 WiFi
- 设备会自动连接到 `realmeGTNeo` 网络
- 20秒超时后如果未连接会打印错误

### 3. 访问接口
- 主页：`http://<ESP32-IP>/`
- 视觉识别页面：`http://<ESP32-IP>/vision`

### 4. 串口监控
- 波特率：115200
- 查看识别结果和调试信息

## 安全注意事项
1. ⚠️ API Key 已在聊天中暴露，需要撤销并重新生成
2. ⚠️ TLS 证书验证已禁用，中间人攻击风险
3. ⚠️ WiFi 密码明文存储在代码中
4. ⚠️ 无身份验证，任何人都可以访问 /vision 接口

## 下一步工作
1. 在实际硬件上测试编译
2. 验证 API 调用是否正常
3. 测试内存使用情况
4. 优化 Base64 编码实现
5. 添加 ArduinoJson 支持
6. 实现更好的错误恢复机制
7. 添加基本的 Web 身份验证
8. 轮换 API Key

## 总结
所有计划的功能已经实现并集成到项目中。代码结构清晰，模块化良好，但由于缺少编译环境和硬件，无法验证实际运行情况。建议在实际设备上进行完整测试，并根据测试结果进行必要的调整。
