#!/usr/bin/env python3
"""RSX_WALKER_PROF, the in-process FIFO walker sampler (diagnostic, macOS
arm64): armed only when set, bounded by RSX_WALKER_PROF_SECONDS even when its
nominal 1 ms sleeps are coalesced, and writes every sample on its own line
(libs/video/tests/test_gcm_walker_prof.c compiles cellGcmSys.c in)."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    print('SKIP: the GCM fixture uses POSIX linking')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='gcm-walker-prof-') as tmp:
    binary = Path(tmp) / 'test'
    linker = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections', '-pthread', '-lm']
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                    '-ffunction-sections', '-fdata-sections',
                    '-I' + str(root / 'include'), '-I' + str(root / 'libs/video'),
                    str(root / 'libs/video/tests/test_gcm_walker_prof.c'), *linker,
                    '-o', str(binary)], check=True)
    base = {k: v for k, v in os.environ.items() if not k.startswith('RSX_WALKER_PROF')}
    for mode, extra in (('off', {}),
                        ('on', {'RSX_WALKER_PROF': str(Path(tmp) / 'on.txt'),
                                'RSX_WALKER_PROF_SECONDS': '1'})):
        out = str(Path(tmp) / (mode + '.txt'))
        result = subprocess.run([str(binary), mode, out], timeout=60, capture_output=True,
                                text=True, env={**base, **extra})
        print(result.stdout.strip())
        if result.returncode:
            print(result.stderr[-2000:])
            raise SystemExit('test_gcm_walker_prof failed')
