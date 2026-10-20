#include <cstdint>
#include <iostream>
#include <stdexcept>
#include "wifi_provisioning.h"

static unsigned checksPassed = 0;
static void require(bool condition, const char* message)
{
  if (!condition) throw std::runtime_error(message);
  ++checksPassed;
}

int main()
{
  require(std::string(WIFI_CONFIG_AP_SSID) == "ESP32-CAM-7", "wrong configuration SSID");
  require(std::string(WIFI_CONFIG_AP_PASSWORD).empty(), "configuration hotspot must be open");
  require(std::string(WIFI_CONFIG_AP_IP) == "192.168.4.1", "wrong configuration address");
  require(!shouldCloseWifiConfigPortal(0, 20099, 100, 20000), "hotspot closed before first 20 seconds");
  require(shouldCloseWifiConfigPortal(0, 20100, 100, 20000), "idle hotspot did not close after 20 seconds");
  require(!shouldCloseWifiConfigPortal(1, 90000, 100, 20000), "connected client did not keep hotspot open");
  require(!shouldCloseWifiConfigPortal(3, 90000, 100, 20000), "multiple connected clients did not keep hotspot open");
  require(!shouldCloseWifiConfigPortal(0, 40010, 40010, 20000), "last client disconnect closed hotspot immediately");
  require(!shouldCloseWifiConfigPortal(0, 60009, 40010, 20000), "disconnect grace was too short");
  require(shouldCloseWifiConfigPortal(0, 60010, 40010, 20000), "hotspot did not close 20 seconds after disconnect");
  require(!shouldCloseWifiConfigPortal(2, 60010, 40010, 20000), "remaining clients did not keep hotspot open");
  require(!shouldCloseWifiConfigPortal(1, 70000, 50000, 20000), "last remaining client did not keep hotspot open");
  require(shouldCloseWifiConfigPortal(0, 90000, 70000, 20000), "last-client grace period did not expire");
  require(!shouldCloseWifiConfigPortal(0, 90000, 70001, 20000), "new disconnect grace period was ignored");
  require(!shouldCloseWifiConfigPortal(1, 90000, 70001, 20000), "client arriving at timeout was ignored");
  require(!shouldCloseWifiConfigPortal(0, 9998, UINT32_MAX - 10000, 20000), "timer wraparound closed hotspot early");
  require(shouldCloseWifiConfigPortal(0, 9999, UINT32_MAX - 10000, 20000), "timer wraparound prevented timeout");
  std::cout << checksPassed << "/" << checksPassed << " Wi-Fi provisioning timeout checks passed\n";
}
