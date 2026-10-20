/*
 * vision.cpp -- ESP32-CAM 大模型视觉识别（OpenAI 兼容 Chat Completions 接口）
 *
 * 设计要点：
 *  1. 所有网络请求都在独立的 FreeRTOS 后台任务中执行，HTTP 处理函数只负责投递请求，
 *     避免阻塞摄像头网页流，也避免在 httpd 任务里直接做 TLS 握手（栈不够会溢出）。
 *  2. g_state 由互斥锁保护，网页状态接口通过 visionGetState() 取一致快照。
 *  3. 大块内存（base64 结果、JPEG、请求体）优先放 PSRAM，失败时回退到内部 RAM。
 */

#include <Arduino.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "esp_wifi.h"
#include "esp_err.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include <mbedtls/base64.h>
#include "esp32-hal-psram.h"

#include "vision.h"
#include "vision_config.h"
#include "mcp_status.h"
#include "esp_camera.h"
#include "img_converters.h"

// 使用 ArduinoJson 解析响应（推荐 7.x）。
// 若未安装该库，可编译时加 -DVISION_NO_ARDUINOJSON 退回内置解析。
#ifndef VISION_NO_ARDUINOJSON
#  include <ArduinoJson.h>
#  define VISION_HAVE_ARDUINOJSON 1
#endif

// ---------------------------------------------------------------------------
// 只读内存流：HTTPClient 的 POST(uint8_t*, size) 会用一次
// _client->write() 把整个负载推给 TLS 层，几十 KB 的请求体在 ESP32 上很容易
// 触发 mbedtls 写入失败（表现为 HTTPC_ERROR_SEND_PAYLOAD_FAILED）。
// 改成 Stream 版本后，HTTPClient 会按 1460 字节分块发送，稳定性好得多。
// ---------------------------------------------------------------------------
class VisionBufferStream : public Stream
{
public:
    VisionBufferStream(const uint8_t *data, size_t len) : _data(data), _len(len), _pos(0) {}

    int available() override { return (int)(_len - _pos); }
    int read() override { return _pos < _len ? _data[_pos++] : -1; }
    int peek() override { return _pos < _len ? _data[_pos] : -1; }
    void flush() override {}
    size_t write(uint8_t) override { return 0; }

    size_t readBytes(uint8_t *buffer, size_t length) override
    {
        size_t n = _len - _pos;
        if (n > length) n = length;
        if (n) { memcpy(buffer, _data + _pos, n); _pos += n; }
        return n;
    }

private:
    const uint8_t *_data;
    size_t _len;
    size_t _pos;
};

#define VISION_STR_HELPER(x) #x
#define VISION_STR(x) VISION_STR_HELPER(x)

// ---------------------------------------------------------------------------
// 全局状态
// ---------------------------------------------------------------------------

static VisionState g_state;
static SemaphoreHandle_t g_stateLock = NULL;
static TaskHandle_t g_taskHandle = NULL;
static volatile int g_pendingManual = 0;   // >0 表示有排队中的手动请求
static bool g_pendingFromMcp = false;      // 排队中的请求是否来自 MCP（决定要不要记录事件）
static char g_pendingPrompt[VISION_PROMPT_MAX] = "";

// 最近一次识别的抓拍图片缓存（JPEG），供网页查看
static uint8_t *g_imageBuf = NULL;
static size_t   g_imageLen = 0;
static SemaphoreHandle_t g_imageLock = NULL;
static uint8_t g_torchDuty = VISION_TORCH_DUTY_DEFAULT;   // 抓拍补光强度
static uint8_t g_framesize = VISION_FRAMESIZE_DEFAULT;   // 抓拍分辨率
static uint8_t g_quality   = VISION_QUALITY_DEFAULT;      // JPEG 画质

// 摄像头只能被一个任务同时使用：自动识别后台任务和网页预览用它互斥
static SemaphoreHandle_t g_camLock = NULL;
static SemaphoreHandle_t g_doneSignal = NULL;   // 识别完成通知（供同步接口等待）
static unsigned long g_requestStartMs = 0;      // 本轮识别起点（用于统计耗时）

// 相机锁带超时：大模型上传最长可能占用一分钟，
// 网页预览 / 改参数不能一直干等，拿不到锁就立刻返回失败。
static bool camLockTake(uint32_t ms)
{
    return g_camLock && xSemaphoreTake(g_camLock, pdMS_TO_TICKS(ms)) == pdTRUE;
}
static void camLockGive(void) { if (g_camLock) xSemaphoreGive(g_camLock); }

// 抓帧阶段的 RAII 守卫：上传和识别期间不再占用摄像头，
// 网页预览与参数修改因此不会被一次识别长时间堵住。
struct CamLockGuard {
    bool held;
    explicit CamLockGuard(uint32_t ms) : held(camLockTake(ms)) {}
    ~CamLockGuard() { release(); }
    void release() {
        if (held) {
            camLockGive();
            held = false;
        }
    }
};

static void stateLock(void)   { if (g_stateLock) xSemaphoreTake(g_stateLock, portMAX_DELAY); }
static void stateUnlock(void) { if (g_stateLock) xSemaphoreGive(g_stateLock); }

static void copyTrunc(char *out, size_t outLen, const char *src);

static void publishMessage(const char *text)
{
    stateLock();
    copyTrunc(g_state.lastMessage, sizeof(g_state.lastMessage), text);
    g_state.lastMessageTime = millis();
    g_state.messageSeq++;
    stateUnlock();
}

static void publishMessagef(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    publishMessage(buf);
}

// 失败收尾：写入错误消息，并更新计数与忙标志。
// 同时刷新 lastRequestTime，使自动模式下一次触发重新计时。
static bool finishFailed(const char *msg)
{
    Serial.printf("Vision: [ERROR] %s\n", msg);
    publishMessage(msg);
    stateLock();
    g_state.errorCount++;
    g_state.isProcessing = false;
    g_state.lastSuccess = false;
    if (g_requestStartMs) g_state.lastDurationMs = millis() - g_requestStartMs;
    g_state.lastRequestTime = millis();
    stateUnlock();
    if (g_doneSignal) xSemaphoreGive(g_doneSignal);
    return false;
}

