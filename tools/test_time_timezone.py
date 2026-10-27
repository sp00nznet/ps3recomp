#!/usr/bin/env python3
"""Run deterministic POSIX timezone syscall tests without a game or GPU."""
import os
import platform
import subprocess
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: timezone fixture requires the POSIX toolchain')
    raise SystemExit(0)

root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='ps3-timezone-') as tmp:
    binary=Path(tmp)/'test'
    linker='-Wl,-dead_strip' if platform.system()=='Darwin' else '-Wl,--gc-sections'
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-O1','-UNDEBUG',
                    '-ffunction-sections','-fdata-sections',linker,'-pthread',
                    '-I'+str(root/'include'),str(root/'tests/test_time_timezone.c'),
                    '-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10)
