#!/usr/bin/env python3
"""Progress-bar calls check the open dialog and its bar count (libs/system/tests/test_msgdialog_progress_bars.c)."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


if sys.platform == "win32":
    print("SKIP: progress-bar fixture requires a POSIX C toolchain")
    raise SystemExit(0)

root = Path(__file__).resolve().parents[1]
env = {key: value for key, value in os.environ.items()
       if not key.startswith("MSGDIALOG_")}
gc_flag = "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"
with tempfile.TemporaryDirectory(prefix="msgdialog-bars-") as temporary:
    binary = Path(temporary) / "test_msgdialog_progress_bars"
    subprocess.run([
        os.environ.get("CC", "cc"), "-std=gnu17", "-O1", "-UNDEBUG",
        "-ffunction-sections", "-fdata-sections", "-I" + str(root / "include"),
        str(root / "libs/system/tests/test_msgdialog_progress_bars.c"),
        str(root / "libs/system/cellSysutil.c"),
        "-pthread", gc_flag, "-o", str(binary),
    ], check=True, timeout=60, env=env, cwd=temporary)
    subprocess.run([str(binary)], check=True, timeout=10, env=env, cwd=temporary)
