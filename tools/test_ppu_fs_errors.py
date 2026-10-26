#!/usr/bin/env python3
"""cellFs error codes, seek errors, file times and directory dots (runtime/ppu/ppu_fs.cpp).

An fd that is not open is CELL_EBADF and a full fd table CELL_EMFILE (CELL_FS_EIO
used to be 0x80010001, which is CELL_EAGAIN); cellFsLseek answers CELL_EINVAL for
an unknown whence or a seek before the start and leaves the offset alone;
cellFsStat / cellFsFstat report the host file's times; "." and ".." are listed as
directories. This builds runtime/ppu/tests/test_fs_errors.cpp (which includes
ppu_fs.cpp) against a ppu_recomp.h made from the lifter's own HEADER_PREAMBLE and
runs it.

It needs a host C++ compiler (under CI=1 a missing one fails instead of
skipping).

Usage:
    python3 tools/test_ppu_fs_errors.py [--exe-out PATH] [--build-only]
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
FIXTURE = os.path.join(ROOT, "runtime", "ppu", "tests", "test_fs_errors.cpp")
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
        print("skip test_ppu_fs_errors: no C++ compiler")
        return 0
    with tempfile.TemporaryDirectory() as td:
        with open(os.path.join(td, "ppu_recomp.h"), "w") as fh:
            fh.write(_stub_header())
        exe = args.exe_out or os.path.join(td, "test_fs_errors")
        cmd = [cxx, "-std=gnu++17", "-w", "-I" + td]
        cmd += ["-I" + os.path.join(ROOT, d) for d in INCLUDE_DIRS]
        cmd += ["-o", exe, FIXTURE]
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
