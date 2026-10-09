#ifndef MCP_STATUS_H
#define MCP_STATUS_H

#include <Arduino.h>
#include "vision_config.h"

// ---------------------------------------------------------------------------
// MCP（小智）桥接状态、日志与机器人抓拍事件
//
// PC 端 MCP 服务通过 /mcp/report 上报连接状态与日志；小智触发的识别会带
// source=mcp，固件把对应图片与回复记录成事件，供网页展示。
// 本模块只做内存缓存（PSRAM 优先），不落盘。
// ---------------------------------------------------------------------------

// 环形缓冲大小
#define MCP_LOG_MAX          24      // 保留的日志条数
#define MCP_LOG_TEXT_MAX     160     // 单条日志最大字节数（UTF-8）
#define MCP_ENDPOINT_MAX     96      // MCP 端点最大字节数
#define MCP_EVENT_MAX        5       // 保留的机器人抓拍事件数
#define MCP_EVENT_IMAGE_MAX  80000UL // 单个事件图片上限，超出只记文字

struct McpLogEntry {
    unsigned long seq;                  // 自增序号
    unsigned long uptimeMs;             // 设备开机后的毫秒数
    char level[8];                      // info / warn / error
    char text[MCP_LOG_TEXT_MAX];        // UTF-8 文本
};

struct McpEventInfo {
    bool used;                          // 该槽位是否有效
    unsigned long seq;                  // 自增序号
    unsigned long uptimeMs;             // 设备开机后的毫秒数
    bool success;                       // 本次识别是否成功
    size_t imageBytes;                  // 0 表示没有缓存图片
    char prompt[VISION_PROMPT_MAX];     // 本次使用的提示词
    char message[VISION_MESSAGE_MAX];   // 识别回复或错误信息（UTF-8）
};

// 初始化（在 visionInit() 之前或之后调用一次均可）
void mcpStatusInit(void);

// ---- PC 端上报 ----
// connected 为 true 时记录连接成功；endpoint 可为 NULL（保持原值）
void mcpSetConnected(bool connected, const char *endpoint);
// 追加一条日志；level 为空时按 info 处理
void mcpAppendLog(const char *level, const char *text);

// ---- 状态查询 ----
bool mcpIsConnected(void);
void mcpGetEndpoint(char *out, size_t outLen);
unsigned long mcpLastReportMs(void);
unsigned long mcpReportCount(void);
unsigned long mcpUptimeMs(void);
size_t mcpLogCount(void);
size_t mcpStoredEventCount(void);
unsigned long mcpLastEventSeq(void);

// ---- 日志快照：按时间倒序（最新在前）写入 out，返回实际条数 ----
size_t mcpGetLogs(McpLogEntry *out, size_t max);

// ---- 机器人抓拍事件 ----
// 记录一次 MCP 触发的识别；jpeg 可为 NULL（只记文字）
void mcpEventRecord(const char *prompt, bool success, const char *message,
                    const uint8_t *jpeg, size_t jpegLen);
// 按时间倒序（最新在前）写入 out，返回实际条数
size_t mcpGetEvents(McpEventInfo *out, size_t max);
// 复制指定序号事件的图片；成功返回 true，由调用方 free()
bool mcpCopyEventImage(unsigned long seq, uint8_t **outBuf, size_t *outLen);

#endif // MCP_STATUS_H