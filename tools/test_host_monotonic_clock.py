#!/usr/bin/env python3
"""The PPU timebase and the lv2 timers read one monotonic host clock (host_monotonic_ns)."""
import os
import platform
import subprocess
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: host_monotonic_ns is the POSIX clock (Windows uses QueryPerformanceCounter)')
    raise SystemExit(0)

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='ps3-host-clock-') as tmp:
    binary = Path(tmp) / 'test'
    linker = '-Wl,-dead_strip' if platform.system() == 'Darwin' else '-Wl,--gc-sections'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                    '-ffunction-sections', '-fdata-sections', linker, '-pthread',
                    '-I' + str(root / 'include'), str(root / 'tests/test_host_monotonic_clock.c'),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=60)