static bool finishFailedf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return finishFailed(buf);
}

static void finishSuccess(const char *msg)
{
    Serial.printf("Vision: [OK] %s\n", msg);
    publishMessage(msg);
    stateLock();
    g_state.requestCount++;
    g_state.isProcessing = false;
    g_state.lastSuccess = true;
    if (g_requestStartMs) g_state.lastDurationMs = millis() - g_requestStartMs;
    g_state.lastRequestTime = millis();
    stateUnlock();
    if (g_doneSignal) xSemaphoreGive(g_doneSignal);
}

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

// ---------------------------------------------------------------------------
// 抓拍图片缓存：保存本次识别用的 JPEG，网页通过 /vision/image 取图
// ---------------------------------------------------------------------------

// 用 8 倍缩小的 RGB565 均值估计画面亮度（0-255，-1 表示测不到）
static int measureBrightness(const camera_fb_t *fb)
{
    if (!fb || fb->format != PIXFORMAT_JPEG || fb->width < 8 || fb->height < 8) return -1;

    // jpg2rgb565() 不做输出边界检查（它把 outbuf_size 当 UINT32_MAX 交给解码器），
    // 这里按 8 倍缩放下解码器最多可能写出的尺寸（16 像素 MCU 对齐）分配，
    // 多留一点也不能越界写坏堆；PSRAM 优先，避免占用内部 RAM。
    size_t mw = (((size_t)fb->width  + 15) / 16) * 2;   // 每个 16x16 MCU 输出 2x2 像素
    size_t mh = (((size_t)fb->height + 15) / 16) * 2;
    size_t n = mw * mh;
    size_t need = n * 2;

    uint8_t *buf = (uint8_t *)ps_malloc(need);
    if (!buf) buf = (uint8_t *)malloc(need);
    if (!buf) return -1;

    int luma = -1;
    if (jpg2rgb565(fb->buf, fb->len, buf, JPG_SCALE_8X)) {
        const uint16_t *px = (const uint16_t *)buf;
        uint64_t sum = 0;
        for (size_t i = 0; i < n; i++) {
            uint16_t v = px[i];
            unsigned r = (v >> 11) & 0x1F;   // 5 位
            unsigned g = (v >> 5)  & 0x3F;   // 6 位
            unsigned b =  v        & 0x1F;   // 5 位
            sum += (246u * r + 238u * g + 94u * b);
        }
        luma = (int)(sum / (n * 100));       // 归一到 0-255
    }

    free(buf);
    return luma;
}

// 环境亮度 -> 补光占空比：越暗补光越强，亮度足够时完全不开灯。
// 上限以网页上“抓拍补光”滑条（g_torchDuty）为基准，再按缺口向上追加。
static uint8_t torchDutyForLuma(int luma)
{
    if (g_torchDuty == 0) return 0;
    if (luma < 0 || luma >= VISION_LUMA_TORCH_ON) return 0;

    int d = g_torchDuty;
    if (luma < VISION_LUMA_VERY_DARK)      d = g_torchDuty + 90;   // 很暗：接近满功率
    else if (luma < VISION_LUMA_DARK)      d = g_torchDuty + 45;   // 偏暗：适当加强

    if (d > 255) d = 255;
    return (uint8_t)d;
}

static bool storeImage(const uint8_t *buf, size_t len)
{
    if (!g_imageLock || !buf || len == 0) return false;

    uint8_t *copy = (uint8_t *)ps_malloc(len);
    if (!copy) copy = (uint8_t *)malloc(len);
    if (!copy) {
        Serial.println("Vision: [WARN] 抓拍图片缓存分配失败，网页将无法显示本次图片");
        return false;
    }
    memcpy(copy, buf, len);

    xSemaphoreTake(g_imageLock, portMAX_DELAY);
    if (g_imageBuf) free(g_imageBuf);
    g_imageBuf = copy;
    g_imageLen = len;
    xSemaphoreGive(g_imageLock);

    stateLock();
    g_state.imageSeq++;
    g_state.imageBytes = len;
    stateUnlock();
    return true;
}

// ---------------------------------------------------------------------------
// 抓拍图旋转校正
//
// 摄像头模块按当前装配方向固定在外壳内，原始画面相对正常观看方向偏 90 度。
// 直接把原始 JPEG 送给大模型，药盒是横躺的，药名识别率明显下降。
// 这里在识别前做一次像素级旋转，只影响送模型的图片；网页预览仍在
// /vision/image 里保存原始帧，避免破坏既有的取图逻辑。
//
// 实现方式：JPEG 解码为 RGB888 -> 按象限重排像素 -> 重新编码为 JPEG。
// 320x240 的一帧约占用 230KB，优先用 PSRAM，失败时回退内部 RAM。
// ---------------------------------------------------------------------------

// 把 src 的 RGB888 像素按 rotateQuad 旋转到 dst。
// 1=左旋90度（逆时针），2=右旋90度（顺时针），3=180度。
static bool rotateRgb888(const uint8_t *src, size_t srcW, size_t srcH,
                         uint8_t *dst, size_t *outW, size_t *outH, uint8_t rotateQuad)
{
    if (!src || !dst || srcW == 0 || srcH == 0) return false;

    size_t dstW = srcW;
    size_t dstH = srcH;
    bool swap = (rotateQuad == 1 || rotateQuad == 2);
    if (swap) {
        dstW = srcH;
        dstH = srcW;
    }

    for (size_t y = 0; y < dstH; y++) {
        for (size_t x = 0; x < dstW; x++) {
            size_t sx, sy;
            switch (rotateQuad) {
                case 1:  // 左旋 90 度（逆时针）：目标(x,y) 来自源(sx = srcW-1-y, sy = x)
                    sx = srcW - 1 - y;
                    sy = x;
                    break;
                case 2:  // 右旋 90 度（顺时针）：目标(x,y) 来自源(sx = y, sy = srcH-1-x)
                    sx = y;
                    sy = srcH - 1 - x;
                    break;
                case 3:  // 180 度
                    sx = srcW - 1 - x;
                    sy = srcH - 1 - y;
                    break;
                default:
                    sx = x;
                    sy = y;
                    break;
            }
            size_t si = (sy * srcW + sx) * 3;
            size_t di = (y * dstW + x) * 3;
            dst[di]     = src[si];
            dst[di + 1] = src[si + 1];
            dst[di + 2] = src[si + 2];
        }
    }

    *outW = dstW;
    *outH = dstH;
    return true;
}

