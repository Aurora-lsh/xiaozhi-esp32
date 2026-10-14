#pragma once

#include <stdint.h>

class McpRegistrationSchedule {
public:
  bool due(bool connected, uint32_t ip, uint32_t now)
  {
    if (!connected || ip == 0) {
      online_ = false;
      attempted_ = false;
      return false;
    }
    if (!online_ || ip != ip_) {
      online_ = true;
      ip_ = ip;
      attempted_ = false;
    }
    const uint32_t interval = succeeded_ ? 60000U : 5000U;
    return !attempted_ || static_cast<uint32_t>(now - lastAttempt_) >= interval;
  }

  void completed(bool succeeded, uint32_t now)
  {
    attempted_ = true;
    succeeded_ = succeeded;
    lastAttempt_ = now;
  }

private:
  bool online_ = false;
  bool attempted_ = false;
  bool succeeded_ = false;
  uint32_t ip_ = 0;
  uint32_t lastAttempt_ = 0;
};
