#!/usr/bin/env python3
"""Check Abort suppression and normal Close delivery through the sysutil pump."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


if sys.platform == "win32":
    print("SKIP: dialog abort fixture requires a POSIX C toolchain")
    raise SystemExit(0)

root = Path(__file__).resolve().parents[1]
env = {key: value for key, value in os.environ.items()
       if not key.startswith("MSGDIALOG_")}
gc_flag = "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"
with tempfile.TemporaryDirectory(prefix="dialog-abort-") as temporary:
    binary = Path(temporary) / "test_dialog_abort"
    subprocess.run([
        os.environ.get("CC", "cc"), "-std=gnu17", "-O1", "-UNDEBUG",
        "-ffunction-sections", "-fdata-sections", "-I" + str(root / "include"),
        str(root / "libs/system/tests/test_dialog_abort.c"),
        str(root / "libs/system/cellSysutil.c"),
        "-pthread", gc_flag, "-o", str(binary),
    ], check=True, timeout=60, env=env, cwd=temporary)
    subprocess.run([str(binary)], check=True, timeout=10, env=env, cwd=temporary)
