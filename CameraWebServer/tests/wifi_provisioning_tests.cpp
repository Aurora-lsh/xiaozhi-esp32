#include <cstdint>
#include <iostream>
#include <stdexcept>
#include "wifi_provisioning.h"

static void require(bool condition, const char* message)
{
  if (!condition) throw std::runtime_error(message);
}

int main()
{
  require(std::string(WIFI_CONFIG_AP_SSID) == "ESP32-CAM-7", "wrong configuration SSID");
  require(std::string(WIFI_CONFIG_AP_PASSWORD).empty(), "configuration hotspot must be open");
  require(std::string(WIFI_CONFIG_AP_IP) == "192.168.4.1", "wrong configuration address");
  require(!shouldStartWifiConfigPortal(true, 20000, 20000), "connected station started portal");
  require(!shouldStartWifiConfigPortal(false, 19999, 20000), "portal started before timeout");
  require(shouldStartWifiConfigPortal(false, 20000, 20000), "portal missing after Wi-Fi timeout");
  require(shouldStartWifiConfigPortal(false, 60000, 20000), "portal stopped after timeout");
  std::cout << "5/5 Wi-Fi provisioning checks passed\\n";
}
