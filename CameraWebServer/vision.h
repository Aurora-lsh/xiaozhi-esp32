#ifndef VISION_H
#define VISION_H

#include <Arduino.h>
#include "vision_config.h"

// 该结构体仅在 vision.cpp 内部维护，通过 visionGetState() 获取一致快照
struct VisionState {
    bool autoEnabled;                      // 是否启用自动识别
    unsigned long intervalMs;              // 自动识别间隔（毫秒）
    bool isProcessing;                     // 当前是否正在请求
    unsigned long lastRequestTime;         // 上次请求完成时间（millis）
    unsigned long lastMessageTime;         // 最新消息时间（millis）
    int requestCount;                      // 成功次数
    int errorCount;                        // 失败次数
    unsigned long messageSeq;              // 消息序号，供网页判断是否有新消息
    unsigned long imageSeq;                 // 抓拍图片序号，网页据此判断是否有新图片
    size_t imageBytes;                      // 最新抓拍图片字节数（0 表示无图）
    uint8_t torchDuty;                      // 抓拍补光强度（0-255）
    uint8_t framesize;                      // 抓拍分辨率（framesize_t 枚举值）
    uint8_t quality;                        // JPEG 画质（0-63，越小越清晰）
    int luma;                               // 最近一次抓拍测得的环境亮度（0-255，-1 表示未测）
    bool lastSuccess;                       // 最近一次识别是否成功
    unsigned long lastDurationMs;           // 最近一次识别的总耗时（毫秒）
    char lastMessage[VISION_MESSAGE_MAX];  // 最新一条消息（UTF-8）
};

// 初始化视觉识别模块，请在 setup() 中调用一次
void visionInit();

// 抓拍补光等级（0-255，0 表示不补光）。硬件上就是 ESP32-CAM 板载的高亮 LED。
void visionSetTorch(uint8_t duty);
uint8_t visionGetTorch();

// 由 app_httpd.cpp 提供的硬件实现：写入补光 LED 占空比
void visionTorchWrite(uint8_t duty);

// 抓拍分辨率（framesize_t 枚举值）与 JPEG 画质（0-63），设置后立即作用于传感器
void visionSetFramesize(uint8_t idx);
uint8_t visionGetFramesize(void);
void visionSetQuality(uint8_t q);
uint8_t visionGetQuality(void);

// 获取一张不上传的预览快照（JPEG），用于网页实时预览与手动对焦。
// withTorch 为 true 时预览期间打开补光灯，方便暗光下对焦。
// 成功时返回 true 并输出已分配的副本，由调用方 free()。
bool visionPreviewSnapshot(uint8_t **buf, size_t *len, bool withTorch);

// 手动触发一次识别（非阻塞，仅投递请求）
// 返回 false 表示当前正忙或已有排队请求
bool visionTriggerManual();

// 设置自动识别开关；intervalMs 为 0 表示识别完成后立即进入下一轮
void visionSetAutoMode(bool enabled, unsigned long intervalMs);

// 触发一次识别并等待完成（同步接口，主要给自动化测试使用）。
// 成功取到结果返回 true；success 表示识别本身成功与否。
// durationMs 为设备侧测得的抓拍+上传+大模型回复总耗时。
bool visionTriggerWait(uint32_t timeoutMs, bool *success,
                       unsigned long *durationMs, char *msg, size_t msgLen);

// 带单次提示词的同步识别；prompt 为空时使用 VISION_DEFAULT_PROMPT。
bool visionTriggerWaitWithPrompt(uint32_t timeoutMs, const char *prompt,
                                 bool *success, unsigned long *durationMs,
                                 char *msg, size_t msgLen);

// 同上，但 fromMcp 为 true 时会把本次抓拍图片与回复记录到 MCP 事件列表，
// 供网页展示“机器人拍摄图片 + 对应识别回复”。记录动作由后台任务完成，
// 因此本函数即使等待超时返回 false，事件也不会丢。
bool visionTriggerWaitMcp(uint32_t timeoutMs, const char *prompt, bool recordMcpEvent,
                          bool *success, unsigned long *durationMs,
                          char *msg, size_t msgLen);

// 非阻塞投递一次识别，立刻返回（不等待结果）。
// fromMcp 为 true 时，后台任务会在识别完成后自动记录 MCP 事件。
// 返回 false 表示当前正忙或已有排队请求。
bool visionTriggerStart(const char *prompt, bool fromMcp);

// 等待“最近一次已投递的识别”完成，最多 timeoutMs。
// 返回 true 表示在超时前完成（success / durationMs / msg 输出结果）；
// 返回 false 表示超时，调用方应改用 visionGetState() 轮询状态，
// 不要长时间占住调用线程（httpd 任务只有一条，堵住会让所有接口排队）。
bool visionWaitDone(uint32_t timeoutMs, bool *success, unsigned long *durationMs,
                    char *msg, size_t msgLen);

// 运行时无线调优（供现场排查链路丢包，无需重新编译）
// protocol  : 协议位图，如 WIFI_PROTOCOL_11B|11G|11N；<0 表示不修改
// bandwidth : WIFI_BW_HT20 / WIFI_BW_HT40；<0 不修改
// psMode    : wifi_ps_type_t（-1 不修改）
// txPower   : 发射功率，单位 0.25dBm（-1 不修改）
// phyRate   : 固定 802.11 发送速率（-1 不修改，-2 恢复自动）
void visionTuneWifi(int protocol, int bandwidth, int psMode, int txPower, int phyRate);

// 获取当前状态快照
VisionState visionGetState();

// 获取最近一次识别使用的抓拍图片（JPEG）。
// 成功返回 true，并通过 buf/len 输出缓冲区指针与长度；使用完后必须调用 visionReleaseImage()。
bool visionAcquireImage(const uint8_t **buf, size_t *len);
void visionReleaseImage();

#endif // VISION_H
