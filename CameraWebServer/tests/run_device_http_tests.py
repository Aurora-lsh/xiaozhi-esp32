"""Execute actual ESP32 configuration HTTP handlers with host request fakes."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "app_httpd.cpp").read_text(encoding="utf-8")
handlers = source[source.index("static esp_err_t device_json_response("):source.index("static uint8_t g_activeChannel(")]
compiler = os.environ.get("CXX") or shutil.which("g++") or shutil.which("clang++")
library = Path(os.environ.get("ARDUINOJSON_INCLUDE") or Path.home() / "Documents/Arduino/libraries/ArduinoJson/src")
if not compiler or not (library / "ArduinoJson.h").exists():
    raise SystemExit("Install a host C++ compiler and set ARDUINOJSON_INCLUDE to ArduinoJson/src if needed.")
with tempfile.TemporaryDirectory(prefix="camera-http-config-") as temporary:
    directory = Path(temporary)
    (directory / "device_http_under_test.h").write_text(handlers, encoding="utf-8")
    executable = directory / "device_http_tests.exe"
    subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-I", str(directory), "-I", str(ROOT), "-I", str(library),
                    str(ROOT / "tests/device_http_tests.cpp"), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
