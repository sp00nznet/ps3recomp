#!/usr/bin/env python3
"""Exercise the real libnet NID handlers with guest memory and loopback I/O."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
root=Path(__file__).resolve().parents[1]
if os.name == "nt":
    print("SKIP: use the documented clang-cl fixture command on Windows")
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix="sys-net-") as tmp:
    binary=Path(tmp)/"fixture"
    subprocess.run(["cc","-std=gnu17","-O1","-UNDEBUG","-Iinclude",
                    "libs/network/tests/test_sys_net.c","-pthread","-o",str(binary)],
                   cwd=root,check=True,timeout=60)
    subprocess.run([str(binary)],check=True,timeout=30)
