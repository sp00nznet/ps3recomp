#!/usr/bin/env python3
"""Run the real interpreter's basic double arithmetic vector semantics and existing self-test."""
import os
import subprocess
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: fixture currently requires a POSIX C compiler')
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
spu = root / 'runtime/spu'
env = {k: v for k, v in os.environ.items()
       if not k.startswith(('SPU_', 'SPURS_', 'YDKJ_', 'LBP_', 'GH3_'))}
with tempfile.TemporaryDirectory(prefix='spu-double-arithmetic-') as tmp:
    for name in ['test_spu_interpreter_double_arithmetic', 'spu_interp_selftest']:
        binary = Path(tmp) / name
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                        '-I'+str(root/'include'), '-I'+str(spu),
                        str(spu/'tests'/f'{name}.c'), str(spu/'spu_interp.c'),
                        '-lm', '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=5, env=env, cwd=tmp)
