#!/usr/bin/env python3
"""A guest store of the gcm put word reaches GCM's put hook, from lifted code's
inline stores and from the runtime's out-of-line vm_write32; other stores do not.

Lifts one `stw r4,0(r3); blr` function with the real lifter, then builds it with
the real runtime/ppu/ppu_loader.cpp and runtime/ppu/tests/test_ppu_put_store_kick.cpp.
"""
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    if os.name == 'nt':
        print('SKIP: fixture uses POSIX mmap')
        return
    with tempfile.TemporaryDirectory(prefix='ppu-put-store-kick-') as tmp:
        tmp = Path(tmp)
        (tmp / 'code.bin').write_bytes(bytes.fromhex('90830000' '4e800020'))   # stw r4,0(r3); blr
        (tmp / 'functions.json').write_text(json.dumps([{'start': '0x10000', 'end': '0x10008'}]))
        out = tmp / 'lift'
        subprocess.run([sys.executable, str(ROOT / 'tools/ppu_lifter.py'), str(tmp / 'code.bin'), '--raw',
                        '--base', '0x10000', '--functions', str(tmp / 'functions.json'), '--jobs', '1',
                        '--single-file', '--output', str(out)],
                       check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        flags = ['-O1', '-w', '-ffunction-sections', '-fdata-sections', '-pthread']
        linker = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections', '-ldl']
        # Lifted sources are C++ (as lift_selftest.py builds them).
        subprocess.run([os.environ.get('CXX', 'c++'), *flags, '-std=c++17', '-x', 'c++',
                        '-I' + str(out), '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'runtime/ppu'),
                        '-c', str(out / 'ppu_recomp.c'), '-o', str(tmp / 'lifted.o')], check=True)
        subprocess.run([os.environ.get('CXX', 'c++'), *flags, '-std=c++17', '-I' + str(out),
                        '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'runtime/platform'),
                        str(ROOT / 'runtime/ppu/ppu_loader.cpp'),
                        str(ROOT / 'runtime/ppu/tests/test_ppu_put_store_kick.cpp'),
                        str(tmp / 'lifted.o'), *linker, '-o', str(tmp / 'test')], check=True)
        env = {k: v for k, v in os.environ.items() if not k.startswith(('PPU_', 'PS3_', 'PS3RECOMP_', 'SPU_'))}
        r = subprocess.run([str(tmp / 'test')], env=env, capture_output=True, text=True, timeout=30)
        print(r.stdout, end='')
        if r.returncode:
            sys.stderr.write(r.stderr[-2000:])
            raise SystemExit('FAIL: put-word stores do not reach GCM')
    print('PASS')


if __name__ == '__main__':
    main()
