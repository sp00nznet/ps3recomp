#!/usr/bin/env python3
"""The PSN terms-of-service check (cellSysutilNpEula.c) answers through the
title's callback on the next sysutil poll."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


if sys.platform == "win32":
    print("SKIP: NP EULA fixture requires a POSIX C toolchain")
    raise SystemExit(0)

root = Path(__file__).resolve().parents[1]
env = dict(os.environ)
gc_flag = "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"
with tempfile.TemporaryDirectory(prefix="np-eula-") as temporary:
    binary = Path(temporary) / "test_np_eula"
    subprocess.run([
        os.environ.get("CC", "cc"), "-std=gnu17", "-O1", "-UNDEBUG",
        "-ffunction-sections", "-fdata-sections", "-I" + str(root / "include"),
        str(root / "libs/system/tests/test_np_eula.c"),
        str(root / "libs/system/cellSysutil.c"),
        "-pthread", gc_flag, "-o", str(binary),
    ], check=True, timeout=60, env=env, cwd=temporary)
    subprocess.run([str(binary)], check=True, timeout=10, env=env, cwd=temporary)
