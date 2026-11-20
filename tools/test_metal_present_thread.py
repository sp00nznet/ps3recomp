#!/usr/bin/env python3
"""The opt-in present thread (PS3RECOMP_RSX_PRESENT_THREAD=1) on real Metal:
stage on the presenting thread, show on the present thread, GPU order against
a later write, drop-oldest without blocking, shutdown discard, and the
windowed show on a detached CAMetalLayer with a resize.
Synthetic 2x1 surfaces only."""
import pathlib, platform, subprocess, tempfile
ROOT = pathlib.Path(__file__).resolve().parents[1]
def main():
    if platform.system() != "Darwin":
        print("SKIP: the Metal present thread fixture requires macOS"); return
    with tempfile.TemporaryDirectory(prefix="ps3recomp-metal-present-thread-") as tmp:
        binary = pathlib.Path(tmp) / "fixture"
        subprocess.run(["clang", "-fobjc-arc", "-I", "include", "-I", "libs/video",
                        "libs/video/tests/test_metal_present_thread.m", "libs/video/rsx_texture_layout.c",
                        "-Wl,-dead_strip", "-framework", "Metal", "-framework", "Cocoa",
                        "-framework", "QuartzCore", "-o", str(binary)], cwd=ROOT, check=True, timeout=120)
        p = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
        print(p.stdout, end=""); print(p.stderr, end="")
        p.check_returncode()
        if "SKIP" in p.stdout:
            return
        lines = [x for x in p.stderr.splitlines() if x.startswith("[rsx present-thread]")]
        assert any(" on: 3 staging slots" in x for x in lines), lines
        final = [x for x in lines if " final " in x]
        assert final and "discarded=2" in final[-1] and "dropped=3" in final[-1], final
        p = subprocess.run([str(binary), "layer"], capture_output=True, text=True, timeout=60)
        print(p.stdout, end=""); print(p.stderr, end="")
        p.check_returncode()
        assert any(" on: 2 staging slots" in x for x in p.stderr.splitlines()), p.stderr
if __name__ == "__main__": main()
