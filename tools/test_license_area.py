#!/usr/bin/env python3
"""Check the licence-area system parameter: North America by default, $PS3_LICENSE_AREA overrides."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


if sys.platform == "win32":
    print("SKIP: licence area fixture requires a POSIX C toolchain")
    raise SystemExit(0)

root = Path(__file__).resolve().parents[1]
env = {key: value for key, value in os.environ.items()
       if key != "PS3_LICENSE_AREA"}
gc_flag = "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"
with tempfile.TemporaryDirectory(prefix="license-area-") as temporary:
    binary = Path(temporary) / "test_license_area"
    subprocess.run([
        os.environ.get("CC", "cc"), "-std=gnu17", "-O1", "-UNDEBUG",
        "-ffunction-sections", "-fdata-sections", "-I" + str(root / "include"),
        str(root / "libs/system/tests/test_license_area.c"),
        str(root / "libs/system/cellSysutil.c"),
        "-pthread", gc_flag, "-o", str(binary),
        *(["-framework", "CoreFoundation"] if sys.platform == "darwin" else []),   # cellSysutil.c reads the host language
    ], check=True, timeout=60, env=env, cwd=temporary)
    subprocess.run([str(binary)], check=True, timeout=10, env=env, cwd=temporary)
