"""Build the real Wi-Fi provisioning decision helper on the host."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
compiler = os.environ.get("CXX") or shutil.which("g++") or shutil.which("clang++")
if not compiler:
    raise SystemExit("Set CXX or install a host C++ compiler to run the tests.")
with tempfile.TemporaryDirectory(prefix="camera-wifi-provisioning-") as temporary:
    executable = Path(temporary) / "wifi_provisioning_tests.exe"
    subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT), str(ROOT / "tests/wifi_provisioning_tests.cpp"),
                    "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
