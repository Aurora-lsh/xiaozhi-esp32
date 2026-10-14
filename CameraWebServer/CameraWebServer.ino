#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include "esp_wifi.h"
#include "vision.h"
#include "mcp_status.h"
#include "mcp_registration.h"
#include "mcp_ip_validation.h"
#include "device_config.h"
#include "wifi_provisioning.h"
#include <mutex>

//
// WARNING!!! PSRAM IC required for UXGA resolution and high JPEG quality
//            Ensure ESP32 Wrover Module or other board with PSRAM is selected
//            Partial images will be transmitted if image exceeds buffer size
//
//            You must select partition scheme from the board menu that has at least 3MB APP space.
//            Face Recognition is DISABLED for ESP32 and ESP32-S2, because it takes up from 15 
//            seconds to process single frame. Face Detection is ENABLED if PSRAM is enabled as well

// ===================
// Select camera model
// ===================
//#define CAMERA_MODEL_WROVER_KIT // Has PSRAM
// #define CAMERA_MODEL_ESP_EYE // Has PSRAM
//#define CAMERA_MODEL_ESP32S3_EYE // Has PSRAM
//#define CAMERA_MODEL_M5STACK_PSRAM // Has PSRAM
//#define CAMERA_MODEL_M5STACK_V2_PSRAM // M5Camera version B Has PSRAM
//#define CAMERA_MODEL_M5STACK_WIDE // Has PSRAM
//#define CAMERA_MODEL_M5STACK_ESP32CAM // No PSRAM
//#define CAMERA_MODEL_M5STACK_UNITCAM // No PSRAM
#define CAMERA_MODEL_AI_THINKER // Has PSRAM
//#define CAMERA_MODEL_TTGO_T_JOURNAL // No PSRAM
//#define CAMERA_MODEL_XIAO_ESP32S3 // Has PSRAM
// ** Espressif Internal Boards **
//#define CAMERA_MODEL_ESP32_CAM_BOARD
//#define CAMERA_MODEL_ESP32S2_CAM_BOARD
//#define CAMERA_MODEL_ESP32S3_CAM_LCD
//#define CAMERA_MODEL_DFRobot_FireBeetle2_ESP32S3 // Has PSRAM
//#define CAMERA_MODEL_DFRobot_Romeo_ESP32S3 // Has PSRAM
#include "camera_pins.h"

// Defaults used only when no serial configuration has been saved.
static const char* DEFAULT_WIFI_SSID = "123";
static const char* DEFAULT_WIFI_PASSWORD = "s72ccjiy";
static const char* WIFI_PREFS_NAMESPACE = "wifi";

static String wifiSsid;
static String wifiPassword;
static String mcpBridgeIp;
static bool cameraReady = false;
static bool httpServerStarted = false;
static bool mcpStatusStarted = false;
static bool networkServicesStarted = false;
static bool registrationStarted = false;
static bool startupCommandsPrinted = false;
static bool provisioningMode = false;
static bool provisioningManual = false;
static std::mutex deviceConfigMutex;
static bool restartRequested = false;
static unsigned long restartRequestedAt = 0;

bool isDeviceProvisioningMode()
{
  return provisioningMode;
}

static void loadWifiCredentials()
{
  Preferences preferences;
  if (preferences.begin(WIFI_PREFS_NAMESPACE, true)) {
    wifiSsid = preferences.getString("ssid", DEFAULT_WIFI_SSID);
    wifiPassword = preferences.getString("pwd", DEFAULT_WIFI_PASSWORD);
    mcpBridgeIp = preferences.getString("bridge_ip", mcpDefaultBridgeIp());
    preferences.end();
  } else {
    wifiSsid = DEFAULT_WIFI_SSID;
    wifiPassword = DEFAULT_WIFI_PASSWORD;
    mcpBridgeIp = mcpDefaultBridgeIp();
  }
  if (!isValidBridgeIp(mcpBridgeIp.c_str())) mcpBridgeIp = mcpDefaultBridgeIp();
}

