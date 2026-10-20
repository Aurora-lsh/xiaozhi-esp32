// Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "esp_http_server.h"
#include <string.h>
#include <WiFi.h>
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "img_converters.h"
#include "fb_gfx.h"
#include "esp32-hal-ledc.h"
#include "sdkconfig.h"
#include <stdlib.h>
#include "vision.h"
#include "mcp_status.h"
#include "device_config.h"
#include <ArduinoJson.h>

#if defined(ARDUINO_ARCH_ESP32) && defined(CONFIG_ARDUHAL_ESP_LOG)
#include "esp32-hal-log.h"
#endif

// Face Detection will not work on boards without (or with disabled) PSRAM
#ifdef BOARD_HAS_PSRAM
#define CONFIG_ESP_FACE_DETECT_ENABLED 1
// Face Recognition takes upward from 15 seconds per frame on chips other than ESP32S3
// Makes no sense to have it enabled for them
#if CONFIG_IDF_TARGET_ESP32S3
#define CONFIG_ESP_FACE_RECOGNITION_ENABLED 1
#else
#define CONFIG_ESP_FACE_RECOGNITION_ENABLED 0
#endif
#else
#define CONFIG_ESP_FACE_DETECT_ENABLED 0
#define CONFIG_ESP_FACE_RECOGNITION_ENABLED 0
#endif

#if CONFIG_ESP_FACE_DETECT_ENABLED

#include <vector>
#include "human_face_detect_msr01.hpp"
#include "human_face_detect_mnp01.hpp"

#define TWO_STAGE 1 /*<! 1: detect by two-stage which is more accurate but slower(with keypoints). */
                    /*<! 0: detect by one-stage which is less accurate but faster(without keypoints). */

#if CONFIG_ESP_FACE_RECOGNITION_ENABLED
#include "face_recognition_tool.hpp"
#include "face_recognition_112_v1_s16.hpp"
#include "face_recognition_112_v1_s8.hpp"

#define QUANT_TYPE 0 //if set to 1 => very large firmware, very slow, reboots when streaming...

#define FACE_ID_SAVE_NUMBER 7
#endif

#define FACE_COLOR_WHITE 0x00FFFFFF
#define FACE_COLOR_BLACK 0x00000000
#define FACE_COLOR_RED 0x000000FF
#define FACE_COLOR_GREEN 0x0000FF00
#define FACE_COLOR_BLUE 0x00FF0000
#define FACE_COLOR_YELLOW (FACE_COLOR_RED | FACE_COLOR_GREEN)
#define FACE_COLOR_CYAN (FACE_COLOR_BLUE | FACE_COLOR_GREEN)
#define FACE_COLOR_PURPLE (FACE_COLOR_BLUE | FACE_COLOR_RED)
#endif

// Enable LED FLASH setting
#define CONFIG_LED_ILLUMINATOR_ENABLED 1

// LED FLASH setup
#if CONFIG_LED_ILLUMINATOR_ENABLED

#define LED_LEDC_CHANNEL 2 //Using different ledc channel/timer than camera
#define CONFIG_LED_MAX_INTENSITY 255

int led_duty = 0;
bool isStreaming = false;

#endif

typedef struct
{
    httpd_req_t *req;
    size_t len;
} jpg_chunking_t;

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *_STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *_STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *_STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\nX-Timestamp: %d.%06d\r\n\r\n";

httpd_handle_t stream_httpd = NULL;
httpd_handle_t camera_httpd = NULL;

#if CONFIG_ESP_FACE_DETECT_ENABLED

static int8_t detection_enabled = 0;

// #if TWO_STAGE
// static HumanFaceDetectMSR01 s1(0.1F, 0.5F, 10, 0.2F);
// static HumanFaceDetectMNP01 s2(0.5F, 0.3F, 5);
// #else
// static HumanFaceDetectMSR01 s1(0.3F, 0.5F, 10, 0.2F);
// #endif

#if CONFIG_ESP_FACE_RECOGNITION_ENABLED
static int8_t recognition_enabled = 0;
static int8_t is_enrolling = 0;

#if QUANT_TYPE
    // S16 model
    FaceRecognition112V1S16 recognizer;
#else
    // S8 model
    FaceRecognition112V1S8 recognizer;
#endif
#endif

#endif

typedef struct
{
    size_t size;  //number of values used for filtering
    size_t index; //current value index
    size_t count; //value count
    int sum;
    int *values; //array to be filled with values
} ra_filter_t;

static ra_filter_t ra_filter;

static ra_filter_t *ra_filter_init(ra_filter_t *filter, size_t sample_size)
{
    memset(filter, 0, sizeof(ra_filter_t));

    filter->values = (int *)malloc(sample_size * sizeof(int));
    if (!filter->values)
    {
        return NULL;
    }
    memset(filter->values, 0, sample_size * sizeof(int));

    filter->size = sample_size;
    return filter;
}

#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
static int ra_filter_run(ra_filter_t *filter, int value)
{
    if (!filter->values)
    {
        return value;
    }
    filter->sum -= filter->values[filter->index];
    filter->values[filter->index] = value;
    filter->sum += filter->values[filter->index];
    filter->index++;
    filter->index = filter->index % filter->size;
    if (filter->count < filter->size)
    {
        filter->count++;
    }
    return filter->sum / filter->count;
}
#endif

#if CONFIG_ESP_FACE_DETECT_ENABLED
#if CONFIG_ESP_FACE_RECOGNITION_ENABLED
static void rgb_print(fb_data_t *fb, uint32_t color, const char *str)
{
    fb_gfx_print(fb, (fb->width - (strlen(str) * 14)) / 2, 10, color, str);
}

static int rgb_printf(fb_data_t *fb, uint32_t color, const char *format, ...)
{
    char loc_buf[64];
    char *temp = loc_buf;
    int len;
    va_list arg;
    va_list copy;
    va_start(arg, format);
    va_copy(copy, arg);
    len = vsnprintf(loc_buf, sizeof(loc_buf), format, arg);
    va_end(copy);
    if (len >= sizeof(loc_buf))
    {
        temp = (char *)malloc(len + 1);
        if (temp == NULL)
        {
            return 0;
        }
    }
    vsnprintf(temp, len + 1, format, arg);
    va_end(arg);
    rgb_print(fb, color, temp);
    if (len > 64)
    {
        free(temp);
    }
    return len;
}
#endif
static void draw_face_boxes(fb_data_t *fb, std::list<dl::detect::result_t> *results, int face_id)
{
    int x, y, w, h;
    uint32_t color = FACE_COLOR_YELLOW;
    if (face_id < 0)
    {
        color = FACE_COLOR_RED;
    }
    else if (face_id > 0)
    {
        color = FACE_COLOR_GREEN;
    }
    if(fb->bytes_per_pixel == 2){
        //color = ((color >> 8) & 0xF800) | ((color >> 3) & 0x07E0) | (color & 0x001F);
        color = ((color >> 16) & 0x001F) | ((color >> 3) & 0x07E0) | ((color << 8) & 0xF800);
    }
    int i = 0;
    for (std::list<dl::detect::result_t>::iterator prediction = results->begin(); prediction != results->end(); prediction++, i++)
    {
        // rectangle box
        x = (int)prediction->box[0];
        y = (int)prediction->box[1];
        w = (int)prediction->box[2] - x + 1;
        h = (int)prediction->box[3] - y + 1;
        if((x + w) > fb->width){
            w = fb->width - x;
        }
        if((y + h) > fb->height){
            h = fb->height - y;
        }
        fb_gfx_drawFastHLine(fb, x, y, w, color);
        fb_gfx_drawFastHLine(fb, x, y + h - 1, w, color);
        fb_gfx_drawFastVLine(fb, x, y, h, color);
        fb_gfx_drawFastVLine(fb, x + w - 1, y, h, color);
#if TWO_STAGE
        // landmarks (left eye, mouth left, nose, right eye, mouth right)
        int x0, y0, j;
        for (j = 0; j < 10; j+=2) {
            x0 = (int)prediction->keypoint[j];
            y0 = (int)prediction->keypoint[j+1];
            fb_gfx_fillRect(fb, x0, y0, 3, 3, color);
        }
#endif
    }
}

#if CONFIG_ESP_FACE_RECOGNITION_ENABLED
static int run_face_recognition(fb_data_t *fb, std::list<dl::detect::result_t> *results)
{
    std::vector<int> landmarks = results->front().keypoint;
    int id = -1;

    Tensor<uint8_t> tensor;
    tensor.set_element((uint8_t *)fb->data).set_shape({fb->height, fb->width, 3}).set_auto_free(false);

    int enrolled_count = recognizer.get_enrolled_id_num();

    if (enrolled_count < FACE_ID_SAVE_NUMBER && is_enrolling){
        id = recognizer.enroll_id(tensor, landmarks, "", true);
        log_i("Enrolled ID: %d", id);
        rgb_printf(fb, FACE_COLOR_CYAN, "ID[%u]", id);
    }

    face_info_t recognize = recognizer.recognize(tensor, landmarks);
    if(recognize.id >= 0){
        rgb_printf(fb, FACE_COLOR_GREEN, "ID[%u]: %.2f", recognize.id, recognize.similarity);
    } else {
        rgb_print(fb, FACE_COLOR_RED, "Intruder Alert!");
    }
    return recognize.id;
}
#endif
#endif

#if CONFIG_LED_ILLUMINATOR_ENABLED
// 供 vision.cpp 调用：直接写入补光 LED 占空比
void visionTorchWrite(uint8_t duty)
{
    ledcWrite(LED_LEDC_CHANNEL, duty);
}

void enable_led(bool en)
{ // Turn LED On or Off
    int duty = en ? led_duty : 0;
    if (en && isStreaming && (led_duty > CONFIG_LED_MAX_INTENSITY))
    {
        duty = CONFIG_LED_MAX_INTENSITY;
    }
    ledcWrite(LED_LEDC_CHANNEL, duty);
    //ledc_set_duty(CONFIG_LED_LEDC_SPEED_MODE, CONFIG_LED_LEDC_CHANNEL, duty);
    //ledc_update_duty(CONFIG_LED_LEDC_SPEED_MODE, CONFIG_LED_LEDC_CHANNEL);
    log_i("Set LED intensity to %d", duty);
}
#endif

