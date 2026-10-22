#!/usr/bin/env python3
"""Raw SPU start through the real spu_raw.c: SPU_RunCntl runs the program on a host thread of its own."""
import os
import subprocess
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: fixture requires the POSIX compiler/linker')
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
spu = root / 'runtime/spu'
env = {k: v for k, v in os.environ.items() if not k.startswith('SPU_')}
with tempfile.TemporaryDirectory(prefix='spu-raw-start-') as tmp:
    binary = Path(tmp) / 'test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                    '-I'+str(root/'include'), '-I'+str(spu), '-I'+str(root/'runtime/platform'),
                    str(spu/'tests/test_spu_raw_start.c'), '-pthread', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=30, cwd=tmp, env=env,
                   stderr=subprocess.DEVNULL if os.environ.get('CI') else None)