// 按 VISION_ROTATE_QUAD 旋转一张 JPEG。
// 成功时返回新分配的 JPEG（调用方 free），并把尺寸写入 outW/outH。
// 失败或无需旋转时返回 nullptr，调用方应继续使用原始帧。
static uint8_t *rotateJpegIfNeeded(const uint8_t *jpeg, size_t jpegLen,
                                   size_t srcW, size_t srcH,
                                   size_t *outLen, size_t *outW, size_t *outH)
{
    if (VISION_ROTATE_QUAD == 0 || !jpeg || jpegLen == 0) return nullptr;
    if (srcW == 0 || srcH == 0) return nullptr;
    if ((size_t)srcW * srcH > (size_t)VISION_ROTATE_MAX_PIXELS) {
        Serial.printf("Vision: [WARN] 分辨率 %ux%u 超出旋转上限，按原图上传\n",
                      (unsigned)srcW, (unsigned)srcH);
        return nullptr;
    }

    // 解码为 RGB888（PSRAM 优先）
    uint8_t *rgb = (uint8_t *)ps_malloc((size_t)srcW * srcH * 3);
    if (!rgb) rgb = (uint8_t *)malloc((size_t)srcW * srcH * 3);
    if (!rgb) {
        Serial.println("Vision: [WARN] 旋转解码缓冲区分配失败，按原图上传");
        return nullptr;
    }

    if (!fmt2rgb888(jpeg, jpegLen, PIXFORMAT_JPEG, rgb)) {
        free(rgb);
        Serial.println("Vision: [WARN] 旋转解码失败，按原图上传");
        return nullptr;
    }

    uint8_t *rotated = (uint8_t *)ps_malloc((size_t)srcW * srcH * 3);
    if (!rotated) rotated = (uint8_t *)malloc((size_t)srcW * srcH * 3);
    if (!rotated) {
        free(rgb);
        Serial.println("Vision: [WARN] 旋转输出缓冲区分配失败，按原图上传");
        return nullptr;
    }

    size_t dstW = 0, dstH = 0;
    if (!rotateRgb888(rgb, srcW, srcH, rotated, &dstW, &dstH, VISION_ROTATE_QUAD)) {
        free(rgb);
        free(rotated);
        return nullptr;
    }
    free(rgb);

    // 重新编码为 JPEG
    uint8_t *outBuf = nullptr;
    size_t outBufLen = 0;
    bool ok = fmt2jpg(rotated, (size_t)dstW * dstH * 3, dstW, dstH,
                      PIXFORMAT_RGB888, VISION_ROTATE_JPEG_QUALITY, &outBuf, &outBufLen);
    free(rotated);

    if (!ok || !outBuf || outBufLen == 0) {
        if (outBuf) free(outBuf);
        Serial.println("Vision: [WARN] 旋转后编码失败，按原图上传");
        return nullptr;
    }
    if (outBufLen > VISION_MAX_IMAGE_BYTES) {
        free(outBuf);
        Serial.println("Vision: [WARN] 旋转后图片超出上限，按原图上传");
        return nullptr;
    }

    *outLen = outBufLen;
    if (outW) *outW = dstW;
    if (outH) *outH = dstH;
    Serial.printf("Vision: 已按 %u 度旋转，输出 %ux%u, %u 字节\n",
                  (unsigned)(VISION_ROTATE_QUAD * 90),
                  (unsigned)dstW, (unsigned)dstH, (unsigned)outBufLen);
    return outBuf;
}

bool visionAcquireImage(const uint8_t **buf, size_t *len)
{
    if (!g_imageLock) return false;

    xSemaphoreTake(g_imageLock, portMAX_DELAY);
    if (!g_imageBuf || g_imageLen == 0) {
        xSemaphoreGive(g_imageLock);
        return false;
    }
    *buf = g_imageBuf;
    *len = g_imageLen;
    return true;   // 由调用方调用 visionReleaseImage() 释放
}

void visionReleaseImage(void)
{
    if (g_imageLock) xSemaphoreGive(g_imageLock);
}

// ---------------------------------------------------------------------------
// 组装请求体（OpenAI 兼容 Chat Completions 格式）：
//   {"model":...,"thinking":{"type":"disabled"},"max_tokens":...,
//    "messages":[{"role":"user","content":[
//      {"type":"text","text":...},
//      {"type":"image_url","image_url":{"url":"data:image/jpeg;base64,..."}}]}]}
// ---------------------------------------------------------------------------

static size_t jsonEscapeText(const char *src, char *dst, size_t dstLen)
{
    if (!dst || dstLen == 0) return 0;
    if (!src || !src[0]) src = VISION_DEFAULT_PROMPT;

    size_t w = 0;
    for (size_t i = 0; src[i] != '\0' && w + 1 < dstLen; i++) {
        unsigned char c = (unsigned char)src[i];
        const char *esc = NULL;
        if (c == '"') esc = "\\\"";
        else if (c == '\\') esc = "\\\\";
        else if (c == '\n') esc = "\\n";
        else if (c == '\r') esc = "\\r";
        else if (c == '\t') esc = "\\t";

        if (esc) {
            size_t n = strlen(esc);
            if (w + n >= dstLen) break;
            memcpy(dst + w, esc, n);
            w += n;
        } else if (c >= 0x20) {
            dst[w++] = (char)c;
        }
    }
    dst[w] = '\0';
    return w;
}

