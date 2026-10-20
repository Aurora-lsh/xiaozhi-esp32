#pragma once

#include <stdint.h>

#define WIFI_CONFIG_AP_SSID "ESP32-CAM-7"
#define WIFI_CONFIG_AP_PASSWORD ""
#define WIFI_CONFIG_AP_IP "192.168.4.1"
#define WIFI_CONFIG_AP_IDLE_TIMEOUT_MS 20000UL

// Run the supplied idle check independently of setup/serial recognition calls.
bool startWifiConfigMonitor(void (*check)());

inline bool shouldCloseWifiConfigPortal(uint8_t connectedStations,
                                        uint32_t now,
                                        uint32_t lastActivity,
                                        uint32_t timeoutMs)
{
  return connectedStations == 0
      && static_cast<uint32_t>(now - lastActivity) >= timeoutMs;
}

// 热点是否已经真正关闭。
//
// 不能用 WiFi.softAPdisconnect(true) 的返回值判断：Arduino core 2.0.14 里
// softAPdisconnect() 先 esp_wifi_set_config() 再调 WiFi.enableAP(false)，
// 而 enableAP(false) 走 WiFiGenericClass::mode()，该函数在「关闭 AP」这条
// 正常路径上返回 false（并非失败）。于是热点已经关掉，返回值却是 false。
// 改为看 WiFi.getMode() 里 AP 位是否已清，才是可靠的判据。
inline bool isAccessPointDisabled(uint8_t wifiMode)
{
  return (wifiMode & 0x02) == 0;   // WIFI_MODE_AP == 2
}
