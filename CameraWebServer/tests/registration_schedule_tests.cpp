#include <cstdint>
#include <iostream>
#include <stdexcept>
#include "mcp_registration_schedule.h"

static void require(bool condition, const char* message)
{
  if (!condition) throw std::runtime_error(message);
}

int main()
{
  McpRegistrationSchedule schedule;
  require(!schedule.due(false, 0, 0), "offline must not register");
  require(!schedule.due(true, 0, 1), "zero IP must not register");
  require(schedule.due(true, 10, 2), "first connection must register");
  schedule.completed(false, 2);
  require(!schedule.due(true, 10, 5001), "failed requests must be throttled");
  require(schedule.due(true, 10, 5002), "failure must retry after five seconds");
  schedule.completed(true, 5002);
  require(!schedule.due(true, 10, 65001), "success must not retry early");
  require(schedule.due(true, 10, 65002), "heartbeat must repeat after sixty seconds");
  schedule.completed(true, 65002);
  require(schedule.due(true, 11, 65003), "IP changes must register immediately");
  schedule.completed(true, 65003);
  require(!schedule.due(false, 0, 65004), "disconnect must reset registration");
  require(schedule.due(true, 11, 65005), "same IP reconnect must register");
  schedule.completed(false, UINT32_MAX - 2000);
  require(!schedule.due(true, 11, 2998), "millis wrap must preserve delay");
  require(schedule.due(true, 11, 2999), "millis wrap must allow retry");
  std::cout << "12/12 registration scheduling checks passed\n";
}