static char *buildRequestBody(const uint8_t *jpeg, size_t jpegLen,
                              const char *prompt, size_t *outLen)
{
    // 请求体按 OpenAI 兼容格式拼装（与 vision-mcp 的 openai_compatible 分支一致）：
    //   {"model":...,"thinking":{"type":"disabled"},"max_tokens":...,
    //    "messages":[{"role":"user","content":[
    //      {"type":"text","text":"<prompt>"},
    //      {"type":"image_url","image_url":{"url":"data:image/jpeg;base64,<b64>"}}]}]}
    // thinking 必须显式关闭：DeepSeek V4 开着思考时只返回 reasoning_content，
    // message.content 会是空字符串。
    static const char kPrefix[] =
        "{\"model\":\"" VISION_MODEL "\","
        "\"thinking\":{\"type\":\"disabled\"},"
        "\"max_tokens\":" VISION_STR(VISION_MAX_TOKENS) ","
        "\"messages\":[{\"role\":\"user\",\"content\":["
        "{\"type\":\"text\",\"text\":\"";
    // 闭合 text 块，再打开 image_url 块与 data URL 前缀
    static const char kPromptSuffix[] = "\"},"
        "{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/jpeg;base64,";
    // 收尾：闭合 url 字符串 -> image_url 对象 -> image_url 块 -> content 数组
    //       -> message 对象 -> messages 数组 -> 根对象
    static const char kSuffix[] = "\"}}]}]}";

    // 尽量放 PSRAM；若 PSRAM 不可用则退回内部 RAM
    size_t b64Cap = 4 * ((jpegLen + 2) / 3) + 8;
    char *b64 = (char *)ps_malloc(b64Cap);
    if (!b64) b64 = (char *)malloc(b64Cap);
    if (!b64) {
        Serial.println("Vision: [ERROR] base64 缓冲区分配失败");
        return NULL;
    }

    size_t b64Len = 0;
    int rc = mbedtls_base64_encode((unsigned char *)b64, b64Cap, &b64Len, jpeg, jpegLen);
    if (rc != 0) {
        Serial.printf("Vision: [ERROR] base64 编码失败 rc=%d\n", rc);
        free(b64);
        return NULL;
    }
    b64[b64Len] = '\0';

    char promptEscaped[VISION_PROMPT_MAX * 2 + 8];
    size_t promptLen = jsonEscapeText(prompt, promptEscaped, sizeof(promptEscaped));
    size_t prefixLen = strlen(kPrefix);
    size_t promptSuffixLen = strlen(kPromptSuffix);
    size_t suffixLen = strlen(kSuffix);
    size_t total = prefixLen + promptLen + promptSuffixLen + b64Len + suffixLen;

    char *body = (char *)ps_malloc(total + 1);
    if (!body) body = (char *)malloc(total + 1);
    if (!body) {
        Serial.println("Vision: [ERROR] 请求体分配失败");
        free(b64);
        return NULL;
    }
    memcpy(body, kPrefix, prefixLen);
    memcpy(body + prefixLen, promptEscaped, promptLen);
    memcpy(body + prefixLen + promptLen, kPromptSuffix, promptSuffixLen);
    memcpy(body + prefixLen + promptLen + promptSuffixLen, b64, b64Len);
    memcpy(body + prefixLen + promptLen + promptSuffixLen + b64Len, kSuffix, suffixLen);
    body[total] = '\0';

    free(b64);
    *outLen = total;
    return body;
}

// ---------------------------------------------------------------------------
// 解析
// ---------------------------------------------------------------------------

