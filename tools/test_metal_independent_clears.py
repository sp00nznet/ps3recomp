#!/usr/bin/env python3
"""Check complete unrelated clear extents on a real macOS Metal device."""
import pathlib
import platform
import subprocess
import tempfile
ROOT = pathlib.Path(__file__).resolve().parents[1]
if platform.system() != "Darwin":
    print("SKIP: Metal requires macOS")
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix="metal-clears-") as tmp:
    binary = pathlib.Path(tmp) / "test"
    subprocess.run(["clang", "-fobjc-arc", "-Iinclude", "-Ilibs/video",
                    "libs/video/tests/test_metal_independent_clears.m",
                    "libs/video/rsx_texture_layout.c", "-Wl,-dead_strip",
                    "-framework", "Metal", "-framework", "Cocoa",
                    "-framework", "QuartzCore", "-o", str(binary)],
                   cwd=ROOT, check=True, timeout=60)
    for mode in ("independent_clears", "independent_depth_stencil"):
        subprocess.run([str(binary), mode], check=True, timeout=20)
