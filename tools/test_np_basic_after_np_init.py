#!/usr/bin/env python3
"""sceNpBasic is usable after sceNpInit alone.

A title calls sceNpInit and then sceNpBasic*; many never call
sceNpBasicInit. sceNpBasic checked only its own init flag, so every
presence, message and handler call of such a title returned
SCE_NP_BASIC_ERROR_NOT_INITIALIZED (0x8002AA01). This builds
libs/network/tests/test_np_basic_after_np_init.c with its second file
(sceNp.c) and runs it.

It needs a host C compiler (under CI=1 a missing one fails instead of
skipping).

Usage:
    python3 tools/test_np_basic_after_np_init.py
"""

import os
import shutil
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
TESTS = os.path.join(ROOT, "libs", "network", "tests")
SOURCES = [os.path.join(TESTS, "test_np_basic_after_np_init.c"),
           os.path.join(TESTS, "test_np_basic_after_np_init_np.c")]


def _cc():
    return (shutil.which(os.environ.get("CC", "cc")) or shutil.which("clang")
            or shutil.which("gcc"))


def main():
    cc = _cc()
    if cc is None:
        if os.environ.get("CI"):
            print("FAIL: no C compiler found")
            return 1
        print("skip test_np_basic_after_np_init: no C compiler")
        return 0
    with tempfile.TemporaryDirectory() as td:
        exe = os.path.join(td, "test_np_basic_after_np_init")
        cmd = [cc, "-std=gnu17", "-w", "-I" + os.path.join(ROOT, "include"),
               "-o", exe] + SOURCES
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print("FAIL: fixture did not build")
            print((r.stdout + r.stderr)[-4000:])
            return 1
        r = subprocess.run([exe], capture_output=True, text=True)
        sys.stdout.write(r.stdout)
        return r.returncode


if __name__ == "__main__":
    sys.exit(main())