static bool saveWifiCredential(const char* key, const String& value)
{
  Preferences preferences;
  if (!preferences.begin(WIFI_PREFS_NAMESPACE, false)) return false;

  // Skip flash writes only if this exact value has already been stored.
  if (preferences.isKey(key) && preferences.getString(key) == value) {
    preferences.end();
    return true;
  }

  size_t written = preferences.putString(key, value);
  // Core 2.0.14 returns zero for BOTH an empty string and a failed write.
  bool saved = value.length() > 0
      ? written == value.length()
      : preferences.isKey(key) && preferences.getString(key, "!") == value;
  preferences.end();
  return saved;
}

static void printWifiCredential(const char* name, const String& value)
{
  Serial.printf("WIFI: %s=%s\n", name, value.c_str());
}

DeviceNetworkConfig getDeviceNetworkConfig()
{
  std::lock_guard<std::mutex> guard(deviceConfigMutex);
  DeviceNetworkConfig config = {};
  snprintf(config.ssid, sizeof(config.ssid), "%s", wifiSsid.c_str());
  snprintf(config.pwd, sizeof(config.pwd), "%s", wifiPassword.c_str());
  snprintf(config.mcpIp, sizeof(config.mcpIp), "%s", mcpBridgeIp.c_str());
  return config;
}

bool isValidDeviceNetworkParameter(const char* name, const char* value)
{
  if (name == nullptr || value == nullptr) return false;
  const size_t length = strlen(value);
  const bool ssid = strcmp(name, "ssid") == 0;
  const bool pwd = strcmp(name, "pwd") == 0;
  const bool bridge = strcmp(name, "mcp_ip") == 0;
  if ((!ssid && !pwd && !bridge) || (ssid && (length == 0 || length > 32))
      || (pwd && length > 64) || (bridge && !isValidBridgeIp(value))) {
    return false;
  }
  return true;
}

DeviceConfigResult setDeviceNetworkParameter(const char* name, const char* value)
{
  if (!isValidDeviceNetworkParameter(name, value)) return DeviceConfigResult::Invalid;
  const bool ssid = strcmp(name, "ssid") == 0;
  const bool pwd = strcmp(name, "pwd") == 0;
  const bool bridge = strcmp(name, "mcp_ip") == 0;
  std::lock_guard<std::mutex> guard(deviceConfigMutex);
  const String next = value;
  if (!saveWifiCredential(bridge ? "bridge_ip" : name, next)) return DeviceConfigResult::StorageFailure;
  if (ssid) wifiSsid = next;
  if (pwd) wifiPassword = next;
  if (bridge) mcpBridgeIp = next;
  return DeviceConfigResult::Saved;
}

void requestDeviceRestart()
{
  std::lock_guard<std::mutex> guard(deviceConfigMutex);
  if (!restartRequested) {
    restartRequestedAt = millis();
    restartRequested = true;
  }
}

static void processDeviceRestart()
{
  bool restart = false;
  {
    std::lock_guard<std::mutex> guard(deviceConfigMutex);
    restart = restartRequested && millis() - restartRequestedAt >= 1000;
  }
  if (restart) ESP.restart();
}

void startCameraServer();
void setupLedFlash(int pin);

static void printSerialCommands()
{
  Serial.println("串口命令（115200 波特率，每行一条）：");
  Serial.println("ssid=<Wi-Fi名称>  设置并保存 Wi-Fi 名称（1-32 字节）");
  Serial.println("pwd=<Wi-Fi密码>  设置并保存密码（0-64 字节；pwd= 表示开放网络）");
  Serial.println("mcp_ip=<桥接服务IPv4>  保存 MCP 桥接服务地址，重启后生效");
  Serial.println("ssid? / pwd? / mcp_ip?  查询已保存配置（pwd? 会明文显示密码）");
  Serial.println("v  单次视觉识别；s  查看识别状态；w  无线诊断；d  网络分段诊断");
  Serial.println("run<N>  连续识别 1-50 次；ps<N>  设置 Wi-Fi 省电模式");
  Serial.println("tx<N>  设置 Wi-Fi 发射功率；proto<N>  设置 Wi-Fi 协议；bw<N>  设置带宽");
  Serial.println("wifi_config  手动开启 ESP32-CAM-7 临时配置热点；reboot  重启并应用设置");
  Serial.println("help  再次显示本命令列表");
}

