#!/usr/bin/env python3
"""Trophy registration delivers its completion through the guest's sysutil
callbacks (libs/network/tests/test_trophy_registration.c, which compiles
sceNpTrophy.c in and links cellSysutil.c)."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    print('SKIP: the trophy registration fixture uses POSIX linking')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='trophy-registration-') as tmp:
    binary = Path(tmp) / 'test'
    linker = ['-Wl,-dead_strip', '-pthread'] if sys.platform == 'darwin' else ['-Wl,--gc-sections', '-pthread', '-lm']
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                    '-ffunction-sections', '-fdata-sections', '-I' + str(root / 'include'),
                    str(root / 'libs/network/tests/test_trophy_registration.c'),
                    str(root / 'libs/system/cellSysutil.c'), *linker,
                    '-o', str(binary)], check=True)
    result = subprocess.run([str(binary)], timeout=30, capture_output=True, text=True)
    print(result.stdout.strip())
    if result.returncode:
        print(result.stderr[-2000:])
        raise SystemExit('test_trophy_registration failed')
