#!/usr/bin/env python3
"""D3D12 register-file engine backend on a real adapter, headless.

Builds libs/video/tests/test_d3d12_engine_backend.c (which includes
rsx_d3d12_backend.c) with clang-cl and runs it with the D3D12 debug layer when
the Graphics Tools feature is installed; any debug-layer error fails the run.
Windows only: elsewhere, and on a host without a D3D12 device, it skips.
No window, swapchain, guest memory or game data.

Usage: python tools/test_d3d12_engine_backend.py [--build-only] [--no-validation]
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ["libs/video/tests/test_d3d12_engine_backend.c"] + ["libs/video/" + n + ".c" for n in (
    "rsx_draw_engine", "rsx_commands", "rsx_dispatch", "rsx_texture_layout", "rsx_vertex_compact",
    "rsx_vertex_fetch", "rsx_vp_decompiler", "rsx_fp_decompiler", "rsx_tiling")]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--build-only", action="store_true")
    ap.add_argument("--no-validation", action="store_true")
    args = ap.parse_args()
    if os.name != "nt":
        print("SKIP: D3D12 needs Windows")
        return 0
    cc = shutil.which(os.environ.get("CC", "clang-cl"))
    if not cc:
        print("FAIL: clang-cl is required (run from an x64 MSVC developer environment)")
        return 1
    with tempfile.TemporaryDirectory(prefix="d3d12-engine-") as tmp:
        exe = Path(tmp) / "test_d3d12_engine_backend.exe"
        cmd = [cc, "/nologo", "/std:c17", "/O1", "/Zi", "/MT", "/w", "/D_CRT_SECURE_NO_WARNINGS",
               "/Iinclude", "/Ilibs/video", "/Ilibs", "/Iruntime", "/I.", f"/Fo{tmp}\\",
               *SOURCES, f"/Fe{exe}", "/link", "d3d12.lib", "dxgi.lib", "dxguid.lib", "d3dcompiler.lib",
               "user32.lib", "gdi32.lib", "ole32.lib"]
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
        if r.returncode:
            print("FAIL: fixture did not build")
            print((r.stdout + r.stderr)[-6000:])
            return 1
        if args.build_only:
            print(f"built {exe}")
            return 0
        env = dict(os.environ)
        if not args.no_validation:
            env["D3D12_ENGINE_TEST_VALIDATION"] = "1"
        r = subprocess.run([str(exe)], cwd=tmp, capture_output=True, text=True, timeout=120, env=env)
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
        if r.returncode == 2:
            print("SKIP: no D3D12 device or debug layer")
            return 0
        return r.returncode


if __name__ == "__main__":
    sys.exit(main())