static void printStartupSerialCommands()
{
  if (startupCommandsPrinted) return;
  startupCommandsPrinted = true;
  printSerialCommands();
}

static void startHttpServerIfNeeded()
{
  if (!mcpStatusStarted) {
    mcpStatusInit();
    mcpStatusStarted = true;
  }
  if (!httpServerStarted) {
    startCameraServer();
    httpServerStarted = true;
  }
}

static void startNetworkServicesIfReady()
{
  if (!cameraReady || WiFi.status() != WL_CONNECTED) return;
  if (provisioningMode && !provisioningManual) {
    WiFi.softAPdisconnect(true);
    provisioningMode = false;
    Serial.println("WiFi config hotspot stopped after station connection");
  }
  startHttpServerIfNeeded();
  if (!networkServicesStarted) {
    networkServicesStarted = true;
    visionInit();
  }
  if (registrationStarted) return;
  registrationStarted = true;
  startMcpRegistration(String(getDeviceNetworkConfig().mcpIp));
  Serial.printf("Camera Ready! Use 'http://%s' to connect\n", WiFi.localIP().toString().c_str());
  Serial.printf("Vision Recognition Ready! Use 'http://%s/vision' to control\n", WiFi.localIP().toString().c_str());
  DeviceNetworkConfig network = getDeviceNetworkConfig();
  Serial.printf("ESP32-CAM网页服务地址: http://%s/vision\n", WiFi.localIP().toString().c_str());
  Serial.printf("MCP上位机服务IP: %s:%u\n", network.mcpIp, (unsigned)mcpBridgePort());
  printStartupSerialCommands();
}

static void startWifiConfigPortal(bool manual = false);

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();
  loadWifiCredentials();
  Serial.println("WiFi startup: waiting for network initialization...");

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size = FRAMESIZE_UXGA;
  config.pixel_format = PIXFORMAT_JPEG; // for streaming
  //config.pixel_format = PIXFORMAT_RGB565; // for face detection/recognition
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count = 1;
  
  // if PSRAM IC present, init with UXGA resolution and higher JPEG quality
  //                      for larger pre-allocated frame buffer.
  if(config.pixel_format == PIXFORMAT_JPEG){
    if(psramFound()){
      // 保留连续抓帧模式：按需抓帧（GRAB_WHEN_EMPTY）在暗光下要等自动曝光
      // 收敛，实测单帧要 ~2 秒，5 次预热加抓拍会拖到 12 秒。连续抓帧时
      // fb_get() 直接取现成一帧，抓拍阶段稳定在 ~240 毫秒。
      config.jpeg_quality = 12;
      config.fb_count = 2;
      config.grab_mode = CAMERA_GRAB_LATEST;
    } else {
      // Limit the frame size when PSRAM is not available
      config.frame_size = FRAMESIZE_SVGA;
      config.fb_location = CAMERA_FB_IN_DRAM;
    }
  } else {
    // Best option for face detection/recognition
    config.frame_size = FRAMESIZE_240X240;
#if CONFIG_IDF_TARGET_ESP32S3
    config.fb_count = 2;
#endif
  }

#if defined(CAMERA_MODEL_ESP_EYE)
  pinMode(13, INPUT_PULLUP);
  pinMode(14, INPUT_PULLUP);
