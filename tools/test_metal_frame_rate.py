#!/usr/bin/env python3
"""Frame-rate contract, real GPU readback, and AppKit timer on macOS."""
import pathlib, platform, subprocess, tempfile
ROOT=pathlib.Path(__file__).resolve().parents[1]
if platform.system()!='Darwin':
    print('SKIP: Metal requires macOS'); raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='metal-fps-') as tmp:
    binary=pathlib.Path(tmp)/'test'
    subprocess.run(['clang','-fobjc-arc','-I','include','-I','libs/video','libs/video/tests/test_metal_frame_rate.m','libs/video/rsx_texture_layout.c','-Wl,-dead_strip','-framework','Metal','-framework','Cocoa','-framework','QuartzCore','-o',str(binary)],cwd=ROOT,check=True,timeout=60)
    for mode in ('stats','callback_contract','headless_shutdown','window','drawable','disabled'):
        subprocess.run([str(binary),mode],check=True,timeout=20)
