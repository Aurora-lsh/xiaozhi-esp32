#include "wifi_provisioning.h"
#include <Arduino.h>

static void (*idleCheck)() = nullptr;

static void configMonitorTask(void* parameter)
{
  (void)parameter;
  for (;;) {
    idleCheck();
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

bool startWifiConfigMonitor(void (*check)())
{
  if (check == nullptr || idleCheck != nullptr) return false;
  idleCheck = check;
  if (xTaskCreate(configMonitorTask, "wifi-config", 3072, nullptr, 1, nullptr) != pdPASS) {
    idleCheck = nullptr;
    return false;
  }
  return true;
}