#endif

  // camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    startWifiConfigPortal();
    printStartupSerialCommands();
    return;
  }

  cameraReady = true;
  sensor_t * s = esp_camera_sensor_get();

  // initial sensors are flipped vertically and colors are a bit saturated
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1); // flip it back
  }

  // ------------------------------------------------------------------
  // 视觉识别取景参数
  // ------------------------------------------------------------------
  if (config.pixel_format == PIXFORMAT_JPEG) {
    // 默认取 QVGA 320x240：单帧体积小、上传快，弱网下更稳。
    s->set_framesize(s, FRAMESIZE_QVGA);

    // 画质（数值越小越清晰）：6 比默认 12 明显更清晰
    s->set_quality(s, 12);   // 体积与清晰度的平衡点，越大压得越狠

    // 不同型号的传感器对部分能力支持不一，这里统一先判空再调用。
    #define SET_IF_OK(fn, ...) do { if (s->fn) s->fn(s, __VA_ARGS__); } while (0)

    // 锐度提到最高（-2 到 2），改善边缘发虚
    SET_IF_OK(set_sharpness, 2);

    // 自动曝光 / 自动增益：亮度自动适应，不必手工调
    SET_IF_OK(set_exposure_ctrl, 1);   // 打开自动曝光（AEC）
    SET_IF_OK(set_gain_ctrl, 1);       // 打开自动增益（AGC）
    SET_IF_OK(set_aec2, 1);            // 开启 AEC2，暗光下更稳
    SET_IF_OK(set_ae_level, 1);        // 曝光补偿 +1，暗光环境整体提亮
    SET_IF_OK(set_gainceiling, GAINCEILING_128X);  // 暗光允许更高增益，避免照片发黑

    // 自动白平衡，避免偏色
    SET_IF_OK(set_whitebal, 1);
    SET_IF_OK(set_awb_gain, 1);
    SET_IF_OK(set_wb_mode, 0);

    // 画质增强：坏点校正、白点校正、镜头阴影校正、伽马校正
    SET_IF_OK(set_bpc, 1);
    SET_IF_OK(set_wpc, 1);
    SET_IF_OK(set_lenc, 1);
    SET_IF_OK(set_raw_gma, 1);
    SET_IF_OK(set_dcw, 1);

    SET_IF_OK(set_brightness, 0);
    SET_IF_OK(set_contrast, 0);
    SET_IF_OK(set_saturation, 0);
  }

#if defined(CAMERA_MODEL_M5STACK_WIDE) || defined(CAMERA_MODEL_M5STACK_ESP32CAM)
  s->set_vflip(s, 1);
  s->set_hmirror(s, 1);
#endif

#if defined(CAMERA_MODEL_ESP32S3_EYE)
  s->set_vflip(s, 1);
#endif

// Setup LED FLash if LED pin is defined in camera_pins.h
#if defined(LED_GPIO_NUM)
  setupLedFlash(LED_GPIO_NUM);
#endif

  Serial.printf("WiFi config loaded: ssid=%s\n", wifiSsid.c_str());
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  WiFi.setAutoReconnect(true);
  // 注意：setSleep/setTxPower 必须在 STA 真正启动后调用才生效，
  // 放在 begin() 紧后面会因为 Wi-Fi 还没 start 而静默失败，
  // 那样射频仍跑在 modem sleep，表现为“信号很好但丢包 80%+、RTT 几百毫秒”。
  // 这里先设置一次，连上以后再补一次确保生效。
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);

  const unsigned long wifiTimeoutMs = 20000;
  unsigned long wifiStartMs = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - wifiStartMs < wifiTimeoutMs) {
    delay(500);
    Serial.print(".");
  }

  if (shouldStartWifiConfigPortal(WiFi.status() == WL_CONNECTED,
                                  millis() - wifiStartMs, wifiTimeoutMs)) {
    Serial.println();
    Serial.printf("WiFi connection failed, status=%d\n", WiFi.status());
    Serial.println("Check SSID/password and make sure a 2.4 GHz network is available.");
    startWifiConfigPortal();
    printStartupSerialCommands();
    return;
  }

  Serial.println("");
  Serial.println("WiFi connected");

  // 连上以后重新下发一次射频参数，确保省电关掉、功率拉满
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);

  // 关键修复：之前用调试接口改过 Wi-Fi 协议位图（去掉 11N），该改动会被
  // 驱动写进 NVS 并在重启后继续生效，导致射频退化成 11b/g-only，
  // 实测把发射方向打成 80%+ 丢包。这里每次开机强制恢复到 11b/g/n。
  esp_wifi_set_protocol(WIFI_IF_STA,
      WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
  esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
  esp_wifi_set_max_tx_power(78);   // 19.5dBm
  esp_wifi_set_ps(WIFI_PS_NONE);

  {
    uint8_t proto = 0;
    esp_wifi_get_protocol(WIFI_IF_STA, &proto);
    int8_t tp = 0;
    esp_wifi_get_max_tx_power(&tp);
    Serial.printf("WiFi radio restored: proto=0x%X txpower=%.2fdBm\n", proto, tp * 0.25);
  }

  startNetworkServicesIfReady();
}

