#!/usr/bin/env python3
"""Synthetic PPU cellFsUnlink host deletion test; no game files required."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    compiler = shutil.which(os.environ.get('CXX', 'c++')) or shutil.which('clang++')
    if not compiler:
        print('No C++ compiler found')
        return 1 if os.environ.get('CI') else 0
    msvc = Path(compiler).stem.lower() in ('clang-cl', 'cl')
    with tempfile.TemporaryDirectory(prefix='ppu-fs-unlink-') as temp:
        work = Path(temp)
        (work / 'ppu_recomp.h').write_text('#include "runtime/ppu/ppu_context.h"\n')
        includes = [work, ROOT, ROOT / 'include', ROOT / 'runtime/ppu']
        binary = work / ('fixture.exe' if os.name == 'nt' else 'fixture')
        fixture = ROOT / 'runtime/ppu/tests/test_fs_unlink.cpp'
        if msvc:
            flags = ['/nologo', '/O2', '/Gy', '/Gw', '/MT', '/W0',
                     '/D_CRT_SECURE_NO_WARNINGS', '/D_CRT_NONSTDC_NO_DEPRECATE']
            flags += ['/I' + str(path) for path in includes]
            commands = [[compiler, *flags, '/std:c++17', '/EHsc', str(fixture),
                         '/Fo' + str(work) + '\\', '/Fe' + str(binary), '/link', '/OPT:REF']]
        else:
            flags = ['-O2', '-w'] + ['-I' + str(path) for path in includes]
            commands = [[compiler, *flags, '-std=c++17', str(fixture), '-o', str(binary)]]
        for command in commands:
            result = subprocess.run(command, capture_output=True, text=True, timeout=90)
            if result.returncode:
                print((result.stdout + result.stderr)[-5000:])
                return result.returncode
        result = subprocess.run([str(binary), str(work / 'private')], timeout=10)
        if result.returncode:
            print('FAIL: filesystem regression exit', result.returncode)
        return result.returncode


if __name__ == '__main__':
    raise SystemExit(main())