static esp_err_t bmp_handler(httpd_req_t *req)
{
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
    uint64_t fr_start = esp_timer_get_time();
#endif
    fb = esp_camera_fb_get();
    if (!fb)
    {
        log_e("Camera capture failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/x-windows-bmp");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.bmp");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    char ts[32];
    snprintf(ts, 32, "%ld.%06ld", fb->timestamp.tv_sec, fb->timestamp.tv_usec);
    httpd_resp_set_hdr(req, "X-Timestamp", (const char *)ts);


    uint8_t * buf = NULL;
    size_t buf_len = 0;
    bool converted = frame2bmp(fb, &buf, &buf_len);
    esp_camera_fb_return(fb);
    if(!converted){
        log_e("BMP Conversion failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    res = httpd_resp_send(req, (const char *)buf, buf_len);
    free(buf);
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
    uint64_t fr_end = esp_timer_get_time();
#endif
    log_i("BMP: %llums, %uB", (uint64_t)((fr_end - fr_start) / 1000), buf_len);
    return res;
}

static size_t jpg_encode_stream(void *arg, size_t index, const void *data, size_t len)
{
    jpg_chunking_t *j = (jpg_chunking_t *)arg;
    if (!index)
    {
        j->len = 0;
    }
    if (httpd_resp_send_chunk(j->req, (const char *)data, len) != ESP_OK)
    {
        return 0;
    }
    j->len += len;
    return len;
}

static esp_err_t capture_handler(httpd_req_t *req)
{
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
    int64_t fr_start = esp_timer_get_time();
#endif

#if CONFIG_LED_ILLUMINATOR_ENABLED
    enable_led(true);
    vTaskDelay(150 / portTICK_PERIOD_MS); // The LED needs to be turned on ~150ms before the call to esp_camera_fb_get()
    fb = esp_camera_fb_get();             // or it won't be visible in the frame. A better way to do this is needed.
    enable_led(false);
#else
    fb = esp_camera_fb_get();
#endif

    if (!fb)
    {
        log_e("Camera capture failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    char ts[32];
    snprintf(ts, 32, "%ld.%06ld", fb->timestamp.tv_sec, fb->timestamp.tv_usec);
    httpd_resp_set_hdr(req, "X-Timestamp", (const char *)ts);

#if CONFIG_ESP_FACE_DETECT_ENABLED
    size_t out_len, out_width, out_height;
    uint8_t *out_buf;
    bool s;
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
    bool detected = false;
#endif
    int face_id = 0;
    if (!detection_enabled || fb->width > 400)
    {
#endif
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
        size_t fb_len = 0;
#endif
        if (fb->format == PIXFORMAT_JPEG)
        {
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
            fb_len = fb->len;
#endif
            res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
        }
        else
        {
            jpg_chunking_t jchunk = {req, 0};
            res = frame2jpg_cb(fb, 80, jpg_encode_stream, &jchunk) ? ESP_OK : ESP_FAIL;
            httpd_resp_send_chunk(req, NULL, 0);
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
            fb_len = jchunk.len;
#endif
        }
        esp_camera_fb_return(fb);
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
        int64_t fr_end = esp_timer_get_time();
#endif
        log_i("JPG: %uB %ums", (uint32_t)(fb_len), (uint32_t)((fr_end - fr_start) / 1000));
        return res;
#if CONFIG_ESP_FACE_DETECT_ENABLED
    }

    jpg_chunking_t jchunk = {req, 0};

    if (fb->format == PIXFORMAT_RGB565
#if CONFIG_ESP_FACE_RECOGNITION_ENABLED
     && !recognition_enabled
#endif
     ){
#if TWO_STAGE
        HumanFaceDetectMSR01 s1(0.1F, 0.5F, 10, 0.2F);
        HumanFaceDetectMNP01 s2(0.5F, 0.3F, 5);
        std::list<dl::detect::result_t> &candidates = s1.infer((uint16_t *)fb->buf, {(int)fb->height, (int)fb->width, 3});
        std::list<dl::detect::result_t> &results = s2.infer((uint16_t *)fb->buf, {(int)fb->height, (int)fb->width, 3}, candidates);
#else
        HumanFaceDetectMSR01 s1(0.3F, 0.5F, 10, 0.2F);
        std::list<dl::detect::result_t> &results = s1.infer((uint16_t *)fb->buf, {(int)fb->height, (int)fb->width, 3});
#endif
        if (results.size() > 0) {
            fb_data_t rfb;
            rfb.width = fb->width;
            rfb.height = fb->height;
            rfb.data = fb->buf;
            rfb.bytes_per_pixel = 2;
            rfb.format = FB_RGB565;
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
            detected = true;
#endif
            draw_face_boxes(&rfb, &results, face_id);
        }
        s = fmt2jpg_cb(fb->buf, fb->len, fb->width, fb->height, PIXFORMAT_RGB565, 90, jpg_encode_stream, &jchunk);
        esp_camera_fb_return(fb);
    } else
    {
        out_len = fb->width * fb->height * 3;
        out_width = fb->width;
        out_height = fb->height;
        out_buf = (uint8_t*)malloc(out_len);
        if (!out_buf) {
            log_e("out_buf malloc failed");
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        s = fmt2rgb888(fb->buf, fb->len, fb->format, out_buf);
        esp_camera_fb_return(fb);
        if (!s) {
            free(out_buf);
            log_e("To rgb888 failed");
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }

        fb_data_t rfb;
        rfb.width = out_width;
        rfb.height = out_height;
        rfb.data = out_buf;
        rfb.bytes_per_pixel = 3;
        rfb.format = FB_BGR888;

#if TWO_STAGE
        HumanFaceDetectMSR01 s1(0.1F, 0.5F, 10, 0.2F);
        HumanFaceDetectMNP01 s2(0.5F, 0.3F, 5);
        std::list<dl::detect::result_t> &candidates = s1.infer((uint8_t *)out_buf, {(int)out_height, (int)out_width, 3});
        std::list<dl::detect::result_t> &results = s2.infer((uint8_t *)out_buf, {(int)out_height, (int)out_width, 3}, candidates);
#else
        HumanFaceDetectMSR01 s1(0.3F, 0.5F, 10, 0.2F);
        std::list<dl::detect::result_t> &results = s1.infer((uint8_t *)out_buf, {(int)out_height, (int)out_width, 3});
#endif

        if (results.size() > 0) {
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
            detected = true;
#endif
#if CONFIG_ESP_FACE_RECOGNITION_ENABLED
            if (recognition_enabled) {
                face_id = run_face_recognition(&rfb, &results);
            }
#endif
            draw_face_boxes(&rfb, &results, face_id);
        }

        s = fmt2jpg_cb(out_buf, out_len, out_width, out_height, PIXFORMAT_RGB888, 90, jpg_encode_stream, &jchunk);
        free(out_buf);
    }

    if (!s) {
        log_e("JPEG compression failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
    int64_t fr_end = esp_timer_get_time();
#endif
    log_i("FACE: %uB %ums %s%d", (uint32_t)(jchunk.len), (uint32_t)((fr_end - fr_start) / 1000), detected ? "DETECTED " : "", face_id);
    return res;
#endif
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    camera_fb_t *fb = NULL;
    struct timeval _timestamp;
    esp_err_t res = ESP_OK;
    size_t _jpg_buf_len = 0;
    uint8_t *_jpg_buf = NULL;
    char *part_buf[128];
#if CONFIG_ESP_FACE_DETECT_ENABLED
    #if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
        bool detected = false;
        int64_t fr_ready = 0;
        int64_t fr_recognize = 0;
        int64_t fr_encode = 0;
        int64_t fr_face = 0;
        int64_t fr_start = 0;
    #endif
    int face_id = 0;
    size_t out_len = 0, out_width = 0, out_height = 0;
    uint8_t *out_buf = NULL;
    bool s = false;
#if TWO_STAGE
    HumanFaceDetectMSR01 s1(0.1F, 0.5F, 10, 0.2F);
    HumanFaceDetectMNP01 s2(0.5F, 0.3F, 5);
#else
    HumanFaceDetectMSR01 s1(0.3F, 0.5F, 10, 0.2F);
#endif
#endif

    static int64_t last_frame = 0;
    if (!last_frame)
    {
        last_frame = esp_timer_get_time();
    }

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if (res != ESP_OK)
    {
        return res;
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "X-Framerate", "60");

#if CONFIG_LED_ILLUMINATOR_ENABLED
    isStreaming = true;
    enable_led(true);
#endif

    while (true)
    {
#if CONFIG_ESP_FACE_DETECT_ENABLED
    #if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
        detected = false;
    #endif
        face_id = 0;
#endif

        fb = esp_camera_fb_get();
        if (!fb)
        {
            log_e("Camera capture failed");
            res = ESP_FAIL;
        }
        else
        {
            _timestamp.tv_sec = fb->timestamp.tv_sec;
            _timestamp.tv_usec = fb->timestamp.tv_usec;
#if CONFIG_ESP_FACE_DETECT_ENABLED
    #if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
            fr_start = esp_timer_get_time();
            fr_ready = fr_start;
            fr_encode = fr_start;
            fr_recognize = fr_start;
            fr_face = fr_start;
    #endif
            if (!detection_enabled || fb->width > 400)
            {
#endif
                if (fb->format != PIXFORMAT_JPEG)
                {
                    bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
                    esp_camera_fb_return(fb);
                    fb = NULL;
                    if (!jpeg_converted)
                    {
                        log_e("JPEG compression failed");
                        res = ESP_FAIL;
                    }
                }
                else
                {
                    _jpg_buf_len = fb->len;
                    _jpg_buf = fb->buf;
                }
#if CONFIG_ESP_FACE_DETECT_ENABLED
            }
            else
            {
                if (fb->format == PIXFORMAT_RGB565
#if CONFIG_ESP_FACE_RECOGNITION_ENABLED
                    && !recognition_enabled
#endif
                ){
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
                    fr_ready = esp_timer_get_time();
#endif
#if TWO_STAGE
                    std::list<dl::detect::result_t> &candidates = s1.infer((uint16_t *)fb->buf, {(int)fb->height, (int)fb->width, 3});
                    std::list<dl::detect::result_t> &results = s2.infer((uint16_t *)fb->buf, {(int)fb->height, (int)fb->width, 3}, candidates);
#else
                    std::list<dl::detect::result_t> &results = s1.infer((uint16_t *)fb->buf, {(int)fb->height, (int)fb->width, 3});
#endif
#if CONFIG_ESP_FACE_DETECT_ENABLED && ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
                    fr_face = esp_timer_get_time();
                    fr_recognize = fr_face;
#endif
                    if (results.size() > 0) {
                        fb_data_t rfb;
                        rfb.width = fb->width;
                        rfb.height = fb->height;
                        rfb.data = fb->buf;
                        rfb.bytes_per_pixel = 2;
                        rfb.format = FB_RGB565;
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
                        detected = true;
#endif
                        draw_face_boxes(&rfb, &results, face_id);
                    }
                    s = fmt2jpg(fb->buf, fb->len, fb->width, fb->height, PIXFORMAT_RGB565, 80, &_jpg_buf, &_jpg_buf_len);
                    esp_camera_fb_return(fb);
                    fb = NULL;
                    if (!s) {
                        log_e("fmt2jpg failed");
                        res = ESP_FAIL;
                    }
#if CONFIG_ESP_FACE_DETECT_ENABLED && ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
                    fr_encode = esp_timer_get_time();
#endif
                } else
                {
                    out_len = fb->width * fb->height * 3;
                    out_width = fb->width;
                    out_height = fb->height;
                    out_buf = (uint8_t*)malloc(out_len);
                    if (!out_buf) {
                        log_e("out_buf malloc failed");
                        res = ESP_FAIL;
                    } else {
                        s = fmt2rgb888(fb->buf, fb->len, fb->format, out_buf);
                        esp_camera_fb_return(fb);
                        fb = NULL;
                        if (!s) {
                            free(out_buf);
                            log_e("To rgb888 failed");
                            res = ESP_FAIL;
                        } else {
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
                            fr_ready = esp_timer_get_time();
#endif

                            fb_data_t rfb;
                            rfb.width = out_width;
                            rfb.height = out_height;
                            rfb.data = out_buf;
                            rfb.bytes_per_pixel = 3;
                            rfb.format = FB_BGR888;

#if TWO_STAGE
                            std::list<dl::detect::result_t> &candidates = s1.infer((uint8_t *)out_buf, {(int)out_height, (int)out_width, 3});
                            std::list<dl::detect::result_t> &results = s2.infer((uint8_t *)out_buf, {(int)out_height, (int)out_width, 3}, candidates);
#else
                            std::list<dl::detect::result_t> &results = s1.infer((uint8_t *)out_buf, {(int)out_height, (int)out_width, 3});
#endif

#if CONFIG_ESP_FACE_DETECT_ENABLED && ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
                            fr_face = esp_timer_get_time();
                            fr_recognize = fr_face;
#endif

                            if (results.size() > 0) {
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
                                detected = true;
#endif
#if CONFIG_ESP_FACE_RECOGNITION_ENABLED
                                if (recognition_enabled) {
                                    face_id = run_face_recognition(&rfb, &results);
    #if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
                                    fr_recognize = esp_timer_get_time();
    #endif
                                }
#endif
                                draw_face_boxes(&rfb, &results, face_id);
                            }
                            s = fmt2jpg(out_buf, out_len, out_width, out_height, PIXFORMAT_RGB888, 90, &_jpg_buf, &_jpg_buf_len);
                            free(out_buf);
                            if (!s) {
                                log_e("fmt2jpg failed");
                                res = ESP_FAIL;
                            }
#if CONFIG_ESP_FACE_DETECT_ENABLED && ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
                            fr_encode = esp_timer_get_time();
#endif
                        }
                    }
                }
            }
#endif
        }
        if (res == ESP_OK)
        {
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        }
        if (res == ESP_OK)
        {
            size_t hlen = snprintf((char *)part_buf, 128, _STREAM_PART, _jpg_buf_len, _timestamp.tv_sec, _timestamp.tv_usec);
            res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
        }
        if (res == ESP_OK)
        {
            res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
        }
        if (fb)
        {
            esp_camera_fb_return(fb);
            fb = NULL;
            _jpg_buf = NULL;
        }
        else if (_jpg_buf)
        {
            free(_jpg_buf);
            _jpg_buf = NULL;
        }
        if (res != ESP_OK)
        {
            log_e("Send frame failed");
            break;
        }
        int64_t fr_end = esp_timer_get_time();

#if CONFIG_ESP_FACE_DETECT_ENABLED && ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
        int64_t ready_time = (fr_ready - fr_start) / 1000;
        int64_t face_time = (fr_face - fr_ready) / 1000;
        int64_t recognize_time = (fr_recognize - fr_face) / 1000;
        int64_t encode_time = (fr_encode - fr_recognize) / 1000;
        int64_t process_time = (fr_encode - fr_start) / 1000;
#endif

        int64_t frame_time = fr_end - last_frame;
        frame_time /= 1000;
#if ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO
        uint32_t avg_frame_time = ra_filter_run(&ra_filter, frame_time);
#endif
        log_i("MJPG: %uB %ums (%.1ffps), AVG: %ums (%.1ffps)"
#if CONFIG_ESP_FACE_DETECT_ENABLED
                      ", %u+%u+%u+%u=%u %s%d"
#endif
                 ,
                 (uint32_t)(_jpg_buf_len),
                 (uint32_t)frame_time, 1000.0 / (uint32_t)frame_time,
                 avg_frame_time, 1000.0 / avg_frame_time
#if CONFIG_ESP_FACE_DETECT_ENABLED
                 ,
                 (uint32_t)ready_time, (uint32_t)face_time, (uint32_t)recognize_time, (uint32_t)encode_time, (uint32_t)process_time,
                 (detected) ? "DETECTED " : "", face_id
#endif
        );
    }

#if CONFIG_LED_ILLUMINATOR_ENABLED
    isStreaming = false;
    enable_led(false);
#endif

    return res;
}

static esp_err_t parse_get(httpd_req_t *req, char **obuf)
{
    char *buf = NULL;
    size_t buf_len = 0;

    buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        buf = (char *)malloc(buf_len);
        if (!buf) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            *obuf = buf;
            return ESP_OK;
        }
        free(buf);
    }
    httpd_resp_send_404(req);
    return ESP_FAIL;
}

static esp_err_t cmd_handler(httpd_req_t *req)
{
    char *buf = NULL;
    char variable[32];
    char value[32];

    if (parse_get(req, &buf) != ESP_OK) {
        return ESP_FAIL;
    }
    if (httpd_query_key_value(buf, "var", variable, sizeof(variable)) != ESP_OK ||
        httpd_query_key_value(buf, "val", value, sizeof(value)) != ESP_OK) {
        free(buf);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    free(buf);

    int val = atoi(value);
    log_i("%s = %d", variable, val);
    sensor_t *s = esp_camera_sensor_get();
    int res = 0;

    if (!strcmp(variable, "framesize")) {
        if (s->pixformat == PIXFORMAT_JPEG) {
            res = s->set_framesize(s, (framesize_t)val);
        }
    }
    else if (!strcmp(variable, "quality"))
        res = s->set_quality(s, val);
    else if (!strcmp(variable, "contrast"))
        res = s->set_contrast(s, val);
    else if (!strcmp(variable, "brightness"))
        res = s->set_brightness(s, val);
    else if (!strcmp(variable, "saturation"))
        res = s->set_saturation(s, val);
    else if (!strcmp(variable, "gainceiling"))
        res = s->set_gainceiling(s, (gainceiling_t)val);
    else if (!strcmp(variable, "colorbar"))
        res = s->set_colorbar(s, val);
    else if (!strcmp(variable, "awb"))
        res = s->set_whitebal(s, val);
    else if (!strcmp(variable, "agc"))
        res = s->set_gain_ctrl(s, val);
    else if (!strcmp(variable, "aec"))
        res = s->set_exposure_ctrl(s, val);
    else if (!strcmp(variable, "hmirror"))
        res = s->set_hmirror(s, val);
    else if (!strcmp(variable, "vflip"))
        res = s->set_vflip(s, val);
    else if (!strcmp(variable, "awb_gain"))
        res = s->set_awb_gain(s, val);
    else if (!strcmp(variable, "agc_gain"))
        res = s->set_agc_gain(s, val);
    else if (!strcmp(variable, "aec_value"))
        res = s->set_aec_value(s, val);
    else if (!strcmp(variable, "aec2"))
        res = s->set_aec2(s, val);
    else if (!strcmp(variable, "dcw"))
        res = s->set_dcw(s, val);
    else if (!strcmp(variable, "bpc"))
        res = s->set_bpc(s, val);
    else if (!strcmp(variable, "wpc"))
        res = s->set_wpc(s, val);
    else if (!strcmp(variable, "raw_gma"))
        res = s->set_raw_gma(s, val);
    else if (!strcmp(variable, "lenc"))
        res = s->set_lenc(s, val);
    else if (!strcmp(variable, "special_effect"))
        res = s->set_special_effect(s, val);
    else if (!strcmp(variable, "wb_mode"))
        res = s->set_wb_mode(s, val);
    else if (!strcmp(variable, "ae_level"))
        res = s->set_ae_level(s, val);
#if CONFIG_LED_ILLUMINATOR_ENABLED
    else if (!strcmp(variable, "led_intensity")) {
        led_duty = val;
        if (isStreaming)
            enable_led(true);
    }
#endif

#if CONFIG_ESP_FACE_DETECT_ENABLED
    else if (!strcmp(variable, "face_detect")) {
        detection_enabled = val;
#if CONFIG_ESP_FACE_RECOGNITION_ENABLED
        if (!detection_enabled) {
            recognition_enabled = 0;
        }
#endif
    }
#if CONFIG_ESP_FACE_RECOGNITION_ENABLED
    else if (!strcmp(variable, "face_enroll")){
        is_enrolling = !is_enrolling;
        log_i("Enrolling: %s", is_enrolling?"true":"false");
    }
    else if (!strcmp(variable, "face_recognize")) {
        recognition_enabled = val;
        if (recognition_enabled) {
            detection_enabled = val;
        }
    }
#endif
#endif
    else {
        log_i("Unknown command: %s", variable);
        res = -1;
    }

    if (res < 0) {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, NULL, 0);
}

static int print_reg(char * p, sensor_t * s, uint16_t reg, uint32_t mask){
    return sprintf(p, "\"0x%x\":%u,", reg, s->get_reg(s, reg, mask));
}

static esp_err_t status_handler(httpd_req_t *req)
{
    static char json_response[1024];

    sensor_t *s = esp_camera_sensor_get();
    char *p = json_response;
    *p++ = '{';

    if(s->id.PID == OV5640_PID || s->id.PID == OV3660_PID){
        for(int reg = 0x3400; reg < 0x3406; reg+=2){
            p+=print_reg(p, s, reg, 0xFFF);//12 bit
        }
        p+=print_reg(p, s, 0x3406, 0xFF);

        p+=print_reg(p, s, 0x3500, 0xFFFF0);//16 bit
        p+=print_reg(p, s, 0x3503, 0xFF);
        p+=print_reg(p, s, 0x350a, 0x3FF);//10 bit
        p+=print_reg(p, s, 0x350c, 0xFFFF);//16 bit

        for(int reg = 0x5480; reg <= 0x5490; reg++){
            p+=print_reg(p, s, reg, 0xFF);
        }

        for(int reg = 0x5380; reg <= 0x538b; reg++){
            p+=print_reg(p, s, reg, 0xFF);
        }

        for(int reg = 0x5580; reg < 0x558a; reg++){
            p+=print_reg(p, s, reg, 0xFF);
        }
        p+=print_reg(p, s, 0x558a, 0x1FF);//9 bit
    } else if(s->id.PID == OV2640_PID){
        p+=print_reg(p, s, 0xd3, 0xFF);
        p+=print_reg(p, s, 0x111, 0xFF);
        p+=print_reg(p, s, 0x132, 0xFF);
    }

    p += sprintf(p, "\"xclk\":%u,", s->xclk_freq_hz / 1000000);
    p += sprintf(p, "\"pixformat\":%u,", s->pixformat);
    p += sprintf(p, "\"framesize\":%u,", s->status.framesize);
    p += sprintf(p, "\"quality\":%u,", s->status.quality);
    p += sprintf(p, "\"brightness\":%d,", s->status.brightness);
    p += sprintf(p, "\"contrast\":%d,", s->status.contrast);
    p += sprintf(p, "\"saturation\":%d,", s->status.saturation);
    p += sprintf(p, "\"sharpness\":%d,", s->status.sharpness);
    p += sprintf(p, "\"special_effect\":%u,", s->status.special_effect);
    p += sprintf(p, "\"wb_mode\":%u,", s->status.wb_mode);
    p += sprintf(p, "\"awb\":%u,", s->status.awb);
    p += sprintf(p, "\"awb_gain\":%u,", s->status.awb_gain);
    p += sprintf(p, "\"aec\":%u,", s->status.aec);
    p += sprintf(p, "\"aec2\":%u,", s->status.aec2);
    p += sprintf(p, "\"ae_level\":%d,", s->status.ae_level);
    p += sprintf(p, "\"aec_value\":%u,", s->status.aec_value);
    p += sprintf(p, "\"agc\":%u,", s->status.agc);
    p += sprintf(p, "\"agc_gain\":%u,", s->status.agc_gain);
    p += sprintf(p, "\"gainceiling\":%u,", s->status.gainceiling);
    p += sprintf(p, "\"bpc\":%u,", s->status.bpc);
    p += sprintf(p, "\"wpc\":%u,", s->status.wpc);
    p += sprintf(p, "\"raw_gma\":%u,", s->status.raw_gma);
    p += sprintf(p, "\"lenc\":%u,", s->status.lenc);
    p += sprintf(p, "\"hmirror\":%u,", s->status.hmirror);
    p += sprintf(p, "\"dcw\":%u,", s->status.dcw);
    p += sprintf(p, "\"colorbar\":%u", s->status.colorbar);
#if CONFIG_LED_ILLUMINATOR_ENABLED
    p += sprintf(p, ",\"led_intensity\":%u", led_duty);
#else
    p += sprintf(p, ",\"led_intensity\":%d", -1);
#endif
#if CONFIG_ESP_FACE_DETECT_ENABLED
    p += sprintf(p, ",\"face_detect\":%u", detection_enabled);
#if CONFIG_ESP_FACE_RECOGNITION_ENABLED
    p += sprintf(p, ",\"face_enroll\":%u,", is_enrolling);
    p += sprintf(p, "\"face_recognize\":%u", recognition_enabled);
#endif
#endif
    *p++ = '}';
    *p++ = 0;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json_response, strlen(json_response));
}

static esp_err_t xclk_handler(httpd_req_t *req)
{
    char *buf = NULL;
    char _xclk[32];

    if (parse_get(req, &buf) != ESP_OK) {
        return ESP_FAIL;
    }
    if (httpd_query_key_value(buf, "xclk", _xclk, sizeof(_xclk)) != ESP_OK) {
        free(buf);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    free(buf);

    int xclk = atoi(_xclk);
    log_i("Set XCLK: %d MHz", xclk);

    sensor_t *s = esp_camera_sensor_get();
    int res = s->set_xclk(s, LEDC_TIMER_0, xclk);
    if (res) {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t reg_handler(httpd_req_t *req)
{
    char *buf = NULL;
    char _reg[32];
    char _mask[32];
    char _val[32];

    if (parse_get(req, &buf) != ESP_OK) {
        return ESP_FAIL;
    }
    if (httpd_query_key_value(buf, "reg", _reg, sizeof(_reg)) != ESP_OK ||
        httpd_query_key_value(buf, "mask", _mask, sizeof(_mask)) != ESP_OK ||
        httpd_query_key_value(buf, "val", _val, sizeof(_val)) != ESP_OK) {
        free(buf);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    free(buf);

    int reg = atoi(_reg);
    int mask = atoi(_mask);
    int val = atoi(_val);
    log_i("Set Register: reg: 0x%02x, mask: 0x%02x, value: 0x%02x", reg, mask, val);

    sensor_t *s = esp_camera_sensor_get();
    int res = s->set_reg(s, reg, mask, val);
    if (res) {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t greg_handler(httpd_req_t *req)
{
    char *buf = NULL;
    char _reg[32];
    char _mask[32];

    if (parse_get(req, &buf) != ESP_OK) {
        return ESP_FAIL;
    }
    if (httpd_query_key_value(buf, "reg", _reg, sizeof(_reg)) != ESP_OK ||
        httpd_query_key_value(buf, "mask", _mask, sizeof(_mask)) != ESP_OK) {
        free(buf);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    free(buf);

    int reg = atoi(_reg);
    int mask = atoi(_mask);
    sensor_t *s = esp_camera_sensor_get();
    int res = s->get_reg(s, reg, mask);
    if (res < 0) {
        return httpd_resp_send_500(req);
    }
    log_i("Get Register: reg: 0x%02x, mask: 0x%02x, value: 0x%02x", reg, mask, res);

    char buffer[20];
    const char * val = itoa(res, buffer, 10);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, val, strlen(val));
}

static int parse_get_var(char *buf, const char * key, int def)
{
    char _int[16];
    if(httpd_query_key_value(buf, key, _int, sizeof(_int)) != ESP_OK){
        return def;
    }
    return atoi(_int);
}

static esp_err_t pll_handler(httpd_req_t *req)
{
    char *buf = NULL;

    if (parse_get(req, &buf) != ESP_OK) {
        return ESP_FAIL;
    }

    int bypass = parse_get_var(buf, "bypass", 0);
    int mul = parse_get_var(buf, "mul", 0);
    int sys = parse_get_var(buf, "sys", 0);
    int root = parse_get_var(buf, "root", 0);
    int pre = parse_get_var(buf, "pre", 0);
    int seld5 = parse_get_var(buf, "seld5", 0);
    int pclken = parse_get_var(buf, "pclken", 0);
    int pclk = parse_get_var(buf, "pclk", 0);
    free(buf);

    log_i("Set Pll: bypass: %d, mul: %d, sys: %d, root: %d, pre: %d, seld5: %d, pclken: %d, pclk: %d", bypass, mul, sys, root, pre, seld5, pclken, pclk);
    sensor_t *s = esp_camera_sensor_get();
    int res = s->set_pll(s, bypass, mul, sys, root, pre, seld5, pclken, pclk);
    if (res) {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t win_handler(httpd_req_t *req)
{
    char *buf = NULL;

    if (parse_get(req, &buf) != ESP_OK) {
        return ESP_FAIL;
    }

    int startX = parse_get_var(buf, "sx", 0);
    int startY = parse_get_var(buf, "sy", 0);
    int endX = parse_get_var(buf, "ex", 0);
    int endY = parse_get_var(buf, "ey", 0);
    int offsetX = parse_get_var(buf, "offx", 0);
    int offsetY = parse_get_var(buf, "offy", 0);
    int totalX = parse_get_var(buf, "tx", 0);
    int totalY = parse_get_var(buf, "ty", 0);
    int outputX = parse_get_var(buf, "ox", 0);
    int outputY = parse_get_var(buf, "oy", 0);
    bool scale = parse_get_var(buf, "scale", 0) == 1;
    bool binning = parse_get_var(buf, "binning", 0) == 1;
    free(buf);

    log_i("Set Window: Start: %d %d, End: %d %d, Offset: %d %d, Total: %d %d, Output: %d %d, Scale: %u, Binning: %u", startX, startY, endX, endY, offsetX, offsetY, totalX, totalY, outputX, outputY, scale, binning);
    sensor_t *s = esp_camera_sensor_get();
    int res = s->set_res_raw(s, startX, startY, endX, endY, offsetX, offsetY, totalX, totalY, outputX, outputY, scale, binning);
    if (res) {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, NULL, 0);
}

// ---------------------------------------------------------------------------
// Vision recognition handlers
// 视觉识别：网页控制台 + 状态查询 + 手动触发 + 参数配置
// 真正的识别逻辑在 vision.cpp，这里只做参数校验与转发
// ---------------------------------------------------------------------------

// 将任意文本转义为合法 JSON 字符串（引号、反斜杠、换行等）
static void vision_json_escape(const char *src, char *dst, size_t dstLen)
{
    if (dstLen == 0) return;
    size_t o = 0;
    for (const char *p = src; p && *p && o + 7 < dstLen; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
            case '"':  dst[o++] = '\\'; dst[o++] = '"';  break;
            case '\\': dst[o++] = '\\'; dst[o++] = '\\'; break;
            case '\n': dst[o++] = '\\'; dst[o++] = 'n';  break;
            case '\r': dst[o++] = '\\'; dst[o++] = 'r';  break;
            case '\t': dst[o++] = '\\'; dst[o++] = 't';  break;
            default:
                if (c < 0x20) {
                    o += snprintf(dst + o, dstLen - o, "\\u%04x", c);
                } else {
                    dst[o++] = (char)c;   // UTF-8 多字节原样透传
                }
        }
    }
    dst[o] = '\0';
}

// 百分号解码（httpd 的 httpd_query_key_value 不会自动解码查询参数，
// 直接把 %E7%94%A8... 当作文本发出去会明显降低大模型识别质量）。
// 就地修改，'+' 视为空格。
static void url_decode(char *s)
{
    if (!s) return;
    char *w = s;
    for (char *r = s; *r; r++) {
        if (*r == '+') {
            *w++ = ' ';
        } else if (*r == '%' && r[1] && r[2]) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = hex(r[1]);
            int lo = hex(r[2]);
            if (hi >= 0 && lo >= 0) {
                *w++ = (char)((hi << 4) | lo);
                r += 2;
            } else {
                *w++ = *r;
            }
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

static esp_err_t provisioning_page_handler(httpd_req_t *req)
{
    if (!isDeviceProvisioningMode()) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_send(req, "配置热点未启用", HTTPD_RESP_USE_STRLEN);
    }
    static const char kProvisioningHtml[] = R"HTML(<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-CAM 网络配置</title>
<style>
body{font-family:-apple-system,"Segoe UI","Microsoft YaHei",sans-serif;margin:0;padding:16px;background:#eef2f6;color:#22303c}
.card{max-width:620px;margin:24px auto;background:#fff;border-radius:12px;padding:22px;box-shadow:0 2px 10px #14283c18}
h1{font-size:21px;margin:0 0 8px}p{color:#6f7f8d;font-size:13px}.row{display:flex;gap:10px;align-items:center;margin:14px 0;flex-wrap:wrap}.row label{width:130px;color:#4a5b6b}
input[type=text],input[type=password]{flex:1;min-width:220px;padding:9px;border:1px solid #cfd9e2;border-radius:8px;font-size:15px}
input[type=checkbox]{width:18px;height:18px}button{background:#2f7de1;color:#fff;border:0;border-radius:8px;padding:10px 16px;font-size:14px;cursor:pointer}.hint{font-size:13px;color:#6f7f8d;margin-top:12px}.err{color:#d9534f}
</style></head><body><div class="card"><h1>ESP32-CAM 网络配置</h1>
<p>连接开放热点 ESP32-CAM-7 后访问本页。填写家庭 Wi-Fi 和 MCP 上位机 IP，保存后重启设备。</p>
<div class="row"><label for="ssid">Wi-Fi 名称</label><input id="ssid" type="text" maxlength="32" autocomplete="off"></div>
<div class="row"><label for="pwd">Wi-Fi 密码</label><input id="pwd" type="password" maxlength="64" placeholder="留空表示不修改" autocomplete="new-password"></div>
<div class="row"><label style="width:auto;display:flex;align-items:center;gap:8px"><input id="clearPwd" type="checkbox"> 清空密码（开放 Wi-Fi）</label></div>
<div class="row"><label for="mcpIp">MCP 上位机 IP</label><input id="mcpIp" type="text" maxlength="15" inputmode="decimal" placeholder="例如 192.168.1.100"></div>
<div class="row"><button onclick="saveConfig()">保存配置</button><button onclick="rebootDevice()">重启设备</button></div><div id="hint" class="hint">正在读取当前配置…</div>
</div><script>
const $=id=>document.getElementById(id);function show(text,error){$('hint').textContent=text;$('hint').className='hint'+(error?' err':'')}
function load(){fetch('/device/config',{cache:'no-store'}).then(async r=>{const v=await r.json();if(!r.ok)throw Error(v.error||'读取失败');$('ssid').value=v.ssid||'';$('mcpIp').value=v.mcp_ip||'';show('配置已读取；密码不会回显。',false)}).catch(e=>show(e.message,true))}
function saveConfig(){const payload={ssid:$('ssid').value,mcp_ip:$('mcpIp').value};if($('clearPwd').checked)payload.pwd='';else if($('pwd').value)payload.pwd=$('pwd').value;fetch('/device/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)}).then(async r=>{const v=await r.json();if(!r.ok)throw Error(v.error||'保存失败');$('pwd').value='';$('clearPwd').checked=false;show('配置已保存，请点击“重启设备”。',false)}).catch(e=>show(e.message,true))}
function rebootDevice(){show('正在请求重启…',false);fetch('/device/reboot',{method:'POST'}).then(async r=>{const v=await r.json();if(!r.ok)throw Error(v.error||'重启失败');show('设备即将重启，请等待其连接新 Wi-Fi。',false)}).catch(e=>show(e.message,true))}load();
</script></body></html>)HTML";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, kProvisioningHtml, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t vision_page_handler(httpd_req_t *req)
{
    // 只有「热点还开着 且 设备没连上外部 Wi-Fi」时才强制显示配置页。
    // 设备一旦连上外部 Wi-Fi，/vision 就应该正常显示拍照界面；
    // 否则配网阶段遗留的状态会让已联网设备一直卡在配置页。
    if (isDeviceProvisioningMode() && WiFi.status() != WL_CONNECTED) {
        return provisioning_page_handler(req);
    }
    static const char kVisionHtml[] = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>ESP32-CAM 视觉识别</title>
<style>
  * { box-sizing: border-box; }
  body { font-family: -apple-system, "Segoe UI", "Microsoft YaHei", sans-serif;
         margin: 0; padding: 16px; background: #eef2f6; color: #22303c; }
  .wrap { max-width: 780px; margin: 0 auto; }
  .card { background: #fff; border-radius: 12px; padding: 18px 20px;
          box-shadow: 0 2px 10px rgba(20,40,60,.08); margin-bottom: 16px; }
  h1 { font-size: 20px; margin: 0 0 4px; }
  h2 { font-size: 15px; margin: 0 0 12px; color: #4a5b6b; font-weight: 600; }
  .sub { color: #7a8a99; font-size: 13px; margin-bottom: 14px; }
  .row { display: flex; align-items: center; gap: 10px; margin: 10px 0; flex-wrap: wrap; }
  .row label { min-width: 96px; font-size: 14px; color: #4a5b6b; }
  button { background: #2f7de1; color: #fff; border: 0; border-radius: 8px;
           padding: 11px 18px; font-size: 15px; cursor: pointer; font-weight: 600; }
  button:hover:not(:disabled) { background: #2569c0; }
  button:disabled { background: #b8c4cf; cursor: not-allowed; }
  button.ghost { background: #eaf3ff; color: #2f7de1; }
  input[type=number] { width: 96px; padding: 8px 10px; font-size: 15px;
           border: 1px solid #cfd9e2; border-radius: 8px; }
  input[type=text], input[type=password] { width: min(360px, 100%); padding: 8px 10px; font-size: 15px;
           border: 1px solid #cfd9e2; border-radius: 8px; }
  input[type=checkbox] { width: 18px; height: 18px; }
  select { padding: 8px 10px; font-size: 15px; border: 1px solid #cfd9e2;
           border-radius: 8px; background: #fff; }
  .pill { font-size: 13px; padding: 4px 10px; border-radius: 999px;
          background: #eaf3ff; color: #2f7de1; font-weight: 600; }
  .pill.busy { background: #fff4e5; color: #c77700; }
  .stats { display: flex; gap: 18px; font-size: 13px; color: #7a8a99; flex-wrap: wrap; }
  #messages { max-height: 340px; overflow-y: auto; margin-top: 4px; }
  .msg { padding: 10px 12px; border-radius: 8px; margin-bottom: 10px;
         background: #f5f9fd; border-left: 3px solid #2f7de1;
         font-size: 14px; line-height: 1.5; white-space: pre-wrap;
         word-break: break-word; }
  .msg.err { background: #fdf4f4; border-left-color: #d9534f; }
  .msg .t { display: block; font-size: 11px; color: #95a5b3; margin-bottom: 5px; }
  .empty { color: #a3b1bd; font-size: 13px; padding: 10px 0; }
  .shotbox { background: #0f1720; border-radius: 10px; min-height: 120px;
             display: flex; align-items: center; justify-content: center;
             overflow: hidden; }
  .shotbox img { display: block; width: 100%; height: auto; max-height: 420px;
                 object-fit: contain; }
  .shotmeta { font-size: 12px; color: #95a5b3; margin-top: 8px; min-height: 16px; }
  input[type=range] { accent-color: #2f7de1; }
  .hint { font-size: 12px; color: #95a5b3; margin: 2px 0 10px; }
  .mcp-logs { max-height: 260px; overflow-y: auto; font-size: 12px;
              font-family: ui-monospace, Consolas, monospace; }
  .mcp-logs .l { padding: 5px 8px; border-bottom: 1px solid #eef2f6;
                 display: flex; gap: 8px; }
  .mcp-logs .l .tm { color: #a3b1bd; flex: none; }
  .mcp-logs .l .lv { flex: none; font-weight: 700; }
  .mcp-logs .l.warn .lv { color: #c77700; }
  .mcp-logs .l.error .lv { color: #d9534f; }
  .mcp-logs .l.info .lv { color: #2f7de1; }
  .mcp-logs .l .tx { word-break: break-all; white-space: pre-wrap; }
  .evt { display: flex; gap: 12px; padding: 10px 0;
         border-bottom: 1px solid #eef2f6; align-items: flex-start; }
  .evt:last-child { border-bottom: 0; }
  .evt img { width: 132px; height: 99px; object-fit: cover; border-radius: 8px;
             background: #0f1720; flex: none; }
  .evt .noimg { width: 132px; height: 99px; border-radius: 8px; flex: none;
                background: #eef2f6; color: #a3b1bd; font-size: 12px;
                display: flex; align-items: center; justify-content: center; }
  .evt .body { flex: 1; min-width: 0; }
  .evt .mt { font-size: 11px; color: #95a5b3; margin-bottom: 4px; }
  .evt .rp { font-size: 14px; line-height: 1.5; white-space: pre-wrap;
             word-break: break-word; }
  .evt .rp.err { color: #d9534f; }
  .evt .pq { font-size: 11px; color: #a3b1bd; margin-top: 4px;
             word-break: break-word; }
  .dot { display: inline-block; width: 9px; height: 9px; border-radius: 50%;
         background: #b8c4cf; margin-right: 6px; }
  .dot.on { background: #35b56a; }
</style>
</head>
<body>
<div class="wrap">
  <div class="card">
    <h1>视觉识别控制台</h1>
    <div class="sub">ESP32-CAM 抓拍上传大模型，支持手动与自动识别</div>
    <div class="sub" style="margin:0">本机地址即为本页面，不再提供摄像头主页</div>
  </div>

  <div class="card">
    <h2>手动识别</h2>
    <div class="row">
      <button id="btnTrigger" onclick="triggerOnce()">拍照并识别</button>
      <span id="statePill" class="pill">空闲</span>
    </div>
  </div>

  <div class="card">
    <h2>网络配置</h2>
    <div class="row"><label for="cfgSsid">Wi-Fi 名称</label><input type="text" id="cfgSsid" maxlength="32" autocomplete="off"></div>
    <div class="row"><label for="cfgPwd">Wi-Fi 密码</label><input type="password" id="cfgPwd" maxlength="64" placeholder="留空表示不修改" autocomplete="new-password"></div>
    <div class="row"><label for="cfgClearPwd">开放 Wi-Fi</label><input type="checkbox" id="cfgClearPwd"><span class="hint">勾选后清空已保存的 Wi-Fi 密码</span></div>
    <div class="row"><label for="cfgMcpIp">MCP 上位机 IP</label><input type="text" id="cfgMcpIp" maxlength="15" placeholder="例如 192.168.1.100" inputmode="decimal"></div>
    <div class="row"><button onclick="saveNetworkConfig()">保存网络配置</button><button class="ghost" onclick="rebootDevice()">重启应用配置</button></div>
    <div class="hint" id="networkConfigHint">修改后先保存，再重启 ESP32-CAM；密码不会回显。</div>
  </div>

  <div class="card">
    <h2>抓拍参数</h2>
    <div class="row">
      <label for="selSize">分辨率</label>
      <select id="selSize" onchange="pushCapture()"></select>
      <span class="hint" style="margin:0">越高越清晰，单帧越大上传越慢</span>
    </div>
    <div class="row">
      <label for="inpQuality">画质</label>
      <input type="range" id="inpQuality" min="4" max="24" step="1" value="6"
             oninput="qualityLabel()" onchange="pushCapture()" style="flex:1;max-width:260px">
      <span class="pill" id="qualityVal">6</span>
    </div>
    <div class="hint">数值越小越清晰、体积越大；建议 6-12。修改后立即生效。</div>
  </div>

  <div class="card">
    <h2>实时预览</h2>
    <div class="row">
      <button id="btnPreview" class="ghost" onclick="togglePreview()">开始预览</button>
      <button class="ghost" onclick="snapPreview()">单张预览</button>
      <label style="display:flex;align-items:center;gap:6px;font-size:14px;color:#4a5b6b">
        <input type="checkbox" id="chkPrevTorch"> 预览补光
      </label>
      <span class="pill" id="previewFps">-</span>
    </div>
    <div class="hint">预览只取帧不上传，用于手动调节镜头焦距；识别时会自动让路。</div>
    <div id="previewBox" class="shotbox" style="margin-top:8px">
      <div class="empty" id="previewEmpty">未开启预览</div>
      <img id="preview" alt="实时预览" style="display:none">
    </div>
  </div>

  <div class="card">
    <h2>自动识别</h2>
    <div class="row">
      <label for="chkAuto">启用自动</label>
      <input type="checkbox" id="chkAuto" onchange="pushConfig()">
    </div>
    <div class="row">
      <label for="inpInterval">间隔（秒）</label>
      <input type="number" id="inpInterval" min="0" max="3600" step="1" value="10">
      <button onclick="pushConfig()">应用</button>
    </div>
    <div class="row">
      <label for="inpTorch">抓拍补光</label>
      <input type="range" id="inpTorch" min="0" max="255" step="5" value="160"
             oninput="torchLabel()" onchange="pushConfig()" style="flex:1;max-width:260px">
      <span class="pill" id="torchVal">160</span>
    </div>
    <div class="hint">间隔设为 0 表示识别完成后立即开始下一轮；此为补光基准强度，环境越暗会自动追加，0 表示完全不补光。</div>
    <div class="stats">
      <span>成功：<b id="okCount">0</b></span>
      <span>失败：<b id="errCount">0</b></span>
      <span>间隔：<b id="curInterval">-</b></span>
    </div>
  </div>

  <div class="card">
    <h2>最新抓拍</h2>
    <div id="shotBox" class="shotbox">
      <div class="empty" id="shotEmpty">暂无图片，点击“拍照并识别”后在此显示</div>
      <img id="shot" alt="最新抓拍" style="display:none">
    </div>
    <div class="shotmeta" id="shotMeta"></div>
    <div class="stats" style="margin-top:6px">
      <span>环境亮度：<b id="lumaVal">-</b></span>
      <span>补光：<b id="torchState">-</b></span>
      <span>当前参数：<b id="paramVal">-</b></span>
    </div>
  </div>

  <div class="card">
    <h2>识别结果</h2>
    <div id="messages"><div class="empty">暂无消息</div></div>
  </div>

  <div class="card">
    <h2>小智 MCP 状态</h2>
    <div class="row" style="margin-top:0">
      <span class="pill" id="mcpStatePill"><span class="dot" id="mcpDot"></span>未知</span>
      <button class="ghost" onclick="mcpRefresh(true)">刷新</button>
    </div>
    <div class="stats">
      <span>端点：<b id="mcpEndpoint">-</b></span>
      <span>上报：<b id="mcpReports">0</b></span>
      <span>日志：<b id="mcpLogCount">0</b></span>
      <span>事件：<b id="mcpEventCount">0</b></span>
    </div>
    <div class="hint" style="margin-top:8px">PC 端 MCP 桥接每 5 秒上报一次；断开时下方日志会显示原因。</div>
    <div id="mcpLogs" class="mcp-logs"><div class="empty">暂无日志</div></div>
  </div>

  <div class="card">
    <h2>机器人抓拍与识别回复</h2>
    <div class="hint">小智通过 MCP 调用拍照时，这里显示对应的图片和识别回复（最多保留 5 条）。</div>
    <div id="mcpEvents"><div class="empty">暂无 MCP 抓拍事件</div></div>
  </div>
</div>

<script>
(function () {
  var lastSeq = 0;
  var started = false;
  var autoFocused = false;
  var torchFocused = false;
  var qualityFocused = false;
  var paramInit = false;

  var $ = function (id) { return document.getElementById(id); };

  // 分辨率档位：value 与固件端 framesize_t 枚举一致
  var SIZES = [
    { v: 5,  t: 'QVGA 320x240' },
    { v: 8,  t: 'VGA 640x480' },
    { v: 9,  t: 'SVGA 800x600' },
    { v: 10, t: 'XGA 1024x768' },
    { v: 11, t: 'HD 1280x720' },
    { v: 12, t: 'SXGA 1280x1024' },
    { v: 13, t: 'UXGA 1600x1200' }
  ];

  var selSize = $('selSize');
  (function buildSizes() {
    for (var i = 0; i < SIZES.length; i++) {
      var o = document.createElement('option');
      o.value = SIZES[i].v;
      o.textContent = SIZES[i].t;
      selSize.appendChild(o);
    }
  })();

  var inp = $('inpInterval');
  inp.addEventListener('focus', function () { autoFocused = true; });
  inp.addEventListener('blur', function () { autoFocused = false; });

  var torch = $('inpTorch');
  torch.addEventListener('focus', function () { torchFocused = true; });
  torch.addEventListener('blur', function () { torchFocused = false; });
  torch.addEventListener('input', function () { torchFocused = true; });

  var quality = $('inpQuality');
  quality.addEventListener('focus', function () { qualityFocused = true; });
  quality.addEventListener('blur', function () { qualityFocused = false; });
  quality.addEventListener('input', function () { qualityFocused = true; });

  function fmtTime(ms) {
    if (!ms) return '';
    var d = new Date(ms);
    var p = function (n) { return (n < 10 ? '0' : '') + n; };
    return p(d.getHours()) + ':' + p(d.getMinutes()) + ':' + p(d.getSeconds());
  }

  function addMessage(text, seq, isErr) {
    var box = $('messages');
    if (!started) { box.innerHTML = ''; started = true; }
    var div = document.createElement('div');
    div.className = 'msg' + (isErr ? ' err' : '');
    var t = document.createElement('span');
    t.className = 't';
    t.textContent = fmtTime(Date.now());
    div.appendChild(t);
    div.appendChild(document.createTextNode(text));
    box.insertBefore(div, box.firstChild);
    while (box.childNodes.length > 40) box.removeChild(box.lastChild);
  }

  window.torchLabel = function () { $('torchVal').textContent = torch.value; };
  window.qualityLabel = function () { $('qualityVal').textContent = quality.value; };

  function networkHint(text, error) {
    var hint = $('networkConfigHint');
    hint.textContent = text;
    hint.style.color = error ? '#d9534f' : '';
  }

  window.loadNetworkConfig = function () {
    fetch('/device/config', { cache: 'no-store' })
      .then(function (r) { return r.json().then(function (v) { if (!r.ok) throw new Error(v.error || '读取失败'); return v; }); })
      .then(function (v) {
        $('cfgSsid').value = v.ssid || '';
        $('cfgMcpIp').value = v.mcp_ip || '';
      })
      .catch(function (e) { networkHint(e.message, true); });
  };

  window.saveNetworkConfig = function () {
    var payload = { ssid: $('cfgSsid').value, mcp_ip: $('cfgMcpIp').value };
    var password = $('cfgPwd').value;
    if ($('cfgClearPwd').checked) payload.pwd = '';
    else if (password.length > 0) payload.pwd = password;
    fetch('/device/config', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload), cache: 'no-store' })
      .then(function (r) { return r.json().then(function (v) { if (!r.ok) throw new Error(v.error || '保存失败'); return v; }); })
      .then(function () { $('cfgPwd').value = ''; $('cfgClearPwd').checked = false; networkHint('配置已保存，请点击“重启应用配置”使 Wi-Fi 和 MCP 地址生效。', false); })
      .catch(function (e) { networkHint(e.message, true); });
  };

  window.rebootDevice = function () {
    networkHint('正在请求重启…', false);
    fetch('/device/reboot', { method: 'POST', cache: 'no-store' })
      .then(function (r) { if (!r.ok) throw new Error('重启请求失败，请重试'); networkHint('设备即将重启，请在串口查看新网页地址。', false); })
      .catch(function (e) { networkHint(e.message, true); });
  };

  function sizeText(v) {
    for (var i = 0; i < SIZES.length; i++) if (SIZES[i].v === v) return SIZES[i].t;
    return String(v);
  }

  var lastImageSeq = 0;

  function refreshImage(seq, bytes) {
    var img = $('shot');
    var ph = $('shotEmpty');
    if (!bytes) {
      img.style.display = 'none';
      ph.style.display = '';
      return;
    }
    ph.style.display = 'none';
    img.style.display = '';
    if (seq !== lastImageSeq) {
      lastImageSeq = seq;
      img.src = '/vision/image?t=' + Date.now();
      $('shotMeta').textContent = '抓拍序号 ' + seq + '，' +
          (bytes >= 1024 ? (bytes / 1024).toFixed(1) + ' KB' : bytes + ' 字节');
    }
  }

  function refresh() {
    fetch('/vision/status', { cache: 'no-store' })
      .then(function (r) { return r.json(); })
      .then(function (d) {
        var busy = !!d.isProcessing;
        $('statePill').textContent = busy ? '识别中…' : '空闲';
        $('statePill').className = 'pill' + (busy ? ' busy' : '');
        $('btnTrigger').disabled = busy;
        $('okCount').textContent = d.requestCount;
        $('errCount').textContent = d.errorCount;
        $('curInterval').textContent = Math.round(d.intervalMs / 1000) + ' 秒';

        if (document.activeElement !== $('chkAuto')) $('chkAuto').checked = !!d.autoEnabled;
        if (!autoFocused && document.activeElement !== inp) {
          inp.value = Math.round(d.intervalMs / 1000);
        }

        if (d.lastMessage && d.messageSeq && d.messageSeq !== lastSeq) {
          lastSeq = d.messageSeq;
          addMessage(d.lastMessage, d.messageSeq, false);
        }

        var td = (typeof d.torchDuty === 'number') ? d.torchDuty : 0;
        if (!torchFocused && document.activeElement !== torch) torch.value = td;
        $('torchVal').textContent = torch.value;

        if (typeof d.quality === 'number' && !qualityFocused && document.activeElement !== quality) {
          quality.value = d.quality;
        }
        $('qualityVal').textContent = quality.value;

        if (typeof d.framesize === 'number') {
          if (!paramInit) { selSize.value = d.framesize; paramInit = true; }
          else if (document.activeElement !== selSize) selSize.value = d.framesize;
        }
        $('paramVal').textContent = sizeText(parseInt(selSize.value, 10)) + ' / Q' + quality.value;

        if (typeof d.luma === 'number' && d.luma >= 0) {
          $('lumaVal').textContent = d.luma;
          $('torchState').textContent = (d.luma < 80 && td > 0) ? '开启' : '关闭';
        } else {
          $('lumaVal').textContent = '-';
          $('torchState').textContent = td > 0 ? '自动' : '关闭';
        }

        refreshImage(d.imageSeq || 0, d.imageBytes || 0);
      })
      .catch(function (e) { console.error('status failed', e); });
  }

  window.triggerOnce = function () {
    $('btnTrigger').disabled = true;
    fetch('/vision/trigger', { cache: 'no-store' })
      .then(function (r) { return r.json(); })
      .then(function (d) {
        if (!d.success) addMessage(d.error || '触发失败', 0, true);
      })
      .catch(function () { addMessage('网络请求失败', 0, true); })
      .finally(function () { setTimeout(refresh, 300); });
  };

  window.pushConfig = function () {
    var en = $('chkAuto').checked ? 1 : 0;
    var sec = parseInt(inp.value, 10);
    if (isNaN(sec) || sec < 0) sec = 0;
    if (sec > 3600) sec = 3600;
    var torchDuty = parseInt(torch.value, 10);
    if (isNaN(torchDuty)) torchDuty = 0;
    fetch('/vision/config?enabled=' + en + '&interval=' + (sec * 1000) +
          '&torch=' + torchDuty, { cache: 'no-store' })
      .then(function (r) { return r.json(); })
      .then(function () { refresh(); })
      .catch(function (e) { console.error('config failed', e); });
  };

  window.pushCapture = function () {
    var size = parseInt(selSize.value, 10);
    var q = parseInt(quality.value, 10);
    fetch('/vision/config?framesize=' + size + '&quality=' + q, { cache: 'no-store' })
      .then(function (r) { return r.json(); })
      .then(function () {
        paramInit = true;
        $('paramVal').textContent = sizeText(size) + ' / Q' + q;
      })
      .catch(function (e) { console.error('capture config failed', e); });
  };

  // ---- 实时预览：只取帧不上传，用于手动对焦 ----
  var previewTimer = null;
  var previewLast = 0;
  var previewBusy = false;

  function previewTick() {
    if (previewBusy || document.hidden) return;
    previewBusy = true;
    var now = Date.now();
    var img = new Image();
    img.onload = function () {
      $('preview').src = img.src;
      $('preview').style.display = '';
      $('previewEmpty').style.display = 'none';
      var dt = Date.now() - previewLast;
      previewLast = Date.now();
      $('previewFps').textContent = dt > 0 ? (1000 / dt).toFixed(1) + ' fps' : '-';
      previewBusy = false;
    };
    img.onerror = function () {
      $('previewFps').textContent = '取帧失败';
      previewBusy = false;
    };
    previewLast = now;
    img.src = '/vision/preview?t=' + now + '&torch=' + ($('chkPrevTorch').checked ? 1 : 0);
  }

  window.togglePreview = function () {
    if (previewTimer) {
      clearInterval(previewTimer);
      previewTimer = null;
      $('btnPreview').textContent = '开始预览';
      $('previewFps').textContent = '-';
    } else {
      previewTimer = setInterval(previewTick, 400);
      $('btnPreview').textContent = '停止预览';
      previewTick();
    }
  };

  window.snapPreview = function () {
    var img = new Image();
    img.onload = function () {
      $('preview').src = img.src;
      $('preview').style.display = '';
      $('previewEmpty').style.display = 'none';
    };
    img.src = '/vision/preview?t=' + Date.now() + '&torch=' + ($('chkPrevTorch').checked ? 1 : 0);
  };

  // ---- 小智 MCP 状态 / 日志 / 机器人抓拍事件 ----
  var mcpLastEventSeq = 0;
  var mcpLogInit = false;
  var mcpEvtInit = false;

  function fmtAgo(ms) {
    if (!ms) return '从未';
    var s = Math.round(ms / 1000);
    if (s < 60) return s + ' 秒前';
    if (s < 3600) return Math.round(s / 60) + ' 分钟前';
    return Math.round(s / 3600) + ' 小时前';
  }

  function fmtUptime(ms) {
    var s = Math.floor(ms / 1000);
    var h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60);
    return (h > 0 ? h + ' 小时 ' : '') + m + ' 分 ' + (s % 60) + ' 秒';
  }

  function escHtml(s) {
    return String(s == null ? '' : s)
      .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  }

  window.mcpRefresh = function (withDetail) {
    fetch('/mcp/status', { cache: 'no-store' })
      .then(function (r) { return r.json(); })
      .then(function (d) {
        var on = !!d.connected;
        $('mcpStatePill').innerHTML = '<span class="dot' + (on ? ' on' : '') + '"></span>' +
            (on ? '已连接' : '未连接');
        $('mcpStatePill').className = 'pill' + (on ? '' : ' busy');
        $('mcpEndpoint').textContent = d.endpoint ? d.endpoint : '-';
        $('mcpReports').textContent = d.reportCount + '（' + fmtAgo(d.lastReportAgoMs) + '）';
        $('mcpLogCount').textContent = d.logCount;
        $('mcpEventCount').textContent = d.eventCount;
      })
      .catch(function () {
        $('mcpStatePill').innerHTML = '<span class="dot"></span>设备离线';
        $('mcpStatePill').className = 'pill busy';
      });

    if (withDetail || !mcpLogInit) {
      mcpLogInit = true;
      fetch('/mcp/log', { cache: 'no-store' })
        .then(function (r) { return r.json(); })
        .then(function (list) {
          var box = $('mcpLogs');
          if (!list || !list.length) {
            box.innerHTML = '<div class="empty">暂无日志</div>';
            return;
          }
          var html = '';
          for (var i = 0; i < list.length; i++) {
            var it = list[i];
            var lv = (it.level || 'info').toLowerCase();
            html += '<div class="l ' + escHtml(lv) + '">' +
                      '<span class="tm">' + fmtUptime(it.uptimeMs) + '</span>' +
                      '<span class="lv">' + escHtml(lv) + '</span>' +
                      '<span class="tx">' + escHtml(it.text) + '</span>' +
                    '</div>';
          }
          box.innerHTML = html;
        })
        .catch(function () {});
    }

    if (withDetail || !mcpEvtInit) {
      fetch('/mcp/events', { cache: 'no-store' })
        .then(function (r) { return r.json(); })
        .then(function (list) {
          mcpEvtInit = true;
          var box = $('mcpEvents');
          if (!list || !list.length) {
            box.innerHTML = '<div class="empty">暂无 MCP 抓拍事件</div>';
            return;
          }
          var html = '';
          for (var i = 0; i < list.length; i++) {
            var e = list[i];
            var img = e.hasImage
              ? '<img src="/mcp/event_image?seq=' + e.seq + '" alt="机器人抓拍">'
              : '<div class="noimg">无图片</div>';
            html += '<div class="evt">' + img +
                      '<div class="body">' +
                        '<div class="mt">#' + e.seq + ' · ' + fmtUptime(e.uptimeMs) +
                          ' · ' + (e.success ? '成功' : '失败') + '</div>' +
                        '<div class="rp' + (e.success ? '' : ' err') + '">' +
                          escHtml(e.message) + '</div>' +
                        '<div class="pq">提示词：' + escHtml(e.prompt) + '</div>' +
                      '</div>' +
                    '</div>';
          }
          box.innerHTML = html;
        })
        .catch(function () {});
    }
  };

  // 轮询间隔放宽 + 页面不可见时完全停止：
  // 每次 /vision/status 都要走一趟丢包链路，问得太勤只会互相挤占带宽，
  // 反而让页面更新更慢。
  function pollTick() {
    if (document.hidden) return;
    refresh();
  }
  function pollMcpTick() {
    if (document.hidden) return;
    mcpRefresh(false);
  }

  setInterval(pollTick, 4000);
  setInterval(pollMcpTick, 8000);

  // 切回前台时立刻补一次，避免显示过期数据
  document.addEventListener('visibilitychange', function () {
    if (!document.hidden) { refresh(); mcpRefresh(false); }
  });

  refresh();
  mcpRefresh(true);
  loadNetworkConfig();
})();
</script>
</body>
</html>)HTML";

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, kVisionHtml, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t device_json_response(httpd_req_t* req, const char* status, const JsonDocument& document)
{
    char body[512];
    if (measureJson(document) >= sizeof(body)) return httpd_resp_send_500(req);
    const size_t length = serializeJson(document, body, sizeof(body));
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, length);
}

static esp_err_t device_error_response(httpd_req_t* req, const char* status, const char* message)
{
    JsonDocument document;
    document["success"] = false;
    document["error"] = message;
    return device_json_response(req, status, document);
}

static esp_err_t device_config_get_handler(httpd_req_t* req)
{
    const DeviceNetworkConfig config = getDeviceNetworkConfig();
    JsonDocument document;
    document["ssid"] = config.ssid;
    document["mcp_ip"] = config.mcpIp;
    document["pwdConfigured"] = config.pwd[0] != '\0';
    return device_json_response(req, "200 OK", document);
}

static esp_err_t device_config_post_handler(httpd_req_t* req)
{
    if (req->content_len <= 0 || req->content_len > 512) {
        return device_error_response(req, "400 Bad Request", "请求体必须为 1-512 字节的 JSON");
    }
    char contentType[64];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", contentType, sizeof(contentType)) != ESP_OK
        || strncmp(contentType, "application/json", 16) != 0
        || (contentType[16] != '\0' && contentType[16] != ';')) {
        return device_error_response(req, "400 Bad Request", "请使用 application/json 提交配置");
    }
    char body[513];
    size_t received = 0;
    while (received < req->content_len) {
        const int count = httpd_req_recv(req, body + received, req->content_len - received);
        if (count <= 0) {
            return device_error_response(req, "408 Request Timeout", "配置请求接收失败，请重试");
        }
        received += count;
    }
    body[received] = '\0';
    JsonDocument document;
    if (deserializeJson(document, body, received) || !document.is<JsonObject>()) {
        return device_error_response(req, "400 Bad Request", "配置 JSON 格式错误");
    }
    const char* names[] = {"ssid", "pwd", "mcp_ip"};
    bool hasParameter = false;
    for (const char* name : names) {
        if (document[name].isUnbound()) continue;
        hasParameter = true;
        if (!document[name].is<const char*>()) {
            return device_error_response(req, "400 Bad Request", "配置项必须为字符串");
        }
        const JsonString text = document[name].as<JsonString>();
        if (strlen(text.c_str()) != text.size() || !isValidDeviceNetworkParameter(name, text.c_str())) {
            return device_error_response(req, "400 Bad Request", "SSID 为 1-32 字节，密码为 0-64 字节，MCP IP 必须为有效 IPv4");
        }
    }
    if (!hasParameter) {
        return device_error_response(req, "400 Bad Request", "缺少 ssid、pwd 或 mcp_ip 配置项");
    }
    for (const char* name : names) {
        if (document[name].isUnbound()) continue;
        if (setDeviceNetworkParameter(name, document[name].as<const char*>()) != DeviceConfigResult::Saved) {
            return device_error_response(req, "503 Service Unavailable", "配置保存失败，部分参数可能已保存，请重新保存");
        }
    }
    JsonDocument response;
    response["success"] = true;
    response["rebootRequired"] = true;
    return device_json_response(req, "200 OK", response);
}

static esp_err_t device_reboot_handler(httpd_req_t* req)
{
    JsonDocument response;
    response["success"] = true;
    const esp_err_t result = device_json_response(req, "200 OK", response);
    if (result == ESP_OK) requestDeviceRestart();
    return result;
}

static uint8_t g_activeChannel(void)
{
    uint8_t primary = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&primary, &second);
    return primary;
}

// 不引入 Arduino WiFi 对象，直接用底层 API 取信号强度
static int g_rssi(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.rssi;
    return 0;
}

static esp_err_t vision_wifi_handler(httpd_req_t *req)
{
    // 现场排查用：可在运行时调整协议/带宽/省电/发射功率/固定速率，不必重新编译烧录。
    int protocol  = -1;
    int bandwidth = -1;
    int psMode    = -1;
    int txPower   = -1;
    int phyRate   = -1;

    char query[192];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char param[24];
        if (httpd_query_key_value(query, "protocol", param, sizeof(param)) == ESP_OK)  protocol  = atoi(param);
        if (httpd_query_key_value(query, "bandwidth", param, sizeof(param)) == ESP_OK) bandwidth = atoi(param);
        if (httpd_query_key_value(query, "ps", param, sizeof(param)) == ESP_OK)        psMode    = atoi(param);
        if (httpd_query_key_value(query, "txpower", param, sizeof(param)) == ESP_OK)   txPower   = atoi(param);
        if (httpd_query_key_value(query, "phy", param, sizeof(param)) == ESP_OK)       phyRate   = atoi(param);
    }

    visionTuneWifi(protocol, bandwidth, psMode, txPower, phyRate);

    char json[192];
    snprintf(json, sizeof(json),
        "{\"success\":true,\"rssi\":%d,\"channel\":%u}",
        g_rssi(), (unsigned)g_activeChannel());
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t vision_status_handler(httpd_req_t *req)
{
    VisionState st = visionGetState();

    char esc[VISION_MESSAGE_MAX * 2 + 8];
    vision_json_escape(st.lastMessage, esc, sizeof(esc));

    char sensorName[32] = "unknown";
    sensor_t *sn = esp_camera_sensor_get();
    if (sn) {
        camera_sensor_info_t *info = esp_camera_sensor_get_info(&sn->id);
        if (info && info->name) {
            snprintf(sensorName, sizeof(sensorName), "%s", info->name);
        } else {
            snprintf(sensorName, sizeof(sensorName), "PID 0x%04X", sn->id.PID);
        }
    }

    char json[VISION_MESSAGE_MAX * 2 + 320];
    snprintf(json, sizeof(json),
        "{\"autoEnabled\":%s,\"intervalMs\":%lu,\"isProcessing\":%s,"
        "\"requestCount\":%d,\"errorCount\":%d,\"messageSeq\":%lu,"
        "\"imageSeq\":%lu,\"imageBytes\":%u,\"torchDuty\":%u,\"luma\":%d,"
        "\"lastDurationMs\":%lu,\"lastSuccess\":%s,"
        "\"framesize\":%u,\"quality\":%u,\"sensor\":\"%s\","
        "\"rssi\":%d,\"channel\":%u,"
        "\"lastMessage\":\"%s\"}",
        st.autoEnabled ? "true" : "false",
        st.intervalMs,
        st.isProcessing ? "true" : "false",
        st.requestCount,
        st.errorCount,
        st.messageSeq,
        st.imageSeq,
        (unsigned)st.imageBytes,
        (unsigned)st.torchDuty,
        st.luma,
        st.lastDurationMs,
        st.lastSuccess ? "true" : "false",
        (unsigned)st.framesize,
        (unsigned)st.quality,
        sensorName,
        g_rssi(),
        (unsigned)g_activeChannel(),
        esc);

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t vision_image_handler(httpd_req_t *req)
{
    // 返回最近一次识别使用的抓拍图片（JPEG）
    const uint8_t *buf = NULL;
    size_t len = 0;

    if (!visionAcquireImage(&buf, &len)) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, "暂无抓拍图片", HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    esp_err_t res = httpd_resp_send(req, (const char *)buf, len);
    visionReleaseImage();
    return res;
}

static esp_err_t vision_preview_handler(httpd_req_t *req)
{
    // 只取一帧返回，不缓存也不上传；用于网页实时预览和手动对焦
    bool withTorch = false;
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char param[8];
        if (httpd_query_key_value(query, "torch", param, sizeof(param)) == ESP_OK) {
            withTorch = (atoi(param) != 0);
        }
    }

    uint8_t *buf = NULL;
    size_t len = 0;

    if (!visionPreviewSnapshot(&buf, &len, withTorch)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, "取帧失败", HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t res = httpd_resp_send(req, (const char *)buf, len);
    free(buf);
    return res;
}

// 截断复制，保证不会把 UTF-8 字符从中间切开（与 vision.cpp 内部实现同语义）
static void copyVisionMsg(char *out, size_t outLen, const char *src)
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

static esp_err_t vision_trigger_handler(httpd_req_t *req)
{
    // 只投递请求，真正的网络操作在 vision.cpp 的后台任务中执行
    bool accepted = visionTriggerManual();

    const char *json = accepted
        ? "{\"success\":true}"
        : "{\"success\":false,\"error\":\"当前正在识别，请稍后再试\"}";

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t vision_trigger_wait_handler(httpd_req_t *req)
{
    // 同步接口：阻塞等待本次识别完成，直接返回结果与设备侧耗时。
    // 主要给自动化测试与需要一步到位的调用方使用。
    char prompt[VISION_PROMPT_MAX];
    prompt[0] = '\0';
    bool fromMcp = false;
    char query[512];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "prompt", prompt, sizeof(prompt));
        url_decode(prompt);
        char src[16];
        if (httpd_query_key_value(query, "source", src, sizeof(src)) == ESP_OK) {
            url_decode(src);
            fromMcp = (strcmp(src, "mcp") == 0);
        }
    }

    bool success = false;
    unsigned long durationMs = 0;
    char msg[VISION_MESSAGE_MAX];
    msg[0] = '\0';

    // 先记下本次请求前的消息序号：客户端据此判断“这一轮的结果出来没有”
    unsigned long seqBefore = visionGetState().messageSeq;

    bool accepted = visionTriggerStart(prompt[0] ? prompt : NULL, fromMcp);
    bool finished = false;

    if (!accepted) {
        copyVisionMsg(msg, sizeof(msg), "设备忙：正在识别或已有排队请求，请稍后再试");
    } else {
        // 只等很短一段：出结果就直接返回，没出就放行 httpd 任务交给轮询
        finished = visionWaitDone(VISION_HTTP_WAIT_MS, &success, &durationMs, msg, sizeof(msg));
        if (!finished) {
            copyVisionMsg(msg, sizeof(msg), "识别进行中，请稍后查询 /vision/status");
        }
    }

    VisionState st = visionGetState();

    char esc[VISION_MESSAGE_MAX * 2 + 8];
    vision_json_escape(msg, esc, sizeof(esc));

    char json[VISION_MESSAGE_MAX * 2 + 224];
    snprintf(json, sizeof(json),
        "{\"accepted\":%s,\"finished\":%s,\"success\":%s,\"durationMs\":%lu,"
        "\"seqBefore\":%lu,\"isProcessing\":%s,\"message\":\"%s\"}",
        accepted ? "true" : "false",
        finished ? "true" : "false",
        success ? "true" : "false",
        durationMs,
        seqBefore,
        st.isProcessing ? "true" : "false",
        esc);

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t vision_config_handler(httpd_req_t *req)
{
    char query[160];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        VisionState st = visionGetState();
        bool enabled = st.autoEnabled;
        // 0 是合法间隔；未传 interval 时保留当前设置，避免把“未修改”误当成 0。
        unsigned long intervalMs = st.intervalMs;
        char param[32];

        if (httpd_query_key_value(query, "enabled", param, sizeof(param)) == ESP_OK) {
            enabled = (atoi(param) != 0);
        }
        if (httpd_query_key_value(query, "interval", param, sizeof(param)) == ESP_OK) {
            intervalMs = strtoul(param, NULL, 10);
        }
        if (httpd_query_key_value(query, "torch", param, sizeof(param)) == ESP_OK) {
            long duty = strtol(param, NULL, 10);
            if (duty < 0) duty = 0;
            if (duty > 255) duty = 255;
            visionSetTorch((uint8_t)duty);
        }
        if (httpd_query_key_value(query, "framesize", param, sizeof(param)) == ESP_OK) {
            long fs = strtol(param, NULL, 10);
            if (fs < 0) fs = 0;
            if (fs > 255) fs = 255;
            visionSetFramesize((uint8_t)fs);
        }
        if (httpd_query_key_value(query, "quality", param, sizeof(param)) == ESP_OK) {
            long q = strtol(param, NULL, 10);
            if (q < 0) q = 0;
            if (q > 255) q = 255;
            visionSetQuality((uint8_t)q);
        }

        visionSetAutoMode(enabled, intervalMs);
    }

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
}

// ---------------------------------------------------------------------------
// MCP（小智）桥接：状态上报、日志、机器人抓拍事件
// PC 端 xiaozhi_mcp_bridge.py 通过 /mcp/report 上报连接状态与日志；
// 小智触发的识别（/vision/trigger_wait?source=mcp）会记录成事件，
// 网页据此展示“机器人拍摄图片 + 对应识别回复”。
// ---------------------------------------------------------------------------

// 把请求的查询串或 POST 表单体读到 buf（GET 用查询串，POST 用 body）
static void mcp_read_params(httpd_req_t *req, char *buf, size_t bufLen)
{
    if (!buf || bufLen == 0) return;
    buf[0] = '\0';

    if (req->method == HTTP_POST || req->method == HTTP_PUT) {
        int remaining = req->content_len;
        size_t off = 0;
        while (remaining > 0 && off + 1 < bufLen) {
            int toRead = (int)(bufLen - 1 - off);
            if (toRead > remaining) toRead = remaining;
            int ret = httpd_req_recv(req, buf + off, toRead);
            if (ret <= 0) break;
            off += ret;
            remaining -= ret;
        }
        buf[off] = '\0';
        // 表单体里的 '+' 代表空格
        for (size_t i = 0; i < off; i++) if (buf[i] == '+') buf[i] = ' ';
    } else {
        httpd_req_get_url_query_str(req, buf, bufLen);
    }
}

// POST /mcp/report 或 GET /mcp/report?connected=1&endpoint=...&log=...&level=...
static esp_err_t mcp_report_handler(httpd_req_t *req)
{
    char params[512];
    mcp_read_params(req, params, sizeof(params));

    char value[192];
    char endpoint[MCP_ENDPOINT_MAX];
    endpoint[0] = '\0';

    if (httpd_query_key_value(params, "endpoint", endpoint, sizeof(endpoint)) != ESP_OK) {
        endpoint[0] = '\0';
    }
    url_decode(endpoint);

    if (httpd_query_key_value(params, "connected", value, sizeof(value)) == ESP_OK) {
        mcpSetConnected(atoi(value) != 0, endpoint);
    }

    if (httpd_query_key_value(params, "log", value, sizeof(value)) == ESP_OK && value[0]) {
        url_decode(value);
        char level[8];
        level[0] = '\0';
        httpd_query_key_value(params, "level", level, sizeof(level));
        url_decode(level);
        mcpAppendLog(level, value);
    }

    char json[192];
    snprintf(json, sizeof(json),
        "{\"success\":true,\"connected\":%s,\"reportCount\":%lu,\"logCount\":%u}",
        mcpIsConnected() ? "true" : "false",
        mcpReportCount(), (unsigned)mcpLogCount());

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

// GET /mcp/status
static esp_err_t mcp_status_handler(httpd_req_t *req)
{
    char endpoint[MCP_ENDPOINT_MAX * 2 + 8];
    char raw[MCP_ENDPOINT_MAX];
    mcpGetEndpoint(raw, sizeof(raw));
    vision_json_escape(raw, endpoint, sizeof(endpoint));

    unsigned long lastReport = mcpLastReportMs();
    unsigned long agoMs = lastReport ? (millis() - lastReport) : 0;

    char json[MCP_ENDPOINT_MAX * 2 + 320];
    snprintf(json, sizeof(json),
        "{\"connected\":%s,\"endpoint\":\"%s\",\"lastReportAgoMs\":%lu,"
        "\"reportCount\":%lu,\"uptimeMs\":%lu,\"logCount\":%u,"
        "\"eventCount\":%u,\"lastEventSeq\":%lu}",
        mcpIsConnected() ? "true" : "false",
        endpoint,
        agoMs,
        mcpReportCount(),
        mcpUptimeMs(),
        (unsigned)mcpLogCount(),
        (unsigned)mcpStoredEventCount(),
        mcpLastEventSeq());

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

// GET /mcp/log?limit=N  —— 分块输出，避免一次性占用大块栈
static esp_err_t mcp_log_handler(httpd_req_t *req)
{
    size_t limit = MCP_LOG_MAX;
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char param[16];
        if (httpd_query_key_value(query, "limit", param, sizeof(param)) == ESP_OK) {
            long v = strtol(param, NULL, 10);
            if (v > 0 && v <= MCP_LOG_MAX) limit = (size_t)v;
        }
    }

    static McpLogEntry logs[MCP_LOG_MAX];
    size_t n = mcpGetLogs(logs, limit);

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    esp_err_t res = httpd_resp_send_chunk(req, "[", 1);
    char esc[MCP_LOG_TEXT_MAX * 2 + 8];
    for (size_t i = 0; res == ESP_OK && i < n; i++) {
        vision_json_escape(logs[i].text, esc, sizeof(esc));
        char item[MCP_LOG_TEXT_MAX * 2 + 96];
        int len = snprintf(item, sizeof(item),
            "%s{\"seq\":%lu,\"uptimeMs\":%lu,\"level\":\"%s\",\"text\":\"%s\"}",
            i ? "," : "", logs[i].seq, logs[i].uptimeMs, logs[i].level, esc);
        res = httpd_resp_send_chunk(req, item, len);
    }
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, "]", 1);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, NULL, 0);
    return res;
}

// GET /mcp/events —— 机器人抓拍事件列表（不含图片本体，图片走 /mcp/event_image）
static esp_err_t mcp_events_handler(httpd_req_t *req)
{
    static McpEventInfo events[MCP_EVENT_MAX];
    size_t n = mcpGetEvents(events, MCP_EVENT_MAX);

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    esp_err_t res = httpd_resp_send_chunk(req, "[", 1);
    char escPrompt[VISION_PROMPT_MAX * 2 + 8];
    char escMsg[VISION_MESSAGE_MAX * 2 + 8];
    for (size_t i = 0; res == ESP_OK && i < n; i++) {
        vision_json_escape(events[i].prompt, escPrompt, sizeof(escPrompt));
        vision_json_escape(events[i].message, escMsg, sizeof(escMsg));
        char item[VISION_PROMPT_MAX * 2 + VISION_MESSAGE_MAX * 2 + 160];
        int len = snprintf(item, sizeof(item),
            "%s{\"seq\":%lu,\"uptimeMs\":%lu,\"success\":%s,\"hasImage\":%s,"
            "\"imageBytes\":%u,\"prompt\":\"%s\",\"message\":\"%s\"}",
            i ? "," : "", events[i].seq, events[i].uptimeMs,
            events[i].success ? "true" : "false",
            events[i].imageBytes ? "true" : "false",
            (unsigned)events[i].imageBytes, escPrompt, escMsg);
        res = httpd_resp_send_chunk(req, item, len);
    }
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, "]", 1);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, NULL, 0);
    return res;
}

// GET /mcp/event_image?seq=N
static esp_err_t mcp_event_image_handler(httpd_req_t *req)
{
    unsigned long seq = 0;
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char param[24];
        if (httpd_query_key_value(query, "seq", param, sizeof(param)) == ESP_OK) {
            seq = strtoul(param, NULL, 10);
        }
    }

    uint8_t *buf = NULL;
    size_t len = 0;
    if (seq == 0 || !mcpCopyEventImage(seq, &buf, &len)) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, "该事件没有缓存图片", HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t res = httpd_resp_send(req, (const char *)buf, len);
    free(buf);
    return res;
}

void startCameraServer()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 24;   // vision + mcp 相关接口
    config.stack_size = 8192;       // 视觉接口需要较大栈，默认 4096 偏小

    // 现场实测（手机热点当 AP，丢包 6%~24%，RTT 抖动可达 500ms）：
    // 默认只开 7 个 socket，很容易被“浏览器多连接 + MCP 上报 + 识别”占满，
    // 占满后新请求只能堆在 backlog 里干等，表现为网页转圈、MCP 显示未连接。
    config.max_open_sockets   = 10;   // LWIP 共 16 个 socket，留余量给 TLS 等
    config.lru_purge_enable   = true; // 满了就回收最久未用的连接，避免彻底卡死
    config.recv_wait_timeout  = 10;   // 默认 5 秒在丢包链路上偏短
    config.send_wait_timeout  = 10;   // 大页面要分十几段发，给足重传时间












    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = vision_page_handler,
        .user_ctx = NULL
    };

    httpd_uri_t config_page_uri = {
        .uri = "/config",
        .method = HTTP_GET,
        .handler = provisioning_page_handler,
        .user_ctx = NULL
    };

    httpd_uri_t vision_uri = {
        .uri = "/vision",
        .method = HTTP_GET,
        .handler = vision_page_handler,
        .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
        ,
        .is_websocket = true,
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL
#endif
    };

    httpd_uri_t vision_status_uri = {
        .uri = "/vision/status",
        .method = HTTP_GET,
        .handler = vision_status_handler,
        .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
        ,
        .is_websocket = true,
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL
#endif
    };

    httpd_uri_t vision_image_uri = {
        .uri = "/vision/image",
        .method = HTTP_GET,
        .handler = vision_image_handler,
        .user_ctx = NULL
    };

    httpd_uri_t vision_preview_uri = {
        .uri = "/vision/preview",
        .method = HTTP_GET,
        .handler = vision_preview_handler,
        .user_ctx = NULL
    };

    httpd_uri_t vision_trigger_uri = {
        .uri = "/vision/trigger",
        .method = HTTP_GET,
        .handler = vision_trigger_handler,
        .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
        ,
        .is_websocket = true,
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL
#endif
    };

    httpd_uri_t vision_trigger_wait_uri = {
        .uri = "/vision/trigger_wait",
        .method = HTTP_GET,
        .handler = vision_trigger_wait_handler,
        .user_ctx = NULL
    };

    httpd_uri_t vision_wifi_uri = {
        .uri = "/vision/wifi",
        .method = HTTP_GET,
        .handler = vision_wifi_handler,
        .user_ctx = NULL
    };

    httpd_uri_t device_config_get_uri = {
        .uri = "/device/config",
        .method = HTTP_GET,
        .handler = device_config_get_handler,
        .user_ctx = NULL
    };

    httpd_uri_t device_config_post_uri = {
        .uri = "/device/config",
        .method = HTTP_POST,
        .handler = device_config_post_handler,
        .user_ctx = NULL
    };

    httpd_uri_t device_reboot_uri = {
        .uri = "/device/reboot",
        .method = HTTP_POST,
        .handler = device_reboot_handler,
        .user_ctx = NULL
    };

    httpd_uri_t mcp_report_uri = {
        .uri = "/mcp/report",
        .method = HTTP_POST,
        .handler = mcp_report_handler,
        .user_ctx = NULL
    };

    httpd_uri_t mcp_report_get_uri = {
        .uri = "/mcp/report",
        .method = HTTP_GET,
        .handler = mcp_report_handler,
        .user_ctx = NULL
    };

    httpd_uri_t mcp_status_uri = {
        .uri = "/mcp/status",
        .method = HTTP_GET,
        .handler = mcp_status_handler,
        .user_ctx = NULL
    };

    httpd_uri_t mcp_log_uri = {
        .uri = "/mcp/log",
        .method = HTTP_GET,
        .handler = mcp_log_handler,
        .user_ctx = NULL
    };

    httpd_uri_t mcp_events_uri = {
        .uri = "/mcp/events",
        .method = HTTP_GET,
        .handler = mcp_events_handler,
        .user_ctx = NULL
    };

    httpd_uri_t mcp_event_image_uri = {
        .uri = "/mcp/event_image",
        .method = HTTP_GET,
        .handler = mcp_event_image_handler,
        .user_ctx = NULL
    };

    httpd_uri_t vision_config_uri = {
        .uri = "/vision/config",
        .method = HTTP_GET,
        .handler = vision_config_handler,
        .user_ctx = NULL
#ifdef CONFIG_HTTPD_WS_SUPPORT
        ,
        .is_websocket = true,
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL
#endif
    };

    ra_filter_init(&ra_filter, 20);

#if CONFIG_ESP_FACE_RECOGNITION_ENABLED
    recognizer.set_partition(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "fr");

    // load ids from flash partition
    recognizer.set_ids_from_flash();
#endif
    log_i("Starting web server on port: '%d'", config.server_port);
    if (httpd_start(&camera_httpd, &config) == ESP_OK)
    {
        // 只注册视觉识别相关接口，不再启动镜像流服务
        httpd_register_uri_handler(camera_httpd, &root_uri);
        httpd_register_uri_handler(camera_httpd, &config_page_uri);
        httpd_register_uri_handler(camera_httpd, &vision_uri);
        httpd_register_uri_handler(camera_httpd, &vision_status_uri);
        httpd_register_uri_handler(camera_httpd, &vision_image_uri);
        httpd_register_uri_handler(camera_httpd, &vision_preview_uri);
        httpd_register_uri_handler(camera_httpd, &vision_trigger_uri);
        httpd_register_uri_handler(camera_httpd, &vision_trigger_wait_uri);
        httpd_register_uri_handler(camera_httpd, &vision_config_uri);
        httpd_register_uri_handler(camera_httpd, &vision_wifi_uri);
        httpd_register_uri_handler(camera_httpd, &device_config_get_uri);
        httpd_register_uri_handler(camera_httpd, &device_config_post_uri);
        httpd_register_uri_handler(camera_httpd, &device_reboot_uri);
        httpd_register_uri_handler(camera_httpd, &mcp_report_uri);
        httpd_register_uri_handler(camera_httpd, &mcp_report_get_uri);
        httpd_register_uri_handler(camera_httpd, &mcp_status_uri);
        httpd_register_uri_handler(camera_httpd, &mcp_log_uri);
        httpd_register_uri_handler(camera_httpd, &mcp_events_uri);
        httpd_register_uri_handler(camera_httpd, &mcp_event_image_uri);
    }
}

void setupLedFlash(int pin)
{
    #if CONFIG_LED_ILLUMINATOR_ENABLED
    ledcSetup(LED_LEDC_CHANNEL, 5000, 8);
    ledcAttachPin(pin, LED_LEDC_CHANNEL);
    #else
    log_i("LED flash is disabled -> CONFIG_LED_ILLUMINATOR_ENABLED = 0");
    #endif
}
