#!/usr/bin/env python3
"""sys_timer_usleep on POSIX with and without PS3RECOMP_USLEEP_SPIN_US
(tests/test_usleep_yield.c). Each mode runs in its own process."""
import os
import platform
import subprocess
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: the opt-in yield is the POSIX branch (Windows already spins sub-millisecond sleeps)')
    raise SystemExit(0)

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='ps3-usleep-yield-') as tmp:
    binary = Path(tmp) / 'test'
    linker = '-Wl,-dead_strip' if platform.system() == 'Darwin' else '-Wl,--gc-sections'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                    '-ffunction-sections', '-fdata-sections', linker, '-pthread',
                    '-I' + str(root / 'include'), str(root / 'tests/test_usleep_yield.c'),
                    '-o', str(binary)], check=True)
    env = {k: v for k, v in os.environ.items() if k != 'PS3RECOMP_USLEEP_SPIN_US'}
    subprocess.run([str(binary), 'off'], env=env, check=True, timeout=60)
    subprocess.run([str(binary), 'spin'], env=dict(env, PS3RECOMP_USLEEP_SPIN_US='100'),
                   check=True, timeout=60)