// ---------------------------------------------------------------------------
// 串口控制台
// 本链路上 HTTP 经常 80%+ 丢包，测速/触发识别走串口更可靠，
// 也方便在不开网页的情况下连续跑测试。
// ---------------------------------------------------------------------------
// 分段网络诊断：把 DNS 解析 / TCP 连接 / TLS 握手分开计时，
// 便于判断 connect() 失败到底卡在哪一段。
static void runNetDiag(void)
{
  Serial.printf("DIAG: ip=%s gw=%s mask=%s dns=%s\n",
                WiFi.localIP().toString().c_str(),
                WiFi.gatewayIP().toString().c_str(),
                WiFi.subnetMask().toString().c_str(),
                WiFi.dnsIP().toString().c_str());

  // 1) 网关可达性（ICMP 从 ESP32 侧没有现成 API，这里用 TCP 连网关 80 端口近似）
  IPAddress gw = WiFi.gatewayIP();
  WiFiClient c1;
  unsigned long t = millis();
  int okGw = c1.connect(gw, 80, 3000) ? 1 : 0;
  unsigned long gwMs = millis() - t;
  c1.stop();

  // 2) DNS 解析
  IPAddress addr;
  t = millis();
  bool dnsOk = WiFi.hostByName("api.deepseek.com", addr);
  unsigned long dnsMs = millis() - t;

  // 3) 纯 TCP 连 443
  WiFiClient c2;
  unsigned long tcpMs = 0;
  int okTcp = 0;
  if (dnsOk) {
    t = millis();
    okTcp = c2.connect(addr, 443, 8000) ? 1 : 0;
    tcpMs = millis() - t;
    c2.stop();
  }

  // 4) TLS 握手
  WiFiClientSecure c3;
  c3.setInsecure();
  c3.setHandshakeTimeout(15);
  unsigned long tlsMs = 0;
  int okTls = 0;
  if (dnsOk) {
    t = millis();
    okTls = c3.connect(addr, 443, 15000) ? 1 : 0;
    tlsMs = millis() - t;
    c3.stop();
  }

  // 5) 公共 DNS 的 TCP 53，绕过域名直接测外网可达性
  IPAddress pub(223, 5, 5, 5);
  WiFiClient c4;
  t = millis();
  int okPub = c4.connect(pub, 53, 5000) ? 1 : 0;
  unsigned long pubMs = millis() - t;
  c4.stop();

  Serial.printf("DIAG: gw_tcp80=%d(%lums) dns=%d(%lums) ip=%s tcp443=%d(%lums) tls=%d(%lums) pub53=%d(%lums)\n",
                okGw, gwMs, dnsOk ? 1 : 0, dnsMs, addr.toString().c_str(),
                okTcp, tcpMs, okTls, tlsMs, okPub, pubMs);
}

static void printWifiDiag(void)
{
  wifi_ps_type_t ps = WIFI_PS_MIN_MODEM;
  esp_wifi_get_ps(&ps);

  int8_t txp = 0;
  esp_wifi_get_max_tx_power(&txp);

  uint8_t proto = 0;
  esp_wifi_get_protocol(WIFI_IF_STA, &proto);

  wifi_bandwidth_t bw = WIFI_BW_HT20;
  esp_wifi_get_bandwidth(WIFI_IF_STA, &bw);

  uint8_t ch = 0;
  wifi_second_chan_t sc = WIFI_SECOND_CHAN_NONE;
  esp_wifi_get_channel(&ch, &sc);

  Serial.printf("WIFI: ps=%d txpower=%.2fdBm proto=0x%X bw=%d channel=%u rssi=%d ip=%s\n",
                (int)ps, txp * 0.25, proto, (int)bw, (unsigned)ch,
                WiFi.RSSI(), WiFi.localIP().toString().c_str());

  wifi_ap_record_t ap;
  if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
    Serial.printf("WIFI: bssid=%02X:%02X:%02X:%02X:%02X:%02X primary_ch=%u rssi=%d\n",
                  ap.bssid[0], ap.bssid[1], ap.bssid[2],
                  ap.bssid[3], ap.bssid[4], ap.bssid[5],
                  (unsigned)ap.primary, ap.rssi);
  }
}

