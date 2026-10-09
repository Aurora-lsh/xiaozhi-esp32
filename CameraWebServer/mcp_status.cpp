/*
 * mcp_status.cpp -- MCP（小智）桥接状态、日志与机器人抓拍事件缓存
 *
 * 设计要点：
 *  1. 全部数据放内存，优先 PSRAM；日志与事件都用固定大小的环形缓冲，
 *     不会因为长时间运行而无限增长。
 *  2. 由互斥锁保护，HTTP 处理函数与视觉任务可以并发读写。
 *  3. 事件图片单独存放：网页按序号取图，避免和历史文字错配。
 */

#include <Arduino.h>
#include <string.h>
#include <stdio.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "esp32-hal-psram.h"

#include "mcp_status.h"

// ---------------------------------------------------------------------------
// 内部状态
// ---------------------------------------------------------------------------

struct McpEventSlot {
    McpEventInfo info;
    uint8_t *image;          // JPEG 副本，可为 NULL
    size_t   imageCap;       // 已分配容量
};

static McpLogEntry   s_logs[MCP_LOG_MAX];
static size_t        s_logHead = 0;     // 下一个写入位置
static size_t        s_logCount = 0;
static unsigned long s_logSeq = 0;

static McpEventSlot  s_events[MCP_EVENT_MAX];
static size_t        s_eventHead = 0;   // 下一个写入位置
static size_t        s_eventCount = 0;
static unsigned long s_eventSeq = 0;

static bool          s_connected = false;
static char          s_endpoint[MCP_ENDPOINT_MAX] = "";
static unsigned long s_lastReportMs = 0;
static unsigned long s_reportCount = 0;
static unsigned long s_bootMs = 0;

static SemaphoreHandle_t s_lock = NULL;

