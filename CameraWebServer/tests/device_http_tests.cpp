#include <algorithm>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include <ArduinoJson.h>
#include "device_config.h"
#include "mcp_ip_validation.h"

using esp_err_t = int;
constexpr int ESP_OK = 0;
constexpr int HTTPD_RESP_USE_STRLEN = -1;
struct httpd_req_t {
  size_t content_len = 0;
  std::string body;
  std::string contentType = "application/json";
  size_t offset = 0;
  size_t chunkSize = 7;
  std::string status = "200 OK";
  std::string response;
  bool sendFailure = false;
  std::map<std::string, std::string> headers;
};

static esp_err_t httpd_resp_set_status(httpd_req_t* request, const char* value) {
  request->status = value; return ESP_OK;
}
static esp_err_t httpd_resp_set_type(httpd_req_t* request, const char* value) {
  request->headers["Content-Type"] = value; return ESP_OK;
}
static esp_err_t httpd_resp_set_hdr(httpd_req_t* request, const char* name, const char* value) {
  request->headers[name] = value; return ESP_OK;
}
static esp_err_t httpd_resp_send(httpd_req_t* request, const char* body, int length) {
  request->response.assign(body, length < 0 ? strlen(body) : static_cast<size_t>(length));
  return request->sendFailure ? -1 : ESP_OK;
}
static esp_err_t httpd_resp_send_500(httpd_req_t* request) {
  request->status = "500 Internal Server Error"; return ESP_OK;
}
static esp_err_t httpd_req_get_hdr_value_str(httpd_req_t* request, const char*, char* value, size_t length) {
  if (request->contentType.size() >= length) return -1;
  std::strcpy(value, request->contentType.c_str()); return ESP_OK;
}
static int httpd_req_recv(httpd_req_t* request, char* destination, size_t length) {
  const size_t count = std::min({length, request->body.size() - request->offset, request->chunkSize});
  std::memcpy(destination, request->body.data() + request->offset, count);
  request->offset += count;
  return static_cast<int>(count);
}

static DeviceNetworkConfig pending = {};
static unsigned writes = 0;
static bool failSave = false;
static bool restartPending = false;
DeviceNetworkConfig getDeviceNetworkConfig() { return pending; }
bool isValidDeviceNetworkParameter(const char* name, const char* value) {
  if (!strcmp(name, "ssid")) return strlen(value) > 0 && strlen(value) <= 32;
  if (!strcmp(name, "pwd")) return strlen(value) <= 64;
  if (!strcmp(name, "mcp_ip")) return isValidBridgeIp(value);
  return false;
}
DeviceConfigResult setDeviceNetworkParameter(const char* name, const char* value) {
  if (failSave) return DeviceConfigResult::StorageFailure;
  ++writes;
  if (!strcmp(name, "ssid")) std::strcpy(pending.ssid, value);
  if (!strcmp(name, "pwd")) std::strcpy(pending.pwd, value);
  if (!strcmp(name, "mcp_ip")) std::strcpy(pending.mcpIp, value);
  return DeviceConfigResult::Saved;
}
void requestDeviceRestart() { restartPending = true; }

#include "device_http_under_test.h"

static void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
static httpd_req_t submit(const std::string& body, const std::string& contentType = "application/json") {
  httpd_req_t request;
  request.body = body; request.content_len = body.size(); request.contentType = contentType;
  device_config_post_handler(&request);
  return request;
}
static void reset() {
  std::strcpy(pending.ssid, "CurrentAP");
  std::strcpy(pending.pwd, "OriginalSecret");
  std::strcpy(pending.mcpIp, "192.168.1.100");
  writes = 0; failSave = false; restartPending = false;
}

int main() {
  std::vector<std::pair<const char*, std::function<void()>>> checks = {
    {"read configuration without disclosing password", [] {
      httpd_req_t request; device_config_get_handler(&request);
      JsonDocument response; require(!deserializeJson(response, request.response), "invalid response JSON");
      require(response["ssid"] == "CurrentAP" && response["mcp_ip"] == "192.168.1.100", "incorrect settings");
      require(response["pwd"].isUnbound() && request.response.find("OriginalSecret") == std::string::npos, "password disclosed");
      require(request.headers["Cache-Control"] == "no-store", "config cached");
    }},
    {"chunked receive preserves password when omitted", [] {
      auto request = submit(R"({"ssid":" Web AP ","mcp_ip":"192.168.1.42"})");
      require(request.status == "200 OK" && writes == 2, "configuration not saved");
      require(std::string(pending.ssid) == " Web AP " && std::string(pending.pwd) == "OriginalSecret", "values altered");
      require(!restartPending, "save caused immediate restart");
    }},
    {"empty password saves open Wi-Fi", [] {
      auto request = submit(R"({"pwd":""})");
      require(request.status == "200 OK" && pending.pwd[0] == 0 && writes == 1, "empty password ignored");
    }},
    {"JSON escaped password preserved", [] {
      auto request = submit(R"({"pwd":"Case=Sensitive\\+\""})");
      require(request.status == "200 OK" && std::string(pending.pwd) == "Case=Sensitive\\+\"", "JSON escaping altered password");
    }},
    {"invalid values validate before any flash writes", [] {
      for (const auto* body : {R"({"ssid":"NewAP","mcp_ip":"127.0.0.1"})", R"({"ssid":""})",
           R"({"pwd":null})", R"({"ssid":123})", R"({"ssid":"ok\u0000bad"})", "{}", "[]", "{bad"}) {
        auto request = submit(body);
        require(request.status == "400 Bad Request" && writes == 0, "invalid request wrote settings");
      }
    }},
    {"oversized request and wrong content type rejected", [] {
      require(submit(std::string(513, 'x')).status == "400 Bad Request", "oversized body accepted");
      require(submit(R"({"ssid":"x"})", "text/plain").status == "400 Bad Request", "plain text accepted");
    }},
    {"incomplete request is rejected", [] {
      httpd_req_t request; request.content_len = 20; request.body = "{}";
      device_config_post_handler(&request);
      require(request.status == "408 Request Timeout" && writes == 0, "truncated body accepted");
    }},
    {"NVS failure reported without restart", [] {
      failSave = true;
      auto request = submit(R"({"ssid":"NewAP"})");
      require(request.status == "503 Service Unavailable" && !restartPending, "save failure ignored");
    }},
    {"restart scheduled only after successful HTTP response", [] {
      httpd_req_t request;
      require(device_reboot_handler(&request) == ESP_OK && restartPending, "restart was not scheduled");
      restartPending = false; request.sendFailure = true;
      device_reboot_handler(&request);
      require(!restartPending, "restart scheduled before successful response");
    }}
  };
  unsigned failed = 0;
  for (const auto& check : checks) {
    reset();
    try { check.second(); std::cout << "PASS: " << check.first << '\n'; }
    catch (const std::exception& error) { ++failed; std::cout << "FAIL: " << check.first << ": " << error.what() << '\n'; }
  }
  std::cout << checks.size() - failed << "/" << checks.size() << " HTTP configuration checks passed\n";
  return failed ? 1 : 0;
}