// 不依赖 ArduinoJson 的兜底解析：在 OpenAI 兼容响应中取
// choices[0].message.content 字符串。注意必须跳过 reasoning_content，
// 否则会把思考内容当成最终回复。
static bool naiveExtractContent(const String &body, char *out, size_t outLen)
{
    // 形如 {"choices":[{"message":{"content":"..."}}],...}
    int anchor = body.indexOf("\"choices\"");
    if (anchor < 0) anchor = 0;

    // 优先定位 message 对象内的 "content"（跳过 reasoning_content）
    int key = -1;
    int msg = body.indexOf("\"message\"", anchor);
    if (msg >= 0) {
        int p = msg + 9;
        while (true) {
            int c = body.indexOf("\"content\"", p);
            if (c < 0) break;
            // 排除 reasoning_content：检查 "content" 前面是否是 "reasoning_"
            if (c >= 11 && body.substring(c - 11, c) == "reasoning_") { p = c + 9; continue; }
            key = c;
            break;
        }
    }
    if (key < 0) key = body.indexOf("\"content\"", anchor);
    if (key < 0) return false;

    int colon = body.indexOf(':', key + 9);
    if (colon < 0) return false;

    // 跳过冒号后的空白，内容必须是字符串（若为数组则放弃）
    int q1 = -1;
    for (int i = colon + 1; i < (int)body.length(); i++) {
        char c = body[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (c == '"') { q1 = i; }
        break;
    }
    if (q1 < 0) return false;

    size_t o = 0;
    for (int i = q1 + 1; i < (int)body.length() && o + 1 < outLen; i++) {
        char c = body[i];
        if (c == '\\') {
            i++;
            if (i >= (int)body.length()) break;
            char e = body[i];
            if      (e == 'n')  out[o++] = '\n';
            else if (e == 't')  out[o++] = '\t';
            else if (e == 'r')  out[o++] = '\r';
            else if (e == 'u')  i += 4;              // \uXXXX 暂不还原
            else                out[o++] = e;
        } else if (c == '"') {
            break;
        } else {
            out[o++] = c;
        }
    }
    out[o] = '\0';
    return o > 0;
}

// 解析响应：返回 true 表示取到可用文本；isError 标记是否为错误消息
static bool parseResponse(const String &body, char *out, size_t outLen, bool *isError)
{
    *isError = false;
    out[0] = '\0';

#if defined(VISION_HAVE_ARDUINOJSON)
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (!err) {
        const char *e = doc["error"]["message"];
        if (e) {
            copyTrunc(out, outLen, e);
            *isError = true;
            return out[0] != '\0';
        }
        // OpenAI 兼容：choices[0].message.content
        JsonVariant content = doc["choices"][0]["message"]["content"];
        if (content.is<const char *>()) {
            copyTrunc(out, outLen, content.as<const char *>());
            return out[0] != '\0';
        }
        // 兼容 content 为数组（多段文本）的返回形式
        if (content.is<JsonArray>()) {
            String s;
            for (JsonVariant part : content.as<JsonArray>()) {
                const char *t = part["text"];
                if (t) s += t;
            }
            copyTrunc(out, outLen, s.c_str());
            return out[0] != '\0';
        }
        // 没有 content 但带 finish_reason 时，把原始 choices 报出来便于定位
        JsonVariant choices = doc["choices"];
        if (!choices.isNull()) {
            String s;
            serializeJson(choices, s);
            copyTrunc(out, outLen, s.c_str());
            return out[0] != '\0';
        }
    }
#endif

    if (naiveExtractContent(body, out, outLen)) return true;
    return false;
}
// 复用同一条 TLS 连接：TLS 握手在本链路上要 1-2 秒，每次重握手会让
// 单次识别明显变慢。服务端断开时下一次请求会自动重新握手。
static WiFiClientSecure s_tlsClient;
static bool s_tlsClientInited = false;

// 关键：HTTPClient 的析构函数会调 _client->stop()，把复用的 TLS 会话直接断掉。
// 这里在堆上只分配一次并终生保留，保证 begin/end 不会把已建立的 TLS 连接关掉，
// 下一次请求可以直接复用，省掉 1-5 秒的握手。
static HTTPClient *s_http = nullptr;

static bool ensureHttpClient(void)
{
    if (s_http) return true;
    s_http = new HTTPClient();
    if (!s_http) {
        Serial.println("Vision: [ERROR] HTTPClient 分配失败");
        return false;
    }
    return true;
}

static bool sendVisionRequest(const char *body, size_t bodyLen, String &resp, int *outCode)
{
    for (int attempt = 1; attempt <= VISION_HTTP_MAX_ATTEMPTS; attempt++) {
        if (!s_tlsClientInited) {
            s_tlsClient.setInsecure();   // 自定义域名暂无 CA，正式部署请改用 setCACert()
            s_tlsClient.setHandshakeTimeout(VISION_HTTP_CONNECT_TIMEOUT_MS / 1000);
            s_tlsClientInited = true;
        }
        // 重试前强制断开，避免复用一条已被服务端关闭的连接
        if (attempt > 1) s_tlsClient.stop();

        if (!ensureHttpClient()) return false;
        HTTPClient &https = *s_http;

        // 小报文立即发送，避免 Nagle 算法带来的 40ms 累计延迟
        s_tlsClient.setNoDelay(true);

        WiFiClientSecure &client = s_tlsClient;

        https.setReuse(true);   // keep-alive：下次请求省掉 TLS 握手
        https.setConnectTimeout(VISION_HTTP_CONNECT_TIMEOUT_MS);
        https.setTimeout(VISION_HTTP_TIMEOUT_MS);

        if (!https.begin(client, VISION_API_ENDPOINT)) {
            https.end();
            Serial.printf("Vision: 第 %d 次初始化请求失败\n", attempt);
            delay(200);
            continue;
        }

        https.addHeader("Content-Type", "application/json");
        https.addHeader("Authorization", String("Bearer ") + VISION_API_KEY);
        https.addHeader("Accept", "application/json");

        Serial.printf("Vision: [t] RSSI %d dBm, 开始 TLS/上传（%u 字节）\n",
                      WiFi.RSSI(), (unsigned)bodyLen);
        unsigned long tSend = millis();

        VisionBufferStream bodyStream((const uint8_t *)body, bodyLen);
        int code = https.sendRequest("POST", &bodyStream, bodyLen);
        Serial.printf("Vision: [t] send+响应头 %lu 毫秒\n", millis() - tSend);

        if (code <= 0) {
            String err = https.errorToString(code);
            https.end();
            s_tlsClient.stop();
            Serial.printf("Vision: 第 %d 次上传失败(%d)：%s\n", attempt, code, err.c_str());
            delay(200);
            continue;
        }

        resp = https.getString();
        Serial.printf("Vision: [t] 读取响应体 %lu 毫秒\n", millis() - tSend);
        https.end();   // 底层 TLS 连接由持久的 HTTPClient 保留，下次复用

        // 4xx 是请求本身的问题（JSON 非法、鉴权失败等），重试没有意义，
        // 直接把响应体交给上层报出来，方便定位。
        if (code >= 400 && code < 500) {
            *outCode = code;
            Serial.printf("Vision: HTTP %d（客户端错误，不重试，%u 字节）\n",
                          code, (unsigned)resp.length());
            return true;
        }

        // 链路丢包会让响应体被截断：JSON 没有闭合就重试，
        // 避免把残缺的响应当成“解析失败”丢给用户。
        bool complete = resp.indexOf("\"choices\"") >= 0 ||
                        resp.indexOf("\"error\"") >= 0;
        if (!complete) {
            Serial.printf("Vision: 第 %d 次响应不完整（%u 字节），重试\n",
                          attempt, (unsigned)resp.length());
            s_tlsClient.stop();
            delay(200);
            continue;
        }

        *outCode = code;
        Serial.printf("Vision: HTTP %d（第 %d 次尝试成功）\n", code, attempt);
        return true;
    }

    return false;
}

// 真正的抓拍与上传；调用前必须已持有 g_camLock。
static bool performVisionRequestImpl(CamLockGuard &cam, const char *prompt)
{
    unsigned long tStage = millis();
    g_requestStartMs = tStage;

    stateLock();
    g_state.isProcessing = true;
    stateUnlock();

// ---- 先关灯测环境亮度，再按亮度决定是否自动补光 ----
    // 必须先在补光关闭时测亮度：如果开着灯测，会把补光自身的光算进环境光，
    // 导致白天也误判为暗光而开灯。
    visionTorchWrite(0);

    camera_fb_t *probe = NULL;
    for (int i = 0; i < VISION_WARMUP_FRAMES; i++) {
        camera_fb_t *warm = esp_camera_fb_get();
        if (warm) {
            if (probe) esp_camera_fb_return(probe);
            probe = warm;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    int luma = probe ? measureBrightness(probe) : -1;
    if (probe) esp_camera_fb_return(probe);

    stateLock();
    g_state.luma = luma;
    stateUnlock();

    // 暗光自动补光：亮度低于阈值才开灯，白天保持关闭以避免过曝。
    uint8_t torchDuty = torchDutyForLuma(luma);
    if (torchDuty > 0) {
        visionTorchWrite(torchDuty);
        // 开灯后自动曝光需要重新收敛，先丢弃几帧再抓拍，否则照片依然偏暗。
        for (int i = 0; i < VISION_WARMUP_FRAMES; i++) {
            camera_fb_t *warm = esp_camera_fb_get();
            if (warm) esp_camera_fb_return(warm);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        Serial.printf("Vision: 环境亮度 %d，自动补光占空比 %u\n",
                      luma, (unsigned)torchDuty);
    } else {
        Serial.printf("Vision: 环境亮度 %d，无需补光\n", luma);
    }

    // 抓帧带重试：偶发拿不到帧时重试
    camera_fb_t *fb = NULL;
    for (int a = 0; a < 5 && !fb; a++) {
        fb = esp_camera_fb_get();
        if (!fb) vTaskDelay(pdMS_TO_TICKS(80));
    }

    // 帧已取到（或已放弃），立即熄灯：补光只在抓拍瞬间亮，
    // 既避免 LED 长时间常亮发热，也不影响后续预览画面。
    if (torchDuty > 0) visionTorchWrite(0);

    if (!fb) {
        return finishFailed("抓拍失败：无法获取相机帧");
    }
    if (fb->format != PIXFORMAT_JPEG) {
        esp_camera_fb_return(fb);
        return finishFailed("相机当前不是 JPEG 格式，请检查配置");
    }
    if (fb->len == 0 || fb->len > VISION_MAX_IMAGE_BYTES) {
        size_t len = fb->len;
        esp_camera_fb_return(fb);
        return finishFailedf("图像过大（%u 字节），已跳过本次识别", (unsigned)len);
    }

    Serial.printf("Vision: 抓拍 %ux%u, %u 字节\n",
                  fb->width, fb->height, (unsigned)fb->len);

    // 先把本次抓拍存下来，无论上传成功与否，网页都能看到对应图片。
    // 这里存的是原始帧，保持 /vision/image 的既有行为。
    storeImage(fb->buf, fb->len);

    // 送模型前做方向校正：外壳内摄像头是侧装的，原图与正常观看方向差 90 度，
    // 不校正会让大模型看到横躺的药盒。旋转失败时退回原始帧，不影响识别流程。
    size_t uploadLen = fb->len;
    const uint8_t *uploadBuf = fb->buf;
    uint8_t *rotatedJpeg = nullptr;
    size_t rotatedW = 0, rotatedH = 0;
    if (VISION_ROTATE_QUAD != 0) {
        rotatedJpeg = rotateJpegIfNeeded(fb->buf, fb->len, fb->width, fb->height,
                                         &uploadLen, &rotatedW, &rotatedH);
        if (rotatedJpeg) uploadBuf = rotatedJpeg;
    }

    size_t bodyLen = 0;
    char *body = buildRequestBody(uploadBuf, uploadLen, prompt, &bodyLen);
    if (rotatedJpeg) free(rotatedJpeg);   // 请求体已拷贝，尽快释放
    esp_camera_fb_return(fb);             // 请求体已拷贝，尽快归还帧缓冲
    cam.release();                        // 后面只剩网络传输，不再占用摄像头
    if (!body) {
        return finishFailed("构造请求体失败");
    }
    Serial.printf("Vision: 请求体 %u 字节（抓拍阶段 %lu 毫秒），开始上传\n",
                  (unsigned)bodyLen, millis() - tStage);

    unsigned long tUpload = millis();
    String resp;
    int code = 0;
    bool sent = sendVisionRequest(body, bodyLen, resp, &code);
    free(body);
    if (sent) {
        Serial.printf("Vision: 上传+模型回复 %lu 毫秒\n", millis() - tUpload);
    }

    if (!sent) {
        return finishFailedf("请求失败：上传重试 %d 次仍未成功", VISION_HTTP_MAX_ATTEMPTS);
    }

    if (code >= 200 && code < 300) {
        Serial.printf("Vision: 响应 %u 字节\n", (unsigned)resp.length());

        char msg[VISION_MESSAGE_MAX];
        bool isErr = false;
        if (parseResponse(resp, msg, sizeof(msg), &isErr) && msg[0] != '\0') {
            if (isErr) {
                return finishFailedf("接口返回错误：%s", msg);
            }
            finishSuccess(msg);
            return true;
        }
        return finishFailed("响应解析失败，未取到文本");
    }

    char brief[128];
    copyTrunc(brief, sizeof(brief), resp.c_str());
    return finishFailedf("请求失败 HTTP %d%s%s", code,
                         brief[0] ? "：" : "", brief);
}

// 摄像头只在抓帧阶段被占用，上传期间就已释放，
// 网页预览不会被一次识别长时间堵死。同时记录本次识别耗时。
static bool performVisionRequest(const char *prompt, bool fromMcp)
{
    CamLockGuard cam(VISION_CAM_LOCK_WAIT_MS);
    if (!cam.held) {
        return finishFailed("相机正忙，请稍后再试");
    }

    bool ok = performVisionRequestImpl(cam, prompt);
    unsigned long dt = millis() - g_requestStartMs;
    Serial.printf("Vision: 本次识别耗时 %lu 毫秒，%s\n", dt, ok ? "成功" : "失败");

    // MCP 事件在识别结束后立即由后台任务记录：这样同步等待接口就算提前超时
    // 返回（不再长时间占用 httpd 任务），图片与回复也不会丢。
    if (fromMcp) {
        VisionState st = visionGetState();
        const uint8_t *img = NULL;
        size_t imgLen = 0;
        bool hasImg = visionAcquireImage(&img, &imgLen);
        mcpEventRecord(prompt ? prompt : VISION_DEFAULT_PROMPT, st.lastSuccess,
                       st.lastMessage, hasImg ? img : NULL, hasImg ? imgLen : 0);
        if (hasImg) visionReleaseImage();
    }
    return ok;
}

// ---------------------------------------------------------------------------
// 后台任务
// ---------------------------------------------------------------------------

static void visionTask(void *param)
{
    (void)param;
    Serial.println("Vision: 后台任务已启动");

    for (;;) {
        bool run = false;
        bool fromMcp = false;
        char prompt[VISION_PROMPT_MAX];
        prompt[0] = '\0';

        stateLock();
        if (g_pendingManual > 0) {
            g_pendingManual--;
            copyTrunc(prompt, sizeof(prompt), g_pendingPrompt);
            g_pendingPrompt[0] = '\0';
            fromMcp = g_pendingFromMcp;
            g_pendingFromMcp = false;
            run = true;
        } else if (g_state.autoEnabled &&
                   (millis() - g_state.lastRequestTime) >= g_state.intervalMs) {
            run = true;
        }
        stateUnlock();

        if (run) {
            performVisionRequest(prompt[0] ? prompt : VISION_DEFAULT_PROMPT, fromMcp);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------

void visionInit(void)
{
    memset(&g_state, 0, sizeof(g_state));
    g_state.autoEnabled = false;
    g_state.intervalMs = VISION_DEFAULT_INTERVAL_MS;
    g_state.isProcessing = false;
    g_state.lastRequestTime = millis();
    g_state.torchDuty = g_torchDuty;
    g_state.framesize = g_framesize;
    g_state.quality   = g_quality;
    g_state.luma = -1;
    g_state.lastDurationMs = 0;
    g_state.lastSuccess = false;
    copyTrunc(g_state.lastMessage, sizeof(g_state.lastMessage), "尚未识别");

    g_imageLock = xSemaphoreCreateMutex();
    if (!g_imageLock) {
        Serial.println("Vision: [ERROR] 创建图片缓存互斥锁失败");
    }

    g_camLock = xSemaphoreCreateMutex();
    g_doneSignal = xSemaphoreCreateBinary();
    if (!g_camLock) {
        Serial.println("Vision: [ERROR] 创建摄像头互斥锁失败");
    }

    // 把默认分辨率与画质写进传感器，保证与状态接口一致
    visionSetFramesize(g_framesize);
    visionSetQuality(g_quality);

    g_stateLock = xSemaphoreCreateMutex();
    if (!g_stateLock) {
        Serial.println("Vision: [ERROR] 创建互斥锁失败");
        return;
    }

    BaseType_t ok = xTaskCreate(visionTask, "VisionTask", VISION_TASK_STACK, NULL, 1, &g_taskHandle);
    if (ok != pdPASS) {
        Serial.println("Vision: [ERROR] 创建任务失败");
        return;
    }

    if (!psramFound()) {
        Serial.println("Vision: 警告 - 未检测到 PSRAM，大图识别可能失败");
    }
    Serial.printf("Vision: 初始化完成，模型 %s，间隔 %lu 毫秒\n",
                  VISION_MODEL, VISION_DEFAULT_INTERVAL_MS);
}

void visionSetTorch(uint8_t duty)
{
    g_torchDuty = duty;
    stateLock();
    g_state.torchDuty = duty;
    stateUnlock();
}

uint8_t visionGetTorch(void)
{
    return g_torchDuty;
}

static bool queueManualRequest(const char *prompt, bool fromMcp)
{
    if (!g_stateLock) return false;

    bool ok = false;
    stateLock();
    if (!g_state.isProcessing && g_pendingManual == 0) {
        copyTrunc(g_pendingPrompt, sizeof(g_pendingPrompt), prompt ? prompt : "");
        g_pendingFromMcp = fromMcp;
        g_pendingManual = 1;
        ok = true;
    }
    stateUnlock();

    if (!ok) {
        Serial.println("Vision: 已在处理中或有排队请求，忽略本次手动触发");
    }
    return ok;
}

bool visionTriggerManual(void)
{
    return queueManualRequest(NULL, false);
}

bool visionTriggerWait(uint32_t timeoutMs, bool *success,
                       unsigned long *durationMs, char *msg, size_t msgLen)
{
    return visionTriggerWaitWithPrompt(timeoutMs, NULL, success, durationMs, msg, msgLen);
}

bool visionTriggerWaitWithPrompt(uint32_t timeoutMs, const char *prompt,
                                 bool *success, unsigned long *durationMs,
                                 char *msg, size_t msgLen)
{
    return visionTriggerWaitMcp(timeoutMs, prompt, false, success, durationMs, msg, msgLen);
}

bool visionTriggerStart(const char *prompt, bool fromMcp)
{
    if (!g_doneSignal || !g_stateLock) return false;

    // 先清掉可能残留的完成信号，再投递本次请求
    xSemaphoreTake(g_doneSignal, 0);
    return queueManualRequest(prompt, fromMcp);
}

bool visionWaitDone(uint32_t timeoutMs, bool *success, unsigned long *durationMs,
                    char *msg, size_t msgLen)
{
    if (!g_doneSignal) return false;
    if (xSemaphoreTake(g_doneSignal, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) {
        return false;   // 超时：调用方应改用 visionGetState() 轮询
    }

    VisionState st = visionGetState();
    if (success)       *success = st.lastSuccess;
    if (durationMs)    *durationMs = st.lastDurationMs;
    if (msg && msgLen) copyTrunc(msg, msgLen, st.lastMessage);
    return true;
}

bool visionTriggerWaitMcp(uint32_t timeoutMs, const char *prompt, bool recordMcpEvent,
                          bool *success, unsigned long *durationMs,
                          char *msg, size_t msgLen)
{
    // recordMcpEvent 只决定“要不要记录事件”，真正的记录由后台任务在识别
    // 完成后完成，所以这里即使等待超时也不会丢图片与回复。
    if (!visionTriggerStart(prompt, recordMcpEvent)) {
        return false;
    }
    if (!visionWaitDone(timeoutMs, success, durationMs, msg, msgLen)) {
        Serial.println("Vision: 同步等待识别完成超时");
        return false;
    }
    return true;
}

void visionSetAutoMode(bool enabled, unsigned long intervalMs)
{
    if (!g_stateLock) return;

    stateLock();
    bool wasEnabled = g_state.autoEnabled;
    g_state.autoEnabled = enabled;

    if (intervalMs < VISION_MIN_INTERVAL_MS) intervalMs = VISION_MIN_INTERVAL_MS;
    if (intervalMs > VISION_MAX_INTERVAL_MS) intervalMs = VISION_MAX_INTERVAL_MS;
    g_state.intervalMs = intervalMs;
    if (enabled && !wasEnabled) {
        // 刚打开自动模式时立即触发一次
        g_state.lastRequestTime = millis() - g_state.intervalMs;
    }
    unsigned long iv = g_state.intervalMs;
    stateUnlock();

    Serial.printf("Vision: 自动模式 %s，间隔 %lu 毫秒\n",
                  enabled ? "开启" : "关闭", iv);
}

void visionTuneWifi(int protocol, int bandwidth, int psMode, int txPower, int phyRate)
{
    if (protocol >= 0) {
        esp_err_t e = esp_wifi_set_protocol(WIFI_IF_STA, (uint8_t)protocol);
        Serial.printf("Vision: [wifi] protocol=0x%X -> %s\n", protocol, esp_err_to_name(e));
    }
    if (bandwidth >= 0) {
        esp_err_t e = esp_wifi_set_bandwidth(WIFI_IF_STA, (wifi_bandwidth_t)bandwidth);
        Serial.printf("Vision: [wifi] bandwidth=%d -> %s\n", bandwidth, esp_err_to_name(e));
    }
    if (psMode >= 0) {
        esp_err_t e = esp_wifi_set_ps((wifi_ps_type_t)psMode);
        Serial.printf("Vision: [wifi] ps=%d -> %s\n", psMode, esp_err_to_name(e));
    }
    if (txPower >= 0) {
        esp_err_t e = esp_wifi_set_max_tx_power((int8_t)txPower);
        Serial.printf("Vision: [wifi] txpower=%d -> %s\n", txPower, esp_err_to_name(e));
    }
    if (phyRate == -2) {
        esp_err_t e = esp_wifi_config_80211_tx_rate(WIFI_IF_STA, WIFI_PHY_RATE_1M_L);
        Serial.printf("Vision: [wifi] phy=auto(1M_L base) -> %s\n", esp_err_to_name(e));
    } else if (phyRate >= 0) {
        esp_err_t e = esp_wifi_config_80211_tx_rate(WIFI_IF_STA, (wifi_phy_rate_t)phyRate);
        Serial.printf("Vision: [wifi] phy=%d -> %s\n", phyRate, esp_err_to_name(e));
    }
}

VisionState visionGetState(void)
{
    VisionState copy;
    stateLock();
    copy = g_state;
    stateUnlock();
    return copy;
}

// ---------------------------------------------------------------------------
// 抓拍参数：分辨率 / 画质 / 预览
// ---------------------------------------------------------------------------

static uint8_t clampFramesize(uint8_t idx)
{
    // 只允许传感器支持范围内的已知档位，索引即 framesize_t 枚举值
    if (idx < FRAMESIZE_QVGA || idx > FRAMESIZE_UXGA) idx = VISION_FRAMESIZE_DEFAULT;
    return idx;
}

void visionSetFramesize(uint8_t idx)
{
    idx = clampFramesize(idx);
    g_framesize = idx;

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        bool locked = camLockTake(VISION_CAM_LOCK_WAIT_MS);
        s->set_framesize(s, (framesize_t)idx);
        if (locked) camLockGive();
    }

    stateLock();
    g_state.framesize = idx;
    stateUnlock();
}

uint8_t visionGetFramesize(void) { return g_framesize; }

void visionSetQuality(uint8_t q)
{
    if (q < 4)  q = 4;      // 再低体积会太大，ESP32 上传容易失败
    if (q > 30) q = 30;
    g_quality = q;

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        bool locked = camLockTake(VISION_CAM_LOCK_WAIT_MS);
        s->set_quality(s, q);
        if (locked) camLockGive();
    }

    stateLock();
    g_state.quality = q;
    stateUnlock();
}

uint8_t visionGetQuality(void) { return g_quality; }

bool visionPreviewSnapshot(uint8_t **buf, size_t *len, bool withTorch)
{
    if (!buf || !len) return false;
    *buf = NULL;
    *len = 0;
    if (!camLockTake(VISION_PREVIEW_LOCK_WAIT_MS)) return false;

    uint8_t duty = (withTorch && g_torchDuty > 0) ? g_torchDuty : 0;
    if (duty > 0) {
        visionTorchWrite(duty);
        vTaskDelay(pdMS_TO_TICKS(120));   // 等一帧，让补光后的曝光稳定
    }

    bool ok = false;
    camera_fb_t *fb = esp_camera_fb_get();   // 丢掉上一帧，保证画面是新的
    if (fb) {
        esp_camera_fb_return(fb);
        fb = esp_camera_fb_get();
    }
    if (fb && fb->format == PIXFORMAT_JPEG && fb->len > 0) {
        uint8_t *copy = (uint8_t *)ps_malloc(fb->len);
        if (!copy) copy = (uint8_t *)malloc(fb->len);
        if (copy) {
            memcpy(copy, fb->buf, fb->len);
            *buf = copy;
            *len = fb->len;
            ok = true;
        }
    }
    if (fb) esp_camera_fb_return(fb);
    if (duty > 0) visionTorchWrite(0);   // 预览帧取完立即熄灯

    camLockGive();
    return ok;
}
