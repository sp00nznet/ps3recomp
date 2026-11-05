#!/usr/bin/env python3
"""A raw-SPU image registered with its parameter area masked out is found whatever that area holds."""
import os
import subprocess
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: fixture requires the POSIX compiler/linker')
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='spu-workload-masked-') as tmp:
    binary = Path(tmp) / 'test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                    '-I' + str(root / 'include'), '-I' + str(root / 'runtime'),
                    '-I' + str(root / 'runtime/spu'),
                    str(root / 'runtime/spu/tests/test_spu_workload_masked.c'),
                    '-pthread', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=30, cwd=tmp)
