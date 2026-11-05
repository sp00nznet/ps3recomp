#!/usr/bin/env python3
"""SPU event_status updates (SPU_EVENT_SET/CLEAR) keep every bit when two host threads update it at once."""
import os
import subprocess
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: fixture requires the POSIX compiler and pthreads')
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
spu = root / 'runtime/spu'
with tempfile.TemporaryDirectory(prefix='spu-event-atomic-') as tmp:
    binary = Path(tmp) / 'test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O2', '-UNDEBUG',
                    '-I' + str(root / 'include'), '-I' + str(spu), '-I' + str(root / 'runtime/platform'),
                    str(spu / 'tests/test_spu_event_atomic.c'), '-pthread', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=120)
