#!/usr/bin/env python3
"""cellGcmGetLastFlipTime: the guest system time of the last completed flip
(libs/video/tests/test_gcm_last_flip_time.c, which compiles cellGcmSys.c in)."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    print('SKIP: the GCM last flip time fixture uses POSIX linking')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='gcm-last-flip-time-') as tmp:
    binary = Path(tmp) / 'test'
    linker = ['-Wl,-dead_strip', '-pthread'] if sys.platform == 'darwin' else ['-Wl,--gc-sections', '-pthread', '-lm']
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                    '-ffunction-sections', '-fdata-sections',
                    '-I' + str(root / 'include'), '-I' + str(root / 'libs/video'),
                    str(root / 'libs/video/tests/test_gcm_last_flip_time.c'), *linker,
                    '-o', str(binary)], check=True)
    result = subprocess.run([str(binary)], timeout=60, capture_output=True, text=True)
    print(result.stdout.strip())
    if result.returncode:
        print(result.stderr[-2000:])
        raise SystemExit('test_gcm_last_flip_time failed')
