"""Compile real sketch serial/Wi-Fi functions against host hardware fakes."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sketch = (ROOT / "CameraWebServer.ino").read_text(encoding="utf-8")
setup = sketch[sketch.index("void setup()"):sketch.index("static void runSerialCommand")]
assert setup.index("loadWifiCredentials();") < setup.index("esp_camera_init("), "Load credentials even when camera initialization fails"
assert "WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());" in setup, "Boot must use saved credentials"
assert "WiFi serial: ssid=... pwd=..." not in setup, "Serial commands must not be advertised before initialization"
assert 'WiFi.softAP(WIFI_CONFIG_AP_SSID, NULL, 1, 0, 4)' in sketch, "Open provisioning AP is not configured"
assert 'WiFi config page: http://' in sketch and '/config' in sketch, "Provisioning page address is not printed"
assert 'shouldStartWifiConfigPortal(WiFi.status() == WL_CONNECTED' in setup, "Wi-Fi timeout does not control provisioning AP startup"
assert 'WiFi.mode(WIFI_AP_STA)' in sketch, "STA reconnect is not preserved during provisioning"
assert 'command == "wifi_config"' in sketch, "Manual Wi-Fi configuration entry is missing"
credentials = sketch[sketch.index("static const char* DEFAULT_WIFI_SSID"):sketch.index("void startCameraServer();")]
commands = sketch[sketch.index("static void runSerialCommand(String cmd)"):]
services = sketch[sketch.index("static void printSerialCommands()"):sketch.index("void setup()")]
assert "ESP32-CAM网页服务地址:" in services, "ESP32-CAM web service address must be printed at startup"
assert "MCP上位机服务IP:" in services, "MCP host service address must be printed at startup"
assert services.count("printSerialCommands();") == 1, "Startup command list must be printed once after initialization"
compiler = os.environ.get("CXX") or shutil.which("g++") or shutil.which("clang++")
if not compiler:
    raise SystemExit("Set CXX or install a host C++ compiler to run the tests.")
with tempfile.TemporaryDirectory(prefix="camera-serial-wifi-") as tmp:
    directory = Path(tmp)
    (directory / "sketch_under_test.h").write_text(credentials + "\n" + services + "\n" + commands, encoding="utf-8")
    executable = directory / "serial_wifi_tests.exe"
    subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-I", str(directory), "-I", str(ROOT), str(ROOT / "tests" / "serial_wifi_tests.cpp"),
                    "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
