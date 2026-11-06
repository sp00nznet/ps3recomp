#!/usr/bin/env python3
"""cellPamf reader calls on a reader that was never initialised.

A getter called on a PAMF reader before (or after a failed)
cellPamfReaderInitialize was handed a fresh, zeroed slot and read the PAMF
header through its NULL pamfAddr: PlayStation All-Stars Battle Royale
(BCUS98472) crashed in cellPamfReaderGetPresentationStartTime at NULL+0x18 when
its attract movie never opened. This builds
libs/codec/tests/test_pamf_uninit_reader.c, which includes the real
cellPamf.c, and runs it.

It needs a host C compiler (under CI=1 a missing one fails instead of
skipping).

Usage:
    python3 tools/test_pamf_uninit_reader.py
"""

import os
import shutil
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
SOURCES = [os.path.join(ROOT, "libs", "codec", "tests", "test_pamf_uninit_reader.c")]


def _cc():
    return (shutil.which(os.environ.get("CC", "cc")) or shutil.which("clang")
            or shutil.which("gcc"))


def main():
    cc = _cc()
    if cc is None:
        if os.environ.get("CI"):
            print("FAIL: no C compiler found")
            return 1
        print("skip test_pamf_uninit_reader: no C compiler")
        return 0
    with tempfile.TemporaryDirectory() as td:
        exe = os.path.join(td, "test_pamf_uninit_reader")
        cmd = [cc, "-std=gnu17", "-w", "-I" + os.path.join(ROOT, "include"),
               "-o", exe] + SOURCES
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print("FAIL: fixture did not build")
            print((r.stdout + r.stderr)[-4000:])
            return 1
        r = subprocess.run([exe], capture_output=True, text=True)
        sys.stdout.write(r.stdout)
        if r.returncode < 0:
            print("FAIL: fixture crashed (signal %d)" % -r.returncode)
            return 1
        return r.returncode


if __name__ == "__main__":
    sys.exit(main())
