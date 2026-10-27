#!/usr/bin/env python3
"""cellHttp response getters and cellHttpRecvResponse into guest memory.

cellHttpResponseGetStatusCode, cellHttpResponseGetContentLength and
cellHttpResponseGetAllHeaders did not exist, so a title's calls took the
unresolved-NID path (status unset, a NULL header array). And
cellHttpRecvResponse recv()'d a body that outgrew the header read into the
host address equal to the guest buffer's EA. This builds
libs/network/tests/test_http_response_getters.c, which talks to a loopback
listener, and runs it.

It needs a host C compiler (under CI=1 a missing one fails instead of
skipping).

Usage:
    python3 tools/test_http_response_getters.py
"""

import os
import shutil
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
TESTS = os.path.join(ROOT, "libs", "network", "tests")
SOURCES = [os.path.join(TESTS, "test_http_response_getters.c")]


def _cc():
    return (shutil.which(os.environ.get("CC", "cc")) or shutil.which("clang")
            or shutil.which("gcc"))


def main():
    cc = _cc()
    if cc is None:
        if os.environ.get("CI"):
            print("FAIL: no C compiler found")
            return 1
        print("skip test_http_response_getters: no C compiler")
        return 0
    with tempfile.TemporaryDirectory() as td:
        exe = os.path.join(td, "test_http_response_getters")
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
