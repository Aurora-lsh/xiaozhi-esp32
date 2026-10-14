#include "mcp_registration.h"
#include "mcp_registration_schedule.h"
#include "mcp_ip_validation.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>

#if __has_include("mcp_registration_config.h")
#include "mcp_registration_config.h"
#else
#define MCP_BRIDGE_DEFAULT_IP "192.168.1.100"
#define MCP_BRIDGE_PORT 8765
#define MCP_REGISTRATION_KEY ""
#endif

static String registrationUrl;

const char* mcpDefaultBridgeIp()
{
  return MCP_BRIDGE_DEFAULT_IP;
}

uint16_t mcpBridgePort()
{
  return MCP_BRIDGE_PORT;
}

static void registrationTask(void* parameter)
{
  (void)parameter;
  McpRegistrationSchedule schedule;
  for (;;) {
    const bool connected = WiFi.status() == WL_CONNECTED;
    const IPAddress ip = WiFi.localIP();
    if (schedule.due(connected, static_cast<uint32_t>(ip), millis())) {
      WiFiClient connection;
      HTTPClient http;
      http.setConnectTimeout(1500);
      http.setTimeout(1500);
      int status = -1;
      if (http.begin(connection, registrationUrl)) {
        http.addHeader("Content-Type", "application/json");
        http.addHeader("X-Camera-Key", MCP_REGISTRATION_KEY);
        const String payload = String("{\"ip\":\"") + ip.toString() + "\"}";
        status = http.POST(payload);
        http.end();
      }
      const bool succeeded = status == 200;
      schedule.completed(succeeded, millis());
      if (succeeded) {
        Serial.printf("MCP: registered camera IP %s\n", ip.toString().c_str());
      } else {
        Serial.printf("MCP: IP registration failed (%d), retry in 5s\n", status);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void startMcpRegistration(const String& bridgeIp)
{
  const String key = MCP_REGISTRATION_KEY;
  if (key.length() == 0) {
    Serial.println("MCP: IP registration disabled; configure mcp_registration_config.h");
    return;
  }
  bool validKey = key.length() <= 128;
  for (size_t index = 0; index < key.length(); ++index) {
    if (key[index] < 33 || key[index] > 126) validKey = false;
  }
  if (!isValidBridgeIp(bridgeIp.c_str()) || MCP_BRIDGE_PORT < 1 || MCP_BRIDGE_PORT > 65535 || !validKey) {
    Serial.println("MCP: invalid registration URL or key");
    return;
  }
  registrationUrl = String("http://") + bridgeIp + ":" + String(MCP_BRIDGE_PORT) + "/camera/register";
  if (xTaskCreate(registrationTask, "mcp-register", 4096, nullptr, 1, nullptr) != pdPASS) {
    Serial.println("MCP: could not create IP registration task");
  }
}
