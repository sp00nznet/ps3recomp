#!/usr/bin/env python3
"""A free EDAT decrypts with the klicensee the title passes to sceNpDrmIsAvailable[2].

On hardware a title hands its own klicensee to sceNpDrmIsAvailable or
sceNpDrmIsAvailable2 before it opens a free (license type 3) or disc-bound
EDAT; the kernel keeps it for later opens. The runtime never took that key,
so such a file failed with -6 unless its klicensee was one of the built-in
platform keys. This builds libs/filesystem/tests/test_edat_guest_klicensee.c
with its second file (sceNp.c) and runs it. The EDAT, its key and its
plaintext are synthetic, made by the test itself.

It needs a host C compiler (under CI=1 a missing one fails instead of
skipping).

Usage:
    python3 tools/test_edat_guest_klicensee.py
"""

import os
import shutil
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
TESTS = os.path.join(ROOT, "libs", "filesystem", "tests")
SOURCES = [os.path.join(TESTS, "test_edat_guest_klicensee.c"),
           os.path.join(TESTS, "test_edat_guest_klicensee_np.c")]


def _cc():
    return (shutil.which(os.environ.get("CC", "cc")) or shutil.which("clang")
            or shutil.which("gcc"))


def main():
    cc = _cc()
    if cc is None:
        if os.environ.get("CI"):
            print("FAIL: no C compiler found")
            return 1
        print("skip test_edat_guest_klicensee: no C compiler")
        return 0
    with tempfile.TemporaryDirectory() as td:
        exe = os.path.join(td, "test_edat_guest_klicensee")
        cmd = [cc, "-std=gnu17", "-w", "-I" + os.path.join(ROOT, "include"),
               "-o", exe] + SOURCES
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print("FAIL: fixture did not build")
            print((r.stdout + r.stderr)[-4000:])
            return 1
        scratch = os.path.join(td, "scratch")
        os.mkdir(scratch)
        r = subprocess.run([exe, scratch], capture_output=True, text=True)
        sys.stdout.write(r.stdout)
        return r.returncode


if __name__ == "__main__":
    sys.exit(main())
