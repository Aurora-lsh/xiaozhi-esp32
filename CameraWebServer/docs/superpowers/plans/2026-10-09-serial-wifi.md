# Serial Wi-Fi Implementation Plan

**Goal:** Add persistent serial Wi-Fi settings, applied on reboot.
**Architecture:** Preferences in the existing sketch; host tests compile real functions with hardware fakes.
**Tech Stack:** ESP32 Arduino Core 2.0.14, Preferences, C++, Python.

- [x] Test actual functions for case, whitespace, bounds, persistence errors, reload, delimiters and overflow.
- [x] Fix boundary handling, load before camera initialization, preserve diagnostics.
- [x] Update README with commands, clear-text warning and Flash erase behavior.
- [x] Run python tests/run_serial_wifi_tests.py and build_flash.ps1 -CompileOnly.

Not a Git repository; no commit. Do not flash hardware.

