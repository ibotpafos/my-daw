#!/usr/bin/env python3
"""Read-only local toolchain inventory. Does not install or switch tools."""
import platform
import shutil
import subprocess

print(f"Host: {platform.system()} {platform.release()} {platform.machine()}")
checks = [
    ("Developer directory", ["xcode-select", "-p"]),
    ("Full Xcode", ["xcodebuild", "-version"]),
    ("Swift", ["xcrun", "swift", "--version"]),
    ("Clang", ["xcrun", "clang", "--version"]),
    ("CMake", ["cmake", "--version"]),
    ("Python", ["python3", "--version"]),
]
for label, argv in checks:
    if not shutil.which(argv[0]):
        print(f"MISSING {label}: {argv[0]}")
        continue
    try:
        result = subprocess.run(argv, capture_output=True, text=True, timeout=15)
        status = "OK" if result.returncode == 0 else "UNAVAILABLE"
        output = (result.stdout or result.stderr).strip().splitlines()
        print(f"{status} {label}: {' / '.join(output[:2])}")
    except subprocess.TimeoutExpired:
        print(f"TIMEOUT {label}")
print("Environment inventory only. Build/test results and physical audio readiness are separate checks.")