static void printVisionState(void)
{
  VisionState st = visionGetState();
  Serial.printf("STATE: lastSuccess=%d lastDurationMs=%lu ok=%d err=%d luma=%d imgBytes=%u msg=%s\n",
                st.lastSuccess ? 1 : 0, st.lastDurationMs,
                st.requestCount, st.errorCount, st.luma,
                (unsigned)st.imageBytes, st.lastMessage);
}

// 连续跑 n 次识别，输出每次耗时与汇总统计（成功率/平均/最快/最慢）
static void runVisionBench(int n)
{
  unsigned long best = 0, worst = 0, sum = 0;
  int ok = 0;
  Serial.printf("BENCH: start n=%d\n", n);
  for (int i = 1; i <= n; i++) {
    bool success = false;
    unsigned long dur = 0;
    char msg[VISION_MESSAGE_MAX];
    msg[0] = '\0';
    unsigned long t0 = millis();
    bool finished = visionTriggerWait(120000, &success, &dur, msg, sizeof(msg));
    unsigned long wall = millis() - t0;

    if (finished && success) {
      ok++;
      sum += dur;
      if (best == 0 || dur < best) best = dur;
      if (dur > worst) worst = dur;
    }
    Serial.printf("BENCH: #%d finished=%d success=%d dev_ms=%lu wall_ms=%lu msg=%s\n",
                  i, finished ? 1 : 0, success ? 1 : 0, dur, wall, msg);
  }
  Serial.printf("BENCH: done ok=%d/%d success_rate=%d%% avg=%lu min=%lu max=%lu\n",
                ok, n, (ok * 100) / n,
                ok ? (sum / ok) : 0UL, best, worst);
}

