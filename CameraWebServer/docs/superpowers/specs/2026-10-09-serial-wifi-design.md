# Serial Wi-Fi design

User approved: save configuration, apply on reboot; no automatic reconnect or flashing.

- Commands: ssid=value, pwd=value, ssid?, pwd?; values preserve case, spaces and equals.
- 115200 baud, CR/LF/CRLF terminators. SSID 1-32 bytes, password 0-64 bytes.
- Save to Preferences/NVS, retain defaults when absent, query stored configuration.
- Empty password supports open networks. Query returns clear-text password as requested.
- Keep existing commands. Discard entire overflowing lines. Report storage failures.
- Load before camera initialization; preserve configuration if Wi-Fi fails.
- Host tests plus actual ESP32 build; do not flash hardware.
