#!/usr/bin/env python3
"""The control register's get follows the walker across a JUMP
(libs/video/tests/test_gcm_get_on_jump.c, which compiles cellGcmSys.c in)."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    print('SKIP: the GCM get fixture uses POSIX linking')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='gcm-get-on-jump-') as tmp:
    binary = Path(tmp) / 'test'
    linker = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections', '-pthread', '-lm']
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                    '-ffunction-sections', '-fdata-sections',
                    '-I' + str(root / 'include'), '-I' + str(root / 'libs/video'),
                    str(root / 'libs/video/tests/test_gcm_get_on_jump.c'), *linker,
                    '-o', str(binary)], check=True)
    result = subprocess.run([str(binary)], timeout=30, capture_output=True, text=True)
    print(result.stdout.strip())
    if result.returncode:
        print(result.stderr[-2000:])
        raise SystemExit('test_gcm_get_on_jump failed')
