#!/usr/bin/env python3
"""cellHttpCreateTransaction takes the request path from CellHttpUri+16.

The guest CellHttpUri is { scheme, hostname, username, password, path, port,
reserved } with 4-byte EAs. The path was read at +8 (the username field), so
every request went out as "GET /" whatever the title asked for. This builds
libs/network/tests/test_http_uri_path.c, which sends a request to a
loopback listener and checks the request line.

It needs a host C compiler (under CI=1 a missing one fails instead of
skipping).

Usage:
    python3 tools/test_http_uri_path.py
"""

import os
import shutil
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
TESTS = os.path.join(ROOT, "libs", "network", "tests")
SOURCES = [os.path.join(TESTS, "test_http_uri_path.c")]


def _cc():
    return (shutil.which(os.environ.get("CC", "cc")) or shutil.which("clang")
            or shutil.which("gcc"))


def main():
    cc = _cc()
    if cc is None:
        if os.environ.get("CI"):
            print("FAIL: no C compiler found")
            return 1
        print("skip test_http_uri_path: no C compiler")
        return 0
    with tempfile.TemporaryDirectory() as td:
        exe = os.path.join(td, "test_http_uri_path")
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
