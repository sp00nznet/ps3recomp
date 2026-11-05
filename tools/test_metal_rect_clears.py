#!/usr/bin/env python3
"""Check that the Metal backend's rectangle clears leave the rest of the target."""
import pathlib
import platform
import subprocess
import tempfile
ROOT = pathlib.Path(__file__).resolve().parents[1]
if platform.system() != "Darwin":
    print("SKIP: Metal requires macOS")
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix="metal-rect-clears-") as tmp:
    binary = pathlib.Path(tmp) / "test"
    subprocess.run(["clang", "-fobjc-arc", "-Iinclude", "-Ilibs/video",
                    "libs/video/tests/test_metal_rect_clears.m",
                    "libs/video/rsx_texture_layout.c", "-Wl,-dead_strip",
                    "-framework", "Metal", "-framework", "Cocoa",
                    "-framework", "QuartzCore", "-o", str(binary)],
                   cwd=ROOT, check=True, timeout=60)
    subprocess.run([str(binary)], check=True, timeout=20)
