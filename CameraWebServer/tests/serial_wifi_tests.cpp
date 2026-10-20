// Host fakes cover hardware boundaries only; the actual ino functions are compiled below.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <atomic>
#include "device_config.h"
#include <stdexcept>
#include <string>
#include <vector>
#include "mcp_ip_validation.h"
#include "wifi_provisioning.h"

class String {
  std::string value;
public:
  String() = default;
  String(const char* text) : value(text) {}
  String(const std::string& text) : value(text) {}
  size_t length() const { return value.size(); }
  const char* c_str() const { return value.c_str(); }
  String substring(size_t offset) const { return value.substr(offset); }
  bool startsWith(const char* prefix) const { return value.rfind(prefix, 0) == 0; }
  void trim() {
    auto first = value.find_first_not_of(" \t\r\n");
    value = first == std::string::npos ? "" : value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
  }
  void toLowerCase() {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });
  }
  int toInt() const { return std::atoi(value.c_str()); }
  String& operator+=(char c) { value += c; return *this; }
  bool operator==(const char* text) const { return value == text; }
  bool operator==(const String& other) const { return value == other.value; }
};

class Preferences {
public:
  static inline std::map<std::string, String> store;
  static inline bool failOpen = false;
  static inline bool failWrite = false;
  static inline unsigned writes = 0;
  bool begin(const char*, bool = false) { return !failOpen; }
  void end() {}
  size_t putString(const char* key, const String& value) {
    ++writes;
    if (failWrite) return 0;
    store[key] = value;
    return value.length();
  }
  bool isKey(const char* key) { return store.count(key) != 0; }
  String getString(const char* key, const char* fallback = "") {
    auto item = store.find(key);
    return item == store.end() ? String(fallback) : item->second;
  }
};

struct SerialFake {
  std::string output;
  std::deque<char> input;
  int available() const { return static_cast<int>(input.size()); }
  int read() { char c = input.front(); input.pop_front(); return c; }
  void println(const char* text) { output += text; output += '\n'; }
  template<typename... Args> void printf(const char* format, Args... args) {
    int size = std::snprintf(nullptr, 0, format, args...);
    std::vector<char> buffer(static_cast<size_t>(size) + 1);
    std::snprintf(buffer.data(), buffer.size(), format, args...);
    output += buffer.data();
  }
} Serial;
struct ESPFake { bool restarted = false; void restart() { restarted = true; } } ESP;
static unsigned long currentMillis = 0;
static unsigned long millis() { return currentMillis; }
static void delay(int) {}
constexpr int VISION_MESSAGE_MAX = 256;
using esp_err_t = int;
using wifi_ps_type_t = int;
using wifi_bandwidth_t = int;
constexpr int WIFI_IF_STA = 0;
static int visionCalls = 0;
static bool visionTriggerWait(int, bool*, unsigned long*, char*, size_t) { ++visionCalls; return true; }
static void printVisionState() { Serial.println("legacy state"); }
static void printWifiDiag() { Serial.println("legacy wifi"); }
static void runNetDiag() { Serial.println("legacy net"); }
static void runVisionBench(int) { Serial.println("legacy bench"); }
static int esp_wifi_set_ps(int) { return 0; }
static int esp_wifi_set_max_tx_power(int8_t) { return 0; }
static int esp_wifi_set_protocol(int, uint8_t) { return 0; }
static int esp_wifi_get_protocol(int, uint8_t*) { return 0; }
static int esp_wifi_set_bandwidth(int, int) { return 0; }
static const char* esp_err_to_name(int) { return "OK"; }
static const char* mcpDefaultBridgeIp() { return "192.168.1.100"; }
static unsigned mcpBridgePort() { return 8765; }
constexpr int WL_CONNECTED = 3;
constexpr int WIFI_AP = 2;
constexpr int WIFI_AP_STA = 3;
using WiFiEvent_t = int;
constexpr int ARDUINO_EVENT_WIFI_AP_STACONNECTED = 12;
constexpr int ARDUINO_EVENT_WIFI_AP_STADISCONNECTED = 13;
struct FakeIp {
  String value = "192.168.1.55";
  FakeIp() = default;
  FakeIp(int, int, int, int) : value("192.168.4.1") {}
  String toString() const { return value; }
};
using IPAddress = FakeIp;
struct WiFiFake {
  bool online = false;
  bool apStarts = true;
  bool modeSucceeds = true;
  bool autoReconnect = true;
  bool apConfigSucceeds = true;
  int currentMode = 0;
  String apSsid;
  bool passwordWasNull = false;
  bool apDisconnected = false;
  bool apStopSucceeds = true;
  unsigned connectedStations = 0;
  unsigned apStopCalls = 0;
  template <typename Callback>
  int onEvent(Callback, int) { return 0; }
  unsigned apConfigCalls = 0;
  unsigned apStartCalls = 0;
  int status() const { return online ? WL_CONNECTED : 0; }
  FakeIp localIP() const { return FakeIp(); }
  FakeIp softAPIP() const { return FakeIp(192, 168, 4, 1); }
  bool mode(int value) {
    if (!modeSucceeds) return false;
    currentMode = value;
    if (value == WIFI_AP) online = false;
    return true;
  }
  void setAutoReconnect(bool value) { autoReconnect = value; }
  bool softAPConfig(FakeIp, FakeIp, FakeIp) { ++apConfigCalls; return apConfigSucceeds; }
  bool softAP(const char* ssid, const char* password, int, int, int) {
    ++apStartCalls; apSsid = ssid; passwordWasNull = password == nullptr; return apStarts;
  }
  bool softAPdisconnect(bool) {
    ++apStopCalls;
    if (!apStopSucceeds) return false;
    apDisconnected = true;
    connectedStations = 0;
    return true;
  }
  unsigned softAPgetStationNum() const { return connectedStations; }
} WiFi;
static unsigned serverStarts = 0;
static unsigned visionStarts = 0;
static unsigned registrationStarts = 0;
static unsigned statusStarts = 0;
static String registeredBridgeIp;
static void startCameraServer() { ++serverStarts; }
static void visionInit() { ++visionStarts; }
static void mcpStatusInit() { ++statusStarts; }
static void startMcpRegistration(const String& ip) { ++registrationStarts; registeredBridgeIp = ip; }

