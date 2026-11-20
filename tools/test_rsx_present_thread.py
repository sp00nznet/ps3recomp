#!/usr/bin/env python3
"""The opt-in present thread's ring and drop policy (rsx_present_thread.h),
with synthetic hooks: no GPU, no window, no game data."""
import os, pathlib, shutil, subprocess, tempfile
ROOT = pathlib.Path(__file__).resolve().parents[1]
def main():
    if os.name == "nt":
        print("SKIP: the present thread is POSIX-only; Windows keeps the ordinary present"); return
    cc = shutil.which(os.environ.get("CC", "cc"))
    if not cc:
        raise SystemExit("C compiler required for the present thread fixture")
    with tempfile.TemporaryDirectory(prefix="rsx-present-thread-") as tmp:
        binary = pathlib.Path(tmp) / "fixture"
        subprocess.run([cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-I", "libs/video",
                        "libs/video/tests/test_rsx_present_thread.c", "-pthread", "-o", str(binary)],
                       cwd=ROOT, check=True, timeout=60)
        subprocess.run([str(binary)], check=True, timeout=60)
if __name__ == "__main__": main()
