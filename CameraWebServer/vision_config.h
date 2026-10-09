#ifndef VISION_CONFIG_H
#define VISION_CONFIG_H

// ---------------------------------------------------------------------------
// 大模型接口配置
// ---------------------------------------------------------------------------

// DeepSeek OpenAI 兼容的 Chat Completions 接口地址
#define VISION_API_ENDPOINT "https://api.deepseek.com/chat/completions"

// API Key
// 接口密钥。注意：此文件会编译进固件，请勿公开分享
#define VISION_API_KEY "sk-0b9b35575f044cfa86e2a41b031882ee"

// 可用模型：deepseek-v4-flash（快，支持图片）/ deepseek-v4-pro（更慢更稳）
#define VISION_MODEL "deepseek-v4-flash"

// ---------------------------------------------------------------------------
// 请求参数
// ---------------------------------------------------------------------------

// 默认提示词
#define VISION_DEFAULT_PROMPT "\u7528\u4e0d\u8d85\u8fc7\u0033\u0030\u4e2a\u5b57\u7b80\u8ff0\u56fe\u7247\u5185\u5bb9\u3002"

// 最大回复 token 数
#define VISION_MAX_TOKENS 80

// 自动识别间隔：默认值 / 最小值 / 最大值（毫秒）。最小值 0 表示识别完成后立即进入下一轮。
#define VISION_DEFAULT_INTERVAL_MS 10000UL
#define VISION_MIN_INTERVAL_MS 0UL
#define VISION_MAX_INTERVAL_MS 3600000UL

// 单条消息最大长度（字节，UTF-8）
#define VISION_MESSAGE_MAX 400

// 单次 HTTP 请求可选提示词的最大字节数（UTF-8）
#define VISION_PROMPT_MAX 256

// 单张图片最大字节数，超过则跳过本次识别
#define VISION_MAX_IMAGE_BYTES 160000UL

// 抓拍前丢弃的预热帧数：自动曝光 / 自动白平衡需要几帧才收敛
#define VISION_WARMUP_FRAMES 4

// 抓拍补光默认强度（0-255，0 表示不补光）。
// ESP32-CAM 板载高亮 LED 位于 GPIO4，暗光下打开可明显提亮。
#define VISION_TORCH_DUTY_DEFAULT 160

// 自动亮度：抓拍前先测环境亮度（0-255），
// 低于阈值才打开补光灯，避免白天过曝。
#define VISION_LUMA_TORCH_ON 80

// 补光分档：环境越暗补光越强。
// 下限以下方滑条设定的 VISION_TORCH_DUTY_DEFAULT 为基础，
// 再按缺口追加，避免距离远时照片发黑。
#define VISION_LUMA_DARK 45
#define VISION_LUMA_VERY_DARK 20

// 默认抓拍分辨率（framesize_t 枚举值）与 JPEG 画质（0-63，数值越小越清晰、体积越大）
#define VISION_FRAMESIZE_DEFAULT 5    // FRAMESIZE_QVGA 320x240
#define VISION_QUALITY_DEFAULT 12

// 网页端等待摄像头的最长时间（毫秒）。
// 识别上传期间相机被占用，超时后直接返回而不是卡住网页线程。
#define VISION_PREVIEW_LOCK_WAIT_MS 2500U
#define VISION_CAM_LOCK_WAIT_MS     3000U

// 传输层偶发失败时的最大尝试次数
#define VISION_HTTP_MAX_ATTEMPTS 4

// /vision/trigger_wait 同步接口最长等待时间（毫秒）。
// 超时后返回 finished=false，调用方可自行再查状态。
#define VISION_SYNC_WAIT_MS 90000UL

// HTTP 层在 /vision/trigger_wait 里最多阻塞多久（毫秒）。
// esp_http_server 只有一条任务，所有 handler 串行执行：如果在这里等满整个
// 识别过程（十几秒），网页、预览、MCP 上报的请求会全部排队，表现为网页卡死、
// MCP 显示未连接。所以只做一次很短的等待，没出结果就立刻放行 httpd 任务，
// 由客户端轮询 /vision/status 拿结果。
#define VISION_HTTP_WAIT_MS 1200UL

// HTTP 读写与连接超时，避免大模型慢响应被截断（60 秒）
#define VISION_HTTP_TIMEOUT_MS 60000U
#define VISION_HTTP_CONNECT_TIMEOUT_MS 15000U

// 视觉任务栈大小；TLS 握手比较吃栈，故给足
#define VISION_TASK_STACK 16384

// 如需证书校验，请填入 CA 证书并改用 setCACert()
// #define VISION_CA_CERT "-----BEGIN CERTIFICATE-----\n...\n-----END CERTIFICATE-----\n"

#endif // VISION_CONFIG_H
