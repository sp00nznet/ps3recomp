#!/usr/bin/env python3
"""cellFsAioRead / cellFsAioWrite on an fd shared with plain reads and other threads.

An AIO read names its own offset. runtime/ppu/ppu_fs.cpp did a bare seek and
read on the fd's FILE*, so the next cellFsRead continued from wherever the
AIO stopped, and a seek from another thread could land between the two.
This builds runtime/ppu/tests/test_fs_aio.cpp (which includes ppu_fs.cpp)
against a ppu_recomp.h made from the lifter's own HEADER_PREAMBLE and runs
it: the fd's position after an AIO read, AIO reads racing from four
threads, an AIO write read back, and reads and writes drawing distinct ids
(cellFsAioWrite had a counter of its own, so the two could share one).

It needs a host C++ compiler (under CI=1 a missing one fails instead of
skipping).

Usage:
    python3 tools/test_ppu_fs_aio.py [--exe-out PATH] [--build-only]
"""

import argparse
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
FIXTURE = os.path.join(ROOT, "runtime", "ppu", "tests", "test_fs_aio.cpp")
INCLUDE_DIRS = ["include", "runtime/ppu", "runtime/memory", "runtime", "libs", "."]


def _cxx():
    return (shutil.which(os.environ.get("CXX", "c++")) or shutil.which("g++")
            or shutil.which("clang++"))


def _stub_header():
    spec = importlib.util.spec_from_file_location(
        "ppu_lifter", os.path.join(TOOLS, "ppu_lifter.py"))
    lifter = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(lifter)
    return lifter.HEADER_PREAMBLE


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe-out", help="keep the built test program at this path")
    ap.add_argument("--build-only", action="store_true",
                    help="build (for another machine, e.g. a Windows target) and stop")
    args = ap.parse_args()

    cxx = _cxx()
    if cxx is None:
        if os.environ.get("CI"):
            print("FAIL: no C++ compiler found")
            return 1
        print("skip test_ppu_fs_aio: no C++ compiler")
        return 0
    with tempfile.TemporaryDirectory() as td:
        with open(os.path.join(td, "ppu_recomp.h"), "w") as fh:
            fh.write(_stub_header())
        exe = args.exe_out or os.path.join(td, "test_fs_aio")
        cmd = [cxx, "-std=gnu++17", "-w", "-I" + td]
        cmd += ["-I" + os.path.join(ROOT, d) for d in INCLUDE_DIRS]
        cmd += ["-o", exe, FIXTURE, "-pthread"]
        if "mingw" in os.path.basename(cxx):
            cmd += ["-static"]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print("FAIL: fixture did not build")
            print((r.stdout + r.stderr)[-4000:])
            return 1
        if args.build_only:
            print(f"built {exe}")
            return 0
        r = subprocess.run([exe, td], capture_output=True, text=True)
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
        return r.returncode


if __name__ == "__main__":
    sys.exit(main())
