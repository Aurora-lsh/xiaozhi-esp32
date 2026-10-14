#pragma once

inline bool isValidBridgeIp(const char* text)
{
  if (text == nullptr) return false;
  const char* cursor = text;
  unsigned firstOctet = 0;
  for (unsigned component = 0; component < 4; ++component) {
    if (*cursor < '0' || *cursor > '9') return false;
    if (*cursor == '0' && cursor[1] >= '0' && cursor[1] <= '9') return false;
    unsigned octet = 0;
    unsigned digits = 0;
    while (*cursor >= '0' && *cursor <= '9') {
      octet = octet * 10 + static_cast<unsigned>(*cursor - '0');
      if (++digits > 3 || octet > 255) return false;
      ++cursor;
    }
    if (component == 0) firstOctet = octet;
    if (component < 3) {
      if (*cursor != '.') return false;
      ++cursor;
    } else if (*cursor != '\0') {
      return false;
    }
  }
  return firstOctet != 0 && firstOctet != 127 && firstOctet < 224;
}
