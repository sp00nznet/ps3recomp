#!/usr/bin/env python3
"""HLE -> guest callbacks get a stack per host thread, and a nested one starts below its caller.

Builds the real runtime/ppu/ppu_loader.cpp with
runtime/ppu/tests/test_ppu_guest_call_stack.cpp: two host threads are inside a
callback at the same time, each nests a second one, and every callback fills
its frame below its stack pointer; afterwards each frame must still hold what
its own callback wrote.
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_ppu_scaffold import stub_header  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def main():
    if os.name == 'nt':
        print('SKIP: fixture uses POSIX threads and mmap')
        return
    with tempfile.TemporaryDirectory(prefix='ppu-guest-call-stack-') as tmp:
        tmp = Path(tmp)
        (tmp / 'ppu_recomp.h').write_text(stub_header())
        binary = tmp / 'test'
        flags = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections', '-pthread', '-ldl']
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O1', '-w',
                        '-ffunction-sections', '-fdata-sections',
                        '-I' + str(tmp), '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'runtime/platform'),
                        str(ROOT / 'runtime/ppu/ppu_loader.cpp'),
                        str(ROOT / 'runtime/ppu/tests/test_ppu_guest_call_stack.cpp'),
                        *flags, '-o', str(binary)], check=True)
        env = {k: v for k, v in os.environ.items() if not k.startswith(('PPU_', 'PS3_', 'PS3RECOMP_', 'SPU_'))}
        r = subprocess.run([str(binary)], env=env, capture_output=True, text=True, timeout=60)
        print(r.stdout, end='')
        if r.returncode:
            sys.stderr.write(r.stderr[-2000:])
            raise SystemExit('FAIL: guest callbacks share a stack')
        print('PASS: guest callbacks run on disjoint stacks')


if __name__ == '__main__':
    main()
