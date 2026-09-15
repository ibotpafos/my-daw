#!/usr/bin/env python3
"""Record build environment; no secrets or dependency downloads."""
import json, os, platform, subprocess
from datetime import datetime, timezone

def run(*args):
    result = subprocess.run(args, capture_output=True, text=True)
    return (result.stdout or result.stderr).strip()
print(json.dumps({"builtAt": datetime.now(timezone.utc).isoformat(), "machine": platform.machine(),
    "os": platform.platform(), "swift": run("xcrun", "swiftc", "--version"),
    "clang": run("xcrun", "clang", "--version"), "sdk": run("xcrun", "--show-sdk-version"),
    "cmake": run("cmake", "--version").splitlines()[0], "deploymentTarget": "macOS 14.0 arm64",
    "signing": "local ad-hoc; not notarized" if os.environ.get("DAW_SIGNING_IDENTITY", "-") == "-" else "certificate signed; not notarized"}, ensure_ascii=False, indent=2))
