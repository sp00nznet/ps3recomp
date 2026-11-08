#!/usr/bin/env python3
"""The legacy D3D12 path's draw-drop counters, with no device (Windows).

Builds libs/video/tests/test_d3d12_drop_stats.c (which includes
rsx_d3d12_backend.c) with clang-cl and runs it twice: with
D3D12_DROP_STATS_EVERY=1 the [D3D12 drops] line must name the counted
reasons; unset, nothing may be printed. Skips off Windows.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ["libs/video/tests/test_d3d12_drop_stats.c"] + ["libs/video/" + n + ".c" for n in (
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
        exe = Path(tmp) / "test_d3d12_drop_stats.exe"
        cmd = [cc, "/nologo", "/std:c17", "/O1", "/MT", "/w", "/D_CRT_SECURE_NO_WARNINGS",
               "/Iinclude", "/Ilibs/video", "/Ilibs", "/Iruntime", "/I.", f"/Fo{tmp}\\",
               *SOURCES, f"/Fe{exe}", "/link", "d3d12.lib", "dxgi.lib", "dxguid.lib", "d3dcompiler.lib",
               "user32.lib", "gdi32.lib", "ole32.lib"]
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
        if r.returncode:
            print("FAIL: fixture did not build")
            print((r.stdout + r.stderr)[-6000:])
            return 1
        env = {k: v for k, v in os.environ.items() if k != "D3D12_DROP_STATS_EVERY"}
        quiet = subprocess.run([str(exe)], cwd=tmp, capture_output=True, text=True, timeout=60, env=env)
        sys.stderr.write(quiet.stderr)
        if quiet.returncode or "[D3D12 drops]" in quiet.stdout:
            print("FAIL: unset D3D12_DROP_STATS_EVERY must print nothing")
            return 1
        env["D3D12_DROP_STATS_EVERY"] = "1"
        loud = subprocess.run([str(exe)], cwd=tmp, capture_output=True, text=True, timeout=60, env=env)
        line = next((l for l in loud.stdout.splitlines() if l.startswith("[D3D12 drops]")), "")
        print(line)
        want = ("draw_calls=5", "not_ready=2", "bad_count=2", "no_vp_resources=1")
        if loud.returncode or not all(w in line for w in want):
            print("FAIL: the report must list", ", ".join(want))
            return 1
        print("PASS")
        return 0


if __name__ == "__main__":
    sys.exit(main())
