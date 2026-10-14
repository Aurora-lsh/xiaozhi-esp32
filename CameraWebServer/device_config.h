#pragma once

enum class DeviceConfigResult { Saved, Invalid, StorageFailure };

struct DeviceNetworkConfig {
  char ssid[33];
  char pwd[65];
  char mcpIp[16];
};

DeviceNetworkConfig getDeviceNetworkConfig();
bool isValidDeviceNetworkParameter(const char* name, const char* value);
DeviceConfigResult setDeviceNetworkParameter(const char* name, const char* value);
void requestDeviceRestart();
bool isDeviceProvisioningMode();
