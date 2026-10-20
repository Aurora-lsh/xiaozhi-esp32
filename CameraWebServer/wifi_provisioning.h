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
