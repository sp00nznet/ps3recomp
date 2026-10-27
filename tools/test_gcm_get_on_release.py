#!/usr/bin/env python3
"""The control register's get is past a semaphore RELEASE when its label lands
(libs/video/tests/test_gcm_get_on_release.c, which compiles cellGcmSys.c in).
Run twice: as shipped, and with GCM_GET_EQ_PUT=1, which must keep its own get."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    print('SKIP: the GCM get fixture uses POSIX linking')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='gcm-get-on-release-') as tmp:
    binary = Path(tmp) / 'test'
    linker = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections', '-pthread', '-lm']
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                    '-ffunction-sections', '-fdata-sections',
                    '-I' + str(root / 'include'), '-I' + str(root / 'libs/video'),
                    str(root / 'libs/video/tests/test_gcm_get_on_release.c'), *linker,
                    '-o', str(binary)], check=True)
    base = {k: v for k, v in os.environ.items() if k != 'GCM_GET_EQ_PUT'}
    for args, extra in (([], {}), (['eq-put'], {'GCM_GET_EQ_PUT': '1'})):
        result = subprocess.run([str(binary), *args], timeout=30, capture_output=True, text=True,
                                env={**base, **extra})
        print(result.stdout.strip())
        if result.returncode:
            print(result.stderr[-2000:])
            raise SystemExit('test_gcm_get_on_release failed')
