#!/usr/bin/env python3
"""cellSync Barrier/Rwm/LFQueue guest layouts (tests/test_sync_layouts.c)."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
sources = [root / 'tests/test_sync_layouts.c', root / 'libs/sync/cellSync.c']
includes = ['include', 'runtime', 'libs', 'libs/sync']

with tempfile.TemporaryDirectory(prefix='sync-layouts-') as tmp:
    binary = Path(tmp) / 'test'
    cc = os.environ.get('CC')
    if sys.platform == 'win32' and not cc:
        cl = shutil.which('clang-cl')
        if not cl:
            print('SKIP: no clang-cl on PATH')
            raise SystemExit(0)
        cmd = [cl, '/nologo', '/std:c17', '-D_CRT_SECURE_NO_WARNINGS'] + ['-I' + str(root / i) for i in includes] \
            + [str(s) for s in sources] + ['/Fe:' + str(binary) + '.exe', '/Fo:' + tmp + '\\']
        binary = Path(str(binary) + '.exe')
    else:
        cmd = [cc or 'cc', '-std=gnu17', '-O1', '-UNDEBUG'] + ['-I' + str(root / i) for i in includes] \
            + [str(s) for s in sources] + ['-o', str(binary), '-lm']
    subprocess.run(cmd, check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