static void lockTake(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void lockGive(void)   { if (s_lock) xSemaphoreGive(s_lock); }

// 截断复制，保证不会把 UTF-8 字符从中间切开
static void copyTrunc(char *out, size_t outLen, const char *src)
{
    if (outLen == 0) return;
    if (!src) { out[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= outLen) {
        n = outLen - 1;
        while (n > 0 && (((unsigned char)src[n]) & 0xC0) == 0x80) n--;
    }
    memcpy(out, src, n);
    out[n] = '\0';
}

void mcpStatusInit(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    memset(s_logs, 0, sizeof(s_logs));
    memset(s_events, 0, sizeof(s_events));
    s_logHead = s_logCount = 0;
    s_logSeq = 0;
    s_eventHead = s_eventCount = 0;
    s_eventSeq = 0;
    s_connected = false;
    s_endpoint[0] = '\0';
    s_lastReportMs = 0;
    s_reportCount = 0;
    s_bootMs = millis();
    Serial.println("MCP: 状态模块初始化完成");
}

// ---------------------------------------------------------------------------
// PC 端上报
// ---------------------------------------------------------------------------

void mcpSetConnected(bool connected, const char *endpoint)
{
    lockTake();
    s_connected = connected;
    if (endpoint && endpoint[0]) {
        copyTrunc(s_endpoint, sizeof(s_endpoint), endpoint);
    }
    s_lastReportMs = millis();
    s_reportCount++;
    lockGive();

    Serial.printf("MCP: 上报连接状态 %s%s%s\n",
                  connected ? "已连接" : "已断开",
                  s_endpoint[0] ? "，端点 " : "",
                  s_endpoint[0] ? s_endpoint : "");
}

void mcpAppendLog(const char *level, const char *text)
{
    if (!text) return;

    lockTake();
    McpLogEntry *e = &s_logs[s_logHead];
    s_logHead = (s_logHead + 1) % MCP_LOG_MAX;
    if (s_logCount < MCP_LOG_MAX) s_logCount++;

    e->seq = ++s_logSeq;
    e->uptimeMs = millis();
    copyTrunc(e->level, sizeof(e->level), (level && level[0]) ? level : "info");
    copyTrunc(e->text, sizeof(e->text), text);
    lockGive();
}

// ---------------------------------------------------------------------------
// 查询
// ---------------------------------------------------------------------------

bool mcpIsConnected(void)
{
    lockTake();
    bool v = s_connected;
    lockGive();
    return v;
}

void mcpGetEndpoint(char *out, size_t outLen)
{
    if (!out || outLen == 0) return;
    lockTake();
    copyTrunc(out, outLen, s_endpoint);
    lockGive();
}

unsigned long mcpLastReportMs(void)
{
    lockTake();
    unsigned long v = s_lastReportMs;
    lockGive();
    return v;
}

unsigned long mcpReportCount(void)
{
    lockTake();
    unsigned long v = s_reportCount;
    lockGive();
    return v;
}

unsigned long mcpUptimeMs(void)
{
    return s_bootMs ? (millis() - s_bootMs) : 0;
}

size_t mcpLogCount(void)
{
    lockTake();
    size_t v = s_logCount;
    lockGive();
    return v;
}

size_t mcpStoredEventCount(void)
{
    lockTake();
    size_t v = s_eventCount;
    lockGive();
    return v;
}

unsigned long mcpLastEventSeq(void)
{
    lockTake();
    unsigned long v = s_eventSeq;
    lockGive();
    return v;
}

size_t mcpGetLogs(McpLogEntry *out, size_t max)
{
    if (!out || max == 0) return 0;

    lockTake();
    size_t n = s_logCount < max ? s_logCount : max;
    for (size_t i = 0; i < n; i++) {
        // 最新的在最前面：head 往回数
        size_t idx = (s_logHead + MCP_LOG_MAX - 1 - i) % MCP_LOG_MAX;
        out[i] = s_logs[idx];
    }
    lockGive();
    return n;
}

// ---------------------------------------------------------------------------
// 机器人抓拍事件
// ---------------------------------------------------------------------------

void mcpEventRecord(const char *prompt, bool success, const char *message,
                    const uint8_t *jpeg, size_t jpegLen)
{
    if (jpegLen > MCP_EVENT_IMAGE_MAX) jpeg = NULL;   // 太大就不缓存图片

    lockTake();
    McpEventSlot *slot = &s_events[s_eventHead];
    s_eventHead = (s_eventHead + 1) % MCP_EVENT_MAX;
    if (s_eventCount < MCP_EVENT_MAX) s_eventCount++;

    memset(&slot->info, 0, sizeof(slot->info));
    slot->info.used = true;
    slot->info.seq = ++s_eventSeq;
    slot->info.uptimeMs = millis();
    slot->info.success = success;
    copyTrunc(slot->info.prompt, sizeof(slot->info.prompt), prompt ? prompt : "");
    copyTrunc(slot->info.message, sizeof(slot->info.message), message ? message : "");

    // 图片：能复用就复用，容量不够才重新分配
    if (jpeg && jpegLen > 0) {
        if (!slot->image || slot->imageCap < jpegLen) {
            if (slot->image) free(slot->image);
            slot->image = (uint8_t *)ps_malloc(jpegLen);
            if (!slot->image) slot->image = (uint8_t *)malloc(jpegLen);
            slot->imageCap = slot->image ? jpegLen : 0;
        }
        if (slot->image) {
            memcpy(slot->image, jpeg, jpegLen);
            slot->info.imageBytes = jpegLen;
        } else {
            Serial.println("MCP: [WARN] 事件图片缓存分配失败，仅记录文字");
            slot->info.imageBytes = 0;
        }
    } else if (slot->image) {
        free(slot->image);
        slot->image = NULL;
        slot->imageCap = 0;
        slot->info.imageBytes = 0;
    }

    unsigned long seq = slot->info.seq;
    lockGive();

    Serial.printf("MCP: 记录机器人事件 #%lu，%s，%u 字节图片\n",
                  seq, success ? "成功" : "失败",
                  (unsigned)jpegLen);
}

size_t mcpGetEvents(McpEventInfo *out, size_t max)
{
    if (!out || max == 0) return 0;

    lockTake();
    size_t n = s_eventCount < max ? s_eventCount : max;
    for (size_t i = 0; i < n; i++) {
        // 最新的在最前面
        size_t idx = (s_eventHead + MCP_EVENT_MAX - 1 - i) % MCP_EVENT_MAX;
        out[i] = s_events[idx].info;
    }
    lockGive();
    return n;
}

bool mcpCopyEventImage(unsigned long seq, uint8_t **outBuf, size_t *outLen)
{
    if (!outBuf || !outLen) return false;
    *outBuf = NULL;
    *outLen = 0;

    lockTake();
    for (size_t i = 0; i < s_eventCount; i++) {
        size_t idx = (s_eventHead + MCP_EVENT_MAX - 1 - i) % MCP_EVENT_MAX;
        McpEventSlot *slot = &s_events[idx];
        if (!slot->info.used || slot->info.seq != seq) continue;
        if (!slot->image || slot->info.imageBytes == 0) { lockGive(); return false; }

        uint8_t *copy = (uint8_t *)ps_malloc(slot->info.imageBytes);
        if (!copy) copy = (uint8_t *)malloc(slot->info.imageBytes);
        if (!copy) { lockGive(); return false; }
        memcpy(copy, slot->image, slot->info.imageBytes);
        *outBuf = copy;
        *outLen = slot->info.imageBytes;
        lockGive();
        return true;
    }
    lockGive();
    return false;
}