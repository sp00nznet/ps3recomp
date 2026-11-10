#!/usr/bin/env python3
"""The PPU reservation registry keeps one entry per distinct context.

Builds the real runtime/ppu/ppu_loader.cpp with
runtime/ppu/tests/test_ppu_resv_registry.cpp and runs each case in its own
process (the registry is process-global): a context registered after another
context was registered 256 times -- a thread slot reused by many thread starts --
must still have its reservation broken by a store-conditional, a plain store
and a line write; the same holds with eight threads registering at once; 128
distinct contexts fit; a 129th stops the process with a message instead of
running unregistered.
"""
import os
import signal
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_ppu_scaffold import stub_header  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def main():
    if os.name == 'nt':
        print('SKIP: fixture builds with the POSIX toolchain flags')
        return
    with tempfile.TemporaryDirectory(prefix='ppu-resv-registry-') as tmp:
        tmp = Path(tmp)
        (tmp / 'ppu_recomp.h').write_text(stub_header())
        binary = tmp / 'test'
        flags = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections', '-ldl']
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O1', '-w', '-pthread',
                        '-ffunction-sections', '-fdata-sections',
                        '-I' + str(tmp), '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'runtime/platform'),
                        str(ROOT / 'runtime/ppu/ppu_loader.cpp'),
                        str(ROOT / 'runtime/ppu/tests/test_ppu_resv_registry.cpp'),
                        *flags, '-o', str(binary)], check=True)
        env = {k: v for k, v in os.environ.items() if not k.startswith(('PPU_', 'PS3_', 'PS3RECOMP_', 'SPU_', 'RESV_'))}
        failed = []
        for mode in ('baseline', 'churn', 'concurrent', 'capacity'):
            r = subprocess.run([str(binary), mode], env=env, capture_output=True, text=True, timeout=60)
            print(r.stdout, end='')
            if r.returncode:
                sys.stderr.write(r.stderr[-2000:])
                failed.append(mode)
        r = subprocess.run([str(binary), 'overflow'], env=env, capture_output=True, text=True, timeout=60)
        print(r.stdout, end='')
        if r.returncode == -signal.SIGABRT and 'reservation registry exhausted: capacity=128' in r.stderr:
            print('[PASS] a 129th distinct context stops the process with a message')
        else:
            print(f'[FAIL] a 129th distinct context: exit {r.returncode}, stderr {r.stderr.strip()[-300:]!r}')
            failed.append('overflow')
        if failed:
            raise SystemExit('FAIL: reservation registry: ' + ', '.join(failed))
        print('PASS: every distinct context stays reachable by reservation breaks')


if __name__ == '__main__':
    main()