static void runSerialCommand(String cmd)
{
  // Normalize only the keyword. Never trim or lowercase credential values.
  String command = cmd;
  command.toLowerCase();

  if (command.startsWith("mcp_ip=")) {
    String value = cmd.substring(7);
    if (!isValidBridgeIp(value.c_str())) {
      Serial.println("CMD: mcp_ip must be a unicast IPv4 address");
    } else if (setDeviceNetworkParameter("mcp_ip", value.c_str()) == DeviceConfigResult::Saved) {
      Serial.printf("CMD: bridge IP saved: %s, reboot to apply\n", value.c_str());
    } else {
      Serial.println("CMD: failed to save bridge IP");
    }
    return;
  }

  if (command.startsWith("ssid=")) {
    String value = cmd.substring(5);
    if (value.length() == 0 || value.length() > 32) {
      Serial.println("CMD: ssid must be 1-32 bytes");
    } else if (setDeviceNetworkParameter("ssid", value.c_str()) == DeviceConfigResult::Saved) {
      Serial.printf("CMD: ssid saved (%u bytes), reboot to connect\n",
                    (unsigned)value.length());
    } else {
      Serial.println("CMD: failed to save ssid");
    }
    return;
  }
  if (command.startsWith("pwd=")) {
    String value = cmd.substring(4);
    if (value.length() > 64) {
      Serial.println("CMD: pwd must be 0-64 bytes (empty for an open network)");
    } else if (setDeviceNetworkParameter("pwd", value.c_str()) == DeviceConfigResult::Saved) {
      Serial.printf("CMD: pwd saved (%u bytes), reboot to connect\n",
                    (unsigned)value.length());
    } else {
      Serial.println("CMD: failed to save pwd");
    }
    return;
  }

  cmd.trim();
  command = cmd;
  command.toLowerCase();
  if (command.length() == 0) return;
  if (command == "mcp_ip?") {
    Serial.printf("MCP: bridge_ip=%s\n", getDeviceNetworkConfig().mcpIp);
  } else if (command == "ssid?") {
    printWifiCredential("ssid", String(getDeviceNetworkConfig().ssid));
  } else if (command == "pwd?") {
    printWifiCredential("pwd", String(getDeviceNetworkConfig().pwd));
  } else if (command == "v") {
    bool success = false;
    unsigned long dur = 0;
    char msg[VISION_MESSAGE_MAX];
    msg[0] = '\0';
    unsigned long t0 = millis();
    bool finished = visionTriggerWait(120000, &success, &dur, msg, sizeof(msg));
    Serial.printf("CMD: finished=%d success=%d dev_ms=%lu wall_ms=%lu msg=%s\n",
                  finished ? 1 : 0, success ? 1 : 0, dur, millis() - t0, msg);
  } else if (command == "s") {
    printVisionState();
  } else if (command == "w") {
    printWifiDiag();
  } else if (command == "d") {
    runNetDiag();
  } else if (command.startsWith("run")) {
    int n = command.substring(3).toInt();
    if (n < 1) n = 1;
    if (n > 50) n = 50;
    runVisionBench(n);
  } else if (command.startsWith("ps")) {
    int v = command.substring(2).toInt();
    esp_err_t e = esp_wifi_set_ps((wifi_ps_type_t)v);
    Serial.printf("CMD: ps=%d -> %s\n", v, esp_err_to_name(e));
  } else if (command.startsWith("tx")) {
    int v = command.substring(2).toInt();
    esp_err_t e = esp_wifi_set_max_tx_power((int8_t)v);
    Serial.printf("CMD: tx=%d -> %s\n", v, esp_err_to_name(e));
  } else if (command.startsWith("proto")) {
    int v = strtol(command.substring(5).c_str(), NULL, 0);
    esp_err_t e = esp_wifi_set_protocol(WIFI_IF_STA, (uint8_t)v);
    uint8_t cur = 0; esp_wifi_get_protocol(WIFI_IF_STA, &cur);
    Serial.printf("CMD: proto=0x%X -> %s (now 0x%X)\n", v, esp_err_to_name(e), cur);
  } else if (command.startsWith("bw")) {
    int v = command.substring(2).toInt();
    esp_err_t e = esp_wifi_set_bandwidth(WIFI_IF_STA, (wifi_bandwidth_t)v);
    Serial.printf("CMD: bw=%d -> %s\n", v, esp_err_to_name(e));
  } else if (command == "wifi_config") {
    startWifiConfigPortal(true);
  } else if (command == "help") {
    printSerialCommands();
  } else if (command == "reboot") {
    Serial.println("CMD: rebooting");
    delay(100);
    ESP.restart();
  } else {
    Serial.printf("CMD: unknown '%s' (help)\n", cmd.c_str());
  }
}

void loop() {
  static String cmdBuf;
  static bool discardLine = false;
  const size_t maxCommandBytes = 160;
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\r' || c == '\n') {
      if (!discardLine) runSerialCommand(cmdBuf);
      cmdBuf = "";
      discardLine = false;
    } else if (!discardLine) {
      if (c != '\0' && cmdBuf.length() < maxCommandBytes) {
        cmdBuf += c;
      } else {
        // Discard until the line ends: its tail must never become a command.
        cmdBuf = "";
        discardLine = true;
        Serial.println("CMD: input too long or contains NUL; line discarded");
      }
    }
  }
  startNetworkServicesIfReady();
  processDeviceRestart();
  delay(20);
}

static void startWifiConfigPortal(bool manual)
{
  if (provisioningMode) return;
  if (!WiFi.mode(WIFI_AP_STA)) {
    Serial.println("WiFi config hotspot mode failed to start");
    return;
  }
  IPAddress apIp(192, 168, 4, 1);
  IPAddress apGateway(192, 168, 4, 1);
  IPAddress apSubnet(255, 255, 255, 0);
  if (!WiFi.softAPConfig(apIp, apGateway, apSubnet)
      || !WiFi.softAP(WIFI_CONFIG_AP_SSID, NULL, 1, 0, 4)) {
    Serial.println("WiFi config hotspot failed to start");
    return;
  }
  provisioningMode = true;
  provisioningManual = manual;
  startHttpServerIfNeeded();
  Serial.printf("WiFi config hotspot: %s (open)\n", WIFI_CONFIG_AP_SSID);
  Serial.printf("WiFi config page: http://%s/config\n", WiFi.softAPIP().toString().c_str());
  printStartupSerialCommands();
}
