#!/usr/bin/env python3
"""cellGifDec / cellJpgDec guest struct layouts against Sony's decoders (tests/test_imgdec_layouts.c)."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
sources = [root / 'tests/test_imgdec_layouts.c', root / 'libs/codec/cellGifDec.c', root / 'libs/codec/cellJpgDec.c']
includes = ['include', 'runtime', 'libs', 'libs/codec', 'libs/filesystem']

with tempfile.TemporaryDirectory(prefix='imgdec-layouts-') as tmp:
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
