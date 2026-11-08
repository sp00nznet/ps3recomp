#!/usr/bin/env python3
"""Opt-in RSX FIFO consumer busy accounting (libs/video/rsx_fifo_busy.h) on a
scripted clock, built from two translation units so the header-only shared
state is checked to be one object. No GPU, no game data."""
import os, pathlib, shutil, subprocess, tempfile
ROOT = pathlib.Path(__file__).resolve().parents[1]
def main():
    if os.name == "nt":
        print("SKIP: the fixture uses POSIX threads"); return
    cc = shutil.which(os.environ.get("CC", "cc"))
    if not cc:
        raise SystemExit("C compiler required for the FIFO busy fixture")
    with tempfile.TemporaryDirectory(prefix="rsx-fifo-busy-") as tmp:
        binary = pathlib.Path(tmp) / "fixture"
        subprocess.run([cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-I", "libs/video",
                        "libs/video/tests/test_rsx_fifo_busy.c", "libs/video/tests/test_rsx_fifo_busy_tu2.c",
                        "-pthread", "-o", str(binary)], cwd=ROOT, check=True, timeout=60)
        subprocess.run([str(binary)], check=True, timeout=60)
        # Also as C++: the hooked files include it from C++ units too.
        cxx = shutil.which(os.environ.get("CXX", "c++"))
        if cxx:
            subprocess.run([cxx, "-x", "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fsyntax-only",
                            "-I", "libs/video", "libs/video/tests/test_rsx_fifo_busy_tu2.c"],
                           cwd=ROOT, check=True, timeout=60)
if __name__ == "__main__": main()
