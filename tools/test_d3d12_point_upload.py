#!/usr/bin/env python3

"""Point upload and constant rows through the real D3D12 backend."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ["libs/video/tests/test_d3d12_point_upload.c"] + ["libs/video/" + n + ".c" for n in (
    "rsx_commands", "rsx_dispatch", "rsx_texture_layout", "rsx_vertex_compact", "rsx_vertex_fetch",
    "rsx_vp_decompiler", "rsx_fp_decompiler", "rsx_tiling")]


def main():
    if os.name != "nt":
        print("SKIP: D3D12 needs Windows")
        return 0
    cc = shutil.which(os.environ.get("CC", "clang-cl"))
    if not cc:
        print("FAIL: clang-cl is required (run from an x64 MSVC developer environment)")
        return 1
    with tempfile.TemporaryDirectory(prefix="d3d12-drops-") as tmp:
        exe = Path(tmp) / "test_d3d12_point_upload.exe"
        cmd = [cc, "/nologo", "/std:c17", "/O1", "/MT", "/w", "/D_CRT_SECURE_NO_WARNINGS",
               "/Iinclude", "/Ilibs/video", "/Ilibs", "/Iruntime", "/I.", f"/Fo{tmp}\\",
               *SOURCES, f"/Fe{exe}", "/link", "d3d12.lib", "dxgi.lib", "dxguid.lib", "d3dcompiler.lib",
               "user32.lib", "gdi32.lib", "ole32.lib"]
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
        if r.returncode:
            print("FAIL: fixture did not build")
            print((r.stdout + r.stderr)[-6000:])
            return 1
        result = subprocess.run([str(exe)], cwd=tmp, capture_output=True, text=True, timeout=60)
        print(result.stdout, end=""); print(result.stderr, end="")
        return result.returncode



if __name__ == "__main__":
    sys.exit(main())
