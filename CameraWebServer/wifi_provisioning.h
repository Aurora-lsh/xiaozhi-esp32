#pragma once

#include <stdint.h>

#define WIFI_CONFIG_AP_SSID "ESP32-CAM-7"
#define WIFI_CONFIG_AP_PASSWORD ""
#define WIFI_CONFIG_AP_IP "192.168.4.1"

inline bool shouldStartWifiConfigPortal(bool stationConnected,
                                        uint32_t elapsedMs,
                                        uint32_t timeoutMs)
{
  return !stationConnected && elapsedMs >= timeoutMs;
}