#include "sketch_under_test.h"

static void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
static void feed(const std::string& line) {
  for (char c : line) Serial.input.push_back(c);
  loop();
}
static void reset() {
  Preferences::store.clear();
  Preferences::failOpen = Preferences::failWrite = false;
  Preferences::writes = 0;
  ESP.restarted = false;
  visionCalls = 0;
  WiFi.online = false;
  WiFi.apStarts = WiFi.apConfigSucceeds = true;
  WiFi.modeSucceeds = WiFi.autoReconnect = true;
  WiFi.currentMode = 0;
  WiFi.apSsid = "";
  WiFi.passwordWasNull = false;
  WiFi.apDisconnected = false;
  WiFi.apStopSucceeds = true;
  WiFi.connectedStations = WiFi.apStopCalls = 0;
  WiFi.apConfigCalls = WiFi.apStartCalls = 0;
  cameraReady = false;
  provisioningMode = false;
  provisioningLastActivityMs = 0;
  provisioningMonitorStarted = false;
  networkServicesStarted = false;
  httpServerStarted = false;
  mcpStatusStarted = false;
  registrationStarted = false;
  startupCommandsPrinted = false;
  restartRequested = false;
  restartRequestedAt = 0;
  currentMillis = 0;
  serverStarts = visionStarts = registrationStarts = statusStarts = 0;
  feed("\n");
  loadWifiCredentials();
  Serial.output.clear();
}
int main() {
  std::vector<std::pair<const char*, std::function<void()>>> tests = {
    {"late WiFi connection starts all services once", [] {
      cameraReady = true;
      mcpBridgeIp = "192.168.1.100";
      loop();
      require(serverStarts == 0 && registrationStarts == 0, "offline services started");
      WiFi.online = true;
      loop();
      require(serverStarts == 1 && visionStarts == 1 && registrationStarts == 1 && statusStarts == 1,
              "late connection did not start services");
      require(registeredBridgeIp == "192.168.1.100", "saved bridge IP not passed to task");
      WiFi.online = false; loop(); WiFi.online = true; loop();
      require(serverStarts == 1 && registrationStarts == 1, "reconnect duplicated services");
      require(Serial.output.find("串口命令（115200 波特率，每行一条）") != std::string::npos,
              "serial commands were not printed after initialization");
      for (const auto* command : {"ssid=<Wi-Fi名称>", "pwd=<Wi-Fi密码>", "mcp_ip=<桥接服务IPv4>",
           "ssid?", "pwd?", "mcp_ip?", "v", "s", "w", "d", "run<N>", "ps<N>", "tx<N>",
           "proto<N>", "bw<N>", "wifi_config", "reboot", "help"}) {
        require(Serial.output.find(command) != std::string::npos, "serial command missing from startup help");
      }
      auto helpStart = Serial.output.find("串口命令（115200 波特率，每行一条）");
      auto visionReady = Serial.output.find("Vision Recognition Ready!");
      require(helpStart != std::string::npos && visionReady != std::string::npos && helpStart > visionReady,
              "commands were not printed after service initialization");
      require(Serial.output.find("ESP32-CAM网页服务地址: http://192.168.1.55/vision") != std::string::npos,
              Serial.output.c_str());
      require(Serial.output.find("MCP上位机服务IP: 192.168.1.100:8765") != std::string::npos,
              Serial.output.c_str());
      auto commandsPrinted = Serial.output.substr(helpStart);
      Serial.output.clear();
      runSerialCommand("help");
      require(commandsPrinted.find(Serial.output) == 0,
              "help output differs from startup command list");
    }},
    {"camera initialization failure prevents service startup", [] {
      WiFi.online = true;
      loop();
      require(serverStarts == 0 && registrationStarts == 0, "failed camera services started");
    }},
    {"camera initialization failure still prints command list once", [] {
      printStartupSerialCommands();
      require(Serial.output.find("mcp_ip=<桥接服务IPv4>") != std::string::npos,
              "commands unavailable after camera initialization failure");
      auto output = Serial.output;
      printStartupSerialCommands();
      require(Serial.output == output, "startup command list printed more than once");
    }},
    {"provisioning hotspot constants are open and addressable", [] {
      require(std::string(WIFI_CONFIG_AP_SSID) == "ESP32-CAM-7", "wrong provisioning SSID");
      require(std::string(WIFI_CONFIG_AP_PASSWORD).empty(), "provisioning hotspot is not open");
      require(std::string(WIFI_CONFIG_AP_IP) == "192.168.4.1", "wrong provisioning IP");
    }},
    {"failed Wi-Fi starts open config hotspot and HTTP server", [] {
      cameraReady = true;
      startWifiConfigPortal();
      require(provisioningMode, "provisioning mode not marked active");
      require(WiFi.currentMode == WIFI_AP_STA, "station auto-retry was disabled in AP mode");
      require(WiFi.apConfigCalls == 1 && WiFi.apStartCalls == 1, "hotspot setup incomplete");
      require(WiFi.apSsid == WIFI_CONFIG_AP_SSID && WiFi.passwordWasNull, "hotspot SSID or open security wrong");
      require(serverStarts == 1 && httpServerStarted && visionStarts == 0, "offline portal services wrong");
      require(statusStarts == 1, "status cache not initialized for portal HTTP handlers");
      require(Serial.output.find("WiFi config page: http://192.168.4.1/config") != std::string::npos,
              "portal page address not printed");
      WiFi.online = true;
      loop();
      require(serverStarts == 1 && visionStarts == 1 && registrationStarts == 1,
              "STA reconnect did not activate services exactly once");
      require(provisioningMode && !WiFi.apDisconnected, "STA connection prematurely closed boot hotspot");
      require(statusStarts == 1, "status cache initialized more than once");
    }},
    {"wifi_config command starts manual open hotspot without erasing settings", [] {
      cameraReady = true;
      WiFi.online = true;
      loop();
      auto writes = Preferences::writes;
      auto savedSsid = wifiSsid;
      runSerialCommand("wifi_config");
      require(provisioningMode && WiFi.currentMode == WIFI_AP_STA && WiFi.passwordWasNull,
              "manual command did not open setup hotspot");
      loop();
      require(provisioningMode && serverStarts == 1, "manual config mode disappeared or duplicated server");
      require(wifiSsid == savedSsid && writes == Preferences::writes && !ESP.restarted,
              "manual setup command erased credentials or restarted device");
    }},
    {"idle hotspot closes exactly twenty seconds after opening", [] {
      currentMillis = 100;
      startWifiConfigPortal();
      currentMillis = 20099;
      loop();
      require(provisioningMode && WiFi.apStopCalls == 0, "startup grace period was shortened");
      currentMillis = 20100;
      loop();
      require(!provisioningMode && WiFi.apStopCalls == 1, "idle hotspot did not close");
      currentMillis = 70000;
      loop();
      require(WiFi.apStartCalls == 1, "offline STA automatically reopened expired hotspot");
    }},
    {"connected hotspot client stays online while STA connects", [] {
      startWifiConfigPortal();
      WiFi.connectedStations = 1;
      currentMillis = 18000;
      onWifiEvent(ARDUINO_EVENT_WIFI_AP_STACONNECTED);
      cameraReady = true;
      WiFi.online = true;
      currentMillis = 100000;
      loop();
      require(provisioningMode && WiFi.apStopCalls == 0, "connected AP client was disconnected by STA or timeout");
      require(visionStarts == 1 && registrationStarts == 1, "STA services did not start alongside AP");
    }},
    {"last disconnect resets twenty second idle period", [] {
      startWifiConfigPortal();
      currentMillis = 1000;
      WiFi.connectedStations = 2;
      onWifiEvent(ARDUINO_EVENT_WIFI_AP_STACONNECTED);
      currentMillis = 50000;
      WiFi.connectedStations = 1;
      onWifiEvent(ARDUINO_EVENT_WIFI_AP_STADISCONNECTED);
      loop();
      require(provisioningMode, "remaining client was ignored");
      currentMillis = 60000;
      WiFi.connectedStations = 0;
      onWifiEvent(ARDUINO_EVENT_WIFI_AP_STADISCONNECTED);
      currentMillis = 79999;
      loop();
      require(provisioningMode, "last disconnect grace ended early");
      currentMillis = 80000;
      loop();
      require(!provisioningMode && WiFi.apStopCalls == 1, "last disconnect timeout did not close AP");
    }},
    {"quick reconnect between main loop polls resets timeout", [] {
      startWifiConfigPortal();
      currentMillis = 19000;
      onWifiEvent(ARDUINO_EVENT_WIFI_AP_STACONNECTED);
      currentMillis = 19500;
      onWifiEvent(ARDUINO_EVENT_WIFI_AP_STADISCONNECTED);
      currentMillis = 39499;
      loop();
      require(provisioningMode, "short-lived client activity was lost");
      currentMillis = 39500;
      loop();
      require(!provisioningMode, "short-lived client grace did not expire");
    }},
    {"wifi_config reopening obeys same idle timeout", [] {
      startWifiConfigPortal();
      currentMillis = 20000;
      loop();
      currentMillis = 30000;
      runSerialCommand("wifi_config");
      require(provisioningMode && WiFi.apStartCalls == 2 && serverStarts == 1, "manual reopening failed or duplicated HTTP server");
      currentMillis = 49999;
      loop();
      require(provisioningMode, "manually reopened hotspot grace ended early");
      currentMillis = 50000;
      loop();
      require(!provisioningMode && WiFi.apStopCalls == 2, "manual hotspot bypassed idle timeout");
    }},
    {"repeating wifi_config does not extend an active idle hotspot", [] {
      startWifiConfigPortal();
      currentMillis = 19000;
      runSerialCommand("wifi_config");
      currentMillis = 20000;
      loop();
      require(!provisioningMode && WiFi.apStartCalls == 1, "repeated command extended idle timeout");
    }},
    {"hotspot close failure retries without losing AP state", [] {
      startWifiConfigPortal();
      currentMillis = 20000;
      WiFi.apStopSucceeds = false;
      loop();
      require(provisioningMode && WiFi.apStopCalls == 1, "close failure marked AP inactive");
      currentMillis = 20999;
      loop();
      require(WiFi.apStopCalls == 1, "close retry was not throttled");
      currentMillis = 21000;
      WiFi.apStopSucceeds = true;
      loop();
      require(!provisioningMode && WiFi.apStopCalls == 2, "close retry failed");
    }},
    {"AP timeout handles millis wraparound", [] {
      currentMillis = UINT32_MAX - 10000;
      startWifiConfigPortal();
      currentMillis = 9998;
      loop();
      require(provisioningMode, "clock wrap closed AP early");
      currentMillis = 9999;
      loop();
      require(!provisioningMode, "clock wrap prevented AP timeout");
    }},
    {"hotspot works even when camera sensor initialization fails", [] {
      cameraReady = false;
      startWifiConfigPortal();
      require(serverStarts == 1 && httpServerStarted && visionStarts == 0 && statusStarts == 1,
              "camera-free configuration page was unavailable");
      require(Serial.output.find("WiFi config hotspot: ESP32-CAM-7 (open)") != std::string::npos,
              "open setup hotspot was not reported");
    }},
    {"hotspot startup failure leaves retry path available", [] {
      WiFi.apStarts = false;
      startWifiConfigPortal();
      require(!provisioningMode && !httpServerStarted && serverStarts == 0,
              "failed AP incorrectly marked active");
      require(Serial.output.find("WiFi config hotspot failed to start") != std::string::npos,
              "AP startup failure not reported");
    }},
    {"hotspot mode failure is reported", [] {
      WiFi.modeSucceeds = false;
      startWifiConfigPortal();
      require(!provisioningMode && serverStarts == 0 && WiFi.apStartCalls == 0,
              "hotspot started despite mode failure");
      require(Serial.output.find("WiFi config hotspot mode failed to start") != std::string::npos,
              "hotspot mode failure not reported");
    }},
    {"web restart waits until after HTTP response", [] {
      currentMillis = 100;
      requestDeviceRestart();
      currentMillis = 1099;
      requestDeviceRestart();
      loop();
      require(!ESP.restarted, "web reboot did not allow response to complete");
      currentMillis = 1100;
      loop();
      require(ESP.restarted, "web reboot was not applied after one second");
    }},
    {"web settings share persistent serial configuration", [] {
      require(setDeviceNetworkParameter("ssid", "Web AP") == DeviceConfigResult::Saved, "web SSID save failed");
      require(setDeviceNetworkParameter("pwd", "Case=Sensitive") == DeviceConfigResult::Saved, "web password save failed");
      require(setDeviceNetworkParameter("mcp_ip", "192.168.1.42") == DeviceConfigResult::Saved, "web bridge save failed");
      auto config = getDeviceNetworkConfig();
      require(std::string(config.ssid) == "Web AP" && std::string(config.pwd) == "Case=Sensitive"
              && std::string(config.mcpIp) == "192.168.1.42", "web snapshot differs from saved settings");
      loadWifiCredentials();
      require(wifiSsid == "Web AP" && wifiPassword == "Case=Sensitive" && mcpBridgeIp == "192.168.1.42",
              "web settings lost on restart");
      require(!ESP.restarted, "saving web configuration restarted immediately");
    }},
    {"web settings validate bounds and report storage failures", [] {
      require(setDeviceNetworkParameter("ssid", "") == DeviceConfigResult::Invalid, "empty SSID saved");
      require(setDeviceNetworkParameter("pwd", std::string(65, 'x').c_str()) == DeviceConfigResult::Invalid, "long password saved");
      require(setDeviceNetworkParameter("mcp_ip", "127.0.0.1") == DeviceConfigResult::Invalid, "invalid IP saved");
      require(setDeviceNetworkParameter("other", "abc") == DeviceConfigResult::Invalid, "unknown setting saved");
      Preferences::failWrite = true;
      require(setDeviceNetworkParameter("ssid", "Unsaved") == DeviceConfigResult::StorageFailure, "write failure ignored");
      require(wifiSsid == DEFAULT_WIFI_SSID, "failed save changed pending settings");
      Preferences::failWrite = false;
      require(setDeviceNetworkParameter("pwd", "") == DeviceConfigResult::Saved, "open Wi-Fi password rejected");
    }},
    {"bridge default query", [] {
      runSerialCommand("mcp_ip?");
      require(Serial.output == "MCP: bridge_ip=192.168.1.100\n", "default bridge IP not reported");
    }},
    {"bridge IP persists after reboot", [] {
      runSerialCommand("MCP_IP=192.168.1.50");
      require(mcpBridgeIp == "192.168.1.50" && !ESP.restarted, "bridge IP not saved");
      require(Serial.output.find("reboot to apply") != std::string::npos, "reboot instruction missing");
      mcpBridgeIp = "old";
      loadWifiCredentials();
      require(mcpBridgeIp == "192.168.1.50", "bridge IP not restored");
    }},
    {"invalid bridge IP is rejected", [] {
      for (const auto* ip : {"", "hostname", "0.0.0.0", "127.0.0.1", "224.1.1.1",
           "256.1.1.1", "192.168.1", "192.168.1.1/path", "1.2.3.4.5", "01.2.3.4", " 1.2.3.4"}) {
        runSerialCommand(String(std::string("mcp_ip=") + ip));
      }
      require(mcpBridgeIp == mcpDefaultBridgeIp() && Preferences::writes == 0, "invalid IP saved");
    }},
    {"bridge storage failure preserves previous address", [] {
      Preferences::failWrite = true;
      runSerialCommand("mcp_ip=192.168.1.50");
      require(mcpBridgeIp == mcpDefaultBridgeIp(), "failed save changed address");
    }},
    {"bridge open failure preserves previous address", [] {
      Preferences::failOpen = true;
      runSerialCommand("mcp_ip=192.168.1.50");
      require(mcpBridgeIp == mcpDefaultBridgeIp(), "failed open changed address");
    }},
    {"bridge unchanged saved IP avoids flash writes", [] {
      runSerialCommand("mcp_ip=192.168.1.50");
      auto writes = Preferences::writes;
      runSerialCommand("mcp_ip=192.168.1.50");
      require(Preferences::writes == writes, "unchanged bridge IP rewritten");
    }},
    {"bridge invalid saved IP falls back", [] {
      Preferences::store["bridge_ip"] = "invalid";
      loadWifiCredentials();
      require(mcpBridgeIp == mcpDefaultBridgeIp(), "invalid saved IP restored");
    }},
    {"bridge serial CRLF commands work", [] {
      feed("mcp_ip=192.168.1.55\r\nmcp_ip?\r\n");
      require(mcpBridgeIp == "192.168.1.55" && Preferences::writes == 1, "bridge CRLF failed");
      require(Serial.output.find("MCP: bridge_ip=192.168.1.55") != std::string::npos, "query lost");
    }},
    {"default queries", [] {
      runSerialCommand("ssid?"); runSerialCommand("pwd?");
      require(Serial.output == std::string("WIFI: ssid=") + DEFAULT_WIFI_SSID + "\nWIFI: pwd=" + DEFAULT_WIFI_PASSWORD + "\n", "defaults not reported");
    }},
    {"case and equals preserved", [] {
      runSerialCommand("SSID=MixedCaseAP"); runSerialCommand("PWD=AbCd=1234");
      require(wifiSsid == "MixedCaseAP" && wifiPassword == "AbCd=1234", "case or equals changed");
      require(!ESP.restarted && visionCalls == 0, "settings changed connection immediately");
    }},
    {"value whitespace preserved", [] {
      runSerialCommand("ssid= AP "); runSerialCommand("pwd= Secret ");
      require(wifiSsid == " AP " && wifiPassword == " Secret ", "value whitespace stripped");
    }},
    {"save replies have real newline", [] {
      runSerialCommand("pwd=AbCd1234");
      require(Serial.output == "CMD: pwd saved (8 bytes), reboot to connect\n", "incorrect success reply");
    }},
    {"reboot reload", [] {
      runSerialCommand("ssid=MixedAP"); runSerialCommand("pwd=PassWord");
      wifiSsid = "discard"; wifiPassword = "discard";
      loadWifiCredentials();
      require(wifiSsid == "MixedAP" && wifiPassword == "PassWord", "saved configuration not reloaded");
    }},
    {"empty password persists", [] {
      runSerialCommand("pwd="); loadWifiCredentials();
      require(wifiPassword == "" && Preferences::store.count("pwd"), "empty password lost");
    }},
    {"empty password storage failure", [] {
      Preferences::failWrite = true; runSerialCommand("pwd=");
      require(wifiPassword == DEFAULT_WIFI_PASSWORD, "failed empty save updated memory");
      require(Serial.output.find("failed to save pwd") != std::string::npos, "failed empty save reported success");
    }},
    {"nonempty password storage failure", [] {
      Preferences::failWrite = true; runSerialCommand("pwd=NewValue");
      require(wifiPassword == DEFAULT_WIFI_PASSWORD, "failed save updated memory");
    }},
    {"storage open failure", [] {
      Preferences::failOpen = true; runSerialCommand("ssid=NewAP");
      require(wifiSsid == DEFAULT_WIFI_SSID, "failed open updated memory");
    }},
    {"SSID upper bound", [] {
      runSerialCommand(String("ssid=" + std::string(33, 'A')));
      require(wifiSsid == DEFAULT_WIFI_SSID && Preferences::writes == 0, "oversized SSID saved");
    }},
    {"password upper bound", [] {
      runSerialCommand(String("pwd=" + std::string(65, 'A')));
      require(wifiPassword == DEFAULT_WIFI_PASSWORD && Preferences::writes == 0, "oversized password saved");
    }},
    {"valid maximum lengths", [] {
      feed("ssid=" + std::string(32, 'S') + "\npwd=" + std::string(64, 'A') + "\n");
      require(wifiSsid.length() == 32 && wifiPassword.length() == 64, "maximum length truncated");
    }},
    {"empty SSID rejected", [] {
      runSerialCommand("ssid=");
      require(wifiSsid == DEFAULT_WIFI_SSID && Preferences::writes == 0, "empty SSID saved");
    }},
    {"unchanged saved settings avoid writes", [] {
      runSerialCommand("ssid=MixedAP"); runSerialCommand("pwd=Secret123");
      auto writes = Preferences::writes;
      runSerialCommand("ssid=MixedAP"); runSerialCommand("pwd=Secret123");
      require(Preferences::writes == writes, "unchanged settings rewritten");
    }},
    {"CR-only commands", [] {
      feed("ssid=CR_AP\r");
      require(wifiSsid == "CR_AP", "CR not treated as terminator");
    }},
    {"CRLF commands once", [] {
      feed("pwd=Secret123\r\n");
      require(Preferences::writes == 1, "CRLF command executed incorrectly");
    }},
    {"overflow tail never executed", [] {
      feed(std::string(161, 'x') + "pwd=Injected\n");
      require(wifiPassword == DEFAULT_WIFI_PASSWORD && Preferences::writes == 0, "overflow tail executed");
      feed("ssid=Recovered\n");
      require(wifiSsid == "Recovered", "parser did not recover");
    }},
    {"UTF-8 byte limit", [] {
      runSerialCommand(String(std::string("ssid=") + std::string("\xE4\xB8\xAD") + std::string(30, 'X')));
      require(wifiSsid == DEFAULT_WIFI_SSID, "SSID byte limit ignored");
    }},
    {"NUL line rejected", [] {
      feed(std::string("pwd=Bad") + std::string(1, '\0') + "Value\n");
      require(wifiPassword == DEFAULT_WIFI_PASSWORD && Preferences::writes == 0, "embedded NUL saved");
    }},
    {"partial command waits for terminator", [] {
      feed("pwd=Partial123");
      require(Preferences::writes == 0, "unterminated command executed");
      feed("\n");
      require(wifiPassword == "Partial123", "partial input lost");
    }},
    {"SSID storage failure", [] {
      Preferences::failWrite = true; runSerialCommand("ssid=NewAP");
      require(wifiSsid == DEFAULT_WIFI_SSID, "failed SSID save updated memory");
    }},
    {"loading absent storage falls back", [] {
      Preferences::failOpen = true;
      wifiSsid = "OldAP"; wifiPassword = "OldPassword";
      loadWifiCredentials();
      require(wifiSsid == DEFAULT_WIFI_SSID && wifiPassword == DEFAULT_WIFI_PASSWORD, "defaults not restored");
    }},
    {"legacy commands and reboot", [] {
      runSerialCommand(" S "); runSerialCommand("HELP"); runSerialCommand("reboot");
      require(Serial.output.find("legacy state") != std::string::npos, "legacy command failed");
      require(Serial.output.find("ssid?") != std::string::npos && ESP.restarted, "help or reboot missing");
    }}
  };
  unsigned failed = 0;
  for (const auto& test : tests) {
    reset();
    try { test.second(); std::cout << "PASS: " << test.first << '\n'; }
    catch (const std::exception& e) { ++failed; std::cout << "FAIL: " << test.first << ": " << e.what() << '\n'; }
  }
  std::cout << (tests.size() - failed) << "/" << tests.size() << " passed\n";
  return failed ? 1 : 0;
}
