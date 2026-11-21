#!/usr/bin/env python3
"""Exercise TOC preservation through the real dispatcher, without game code."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    if os.name == 'nt':
        print('SKIP: standalone dispatcher fixture requires a POSIX toolchain')
        return 0
    with tempfile.TemporaryDirectory(prefix='ppu-hle-toc-') as directory:
        tmp = Path(directory)
        (tmp / 'ppu_recomp.h').write_text('#include "ppu_context.h"\n')
        includes = ['-I' + str(p) for p in (ROOT / 'include', ROOT / 'runtime/ppu', tmp)]
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O1', '-UNDEBUG',
                        *includes, str(ROOT / 'tests/test_ppu_hle_toc.cpp'),
                        str(ROOT / 'runtime/ppu/ppu_hle.cpp'), '-pthread',
                        '-o', str(tmp / 'fixture')], check=True, timeout=45)
        env = {k: v for k, v in os.environ.items()
               if k in ('PATH', 'HOME', 'TMPDIR', 'SYSTEMROOT')}
        subprocess.run([str(tmp / 'fixture')], env=env, check=True, timeout=10)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
