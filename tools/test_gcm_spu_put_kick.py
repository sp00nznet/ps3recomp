#!/usr/bin/env python3
"""An SPU transfer that writes the put word wakes the FIFO walker
(PS3RECOMP_GCM_SPU_PUT_KICK=1; libs/video/tests/test_gcm_spu_put_kick.c, which compiles
cellGcmSys.c in)."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    print('SKIP: the GCM walker fixture uses POSIX linking')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='gcm-spu-put-kick-') as tmp:
    binary = Path(tmp) / 'test'
    linker = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections', '-pthread', '-lm']
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                    '-ffunction-sections', '-fdata-sections',
                    '-I' + str(root / 'include'), '-I' + str(root / 'libs/video'),
                    '-I' + str(root / 'runtime/platform'),
                    str(root / 'libs/video/tests/test_gcm_spu_put_kick.c'),
                    str(root / 'runtime/platform/win32_compat.c'), *linker, '-pthread',
                    '-o', str(binary)], check=True)
    result = subprocess.run([str(binary)], timeout=30, capture_output=True, text=True)
    print(result.stdout.strip())
    if result.returncode:
        print(result.stderr[-2000:])
        raise SystemExit('test_gcm_spu_put_kick failed')
