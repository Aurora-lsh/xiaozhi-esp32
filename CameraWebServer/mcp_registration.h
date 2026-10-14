#pragma once

#include <Arduino.h>

const char* mcpDefaultBridgeIp();
uint16_t mcpBridgePort();
void startMcpRegistration(const String& bridgeIp);
