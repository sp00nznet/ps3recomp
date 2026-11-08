#!/usr/bin/env python3
"""PPU reservation breaks with PS3RECOMP_PPU_RESV_OPEN_COUNT off and on.

Lifts lwarx and stwcx. with ppu_lifter.py --resv-open-count, builds them with
the real runtime/ppu/ppu_loader.cpp and runtime/ppu/tests/test_ppu_resv_open_count.cpp,
and runs the driver with the counts off and on (with and without
PPU_RESV_STORE=1). PPU_RESV_OFF=1 (plain value CAS) must make the stress
report violations. Also checks that a lift without --resv-open-count emits no
open-count call. No game files are used.
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ppu_lifter as L  # noqa: E402
from ppu_disasm import Instruction  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def lift(open_count):
    lifter = L.PPULifter()
    lifter.resv_open_count = open_count
    for addr, mnemonic, operands in ((0x1000, 'lwarx', 'r3,0,r4'), (0x2000, 'stwcx.', 'r5,0,r4')):
        insns = [Instruction(addr, 0, mnemonic, operands), Instruction(addr + 4, 0, 'blr', '')]
        lifter.lift_function(insns, addr, addr + 8)
    return lifter.emit_header(), lifter.emit_source()


def main():
    if os.name == 'nt':
        print('SKIP: driver builds with the POSIX toolchain')
        return
    header, source = lift(False)
    assert 'ppu_resv_open_note' not in header + source, 'default lift must not change'
    header, source = lift(True)
    assert source.count('ppu_resv_open_note(ctx, (uint32_t)ea)') == 1, 'lwarx must note its reservation'
    with tempfile.TemporaryDirectory(prefix='ppu-resv-open-count-') as tmp:
        tmp = Path(tmp)
        (tmp / 'ppu_recomp.h').write_text(header)
        (tmp / 'lifted.cpp').write_text(source)
        binary = tmp / 'test'
        flags = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections', '-ldl']
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O2', '-w', '-UNDEBUG',
                        '-ffunction-sections', '-fdata-sections',
                        '-I' + str(tmp), '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'runtime/ppu'),
                        '-I' + str(ROOT / 'runtime/platform'),
                        str(ROOT / 'runtime/ppu/ppu_loader.cpp'), str(tmp / 'lifted.cpp'),
                        str(ROOT / 'runtime/ppu/tests/test_ppu_resv_open_count.cpp'),
                        '-pthread', *flags, '-o', str(binary)], check=True)
        rounds = os.environ.get('PPU_RESV_OPEN_COUNT_ROUNDS', '200000')
        base = {k: v for k, v in os.environ.items() if not k.startswith(('PPU_', 'PS3_', 'PS3RECOMP_', 'SPU_'))}
        for counting in (False, True):
            for store in (False, True):
                env = dict(base)
                if counting:
                    env['PS3RECOMP_PPU_RESV_OPEN_COUNT'] = '1'
                if store:
                    env['PPU_RESV_STORE'] = '1'
                subprocess.run([str(binary), rounds], env=env, check=True, timeout=300)
        for counting in (False, True):
            env = dict(base, PPU_RESV_OFF='1')
            if counting:
                env['PS3RECOMP_PPU_RESV_OPEN_COUNT'] = '1'
            r = subprocess.run([str(binary), rounds], env=env, timeout=300)
            assert r.returncode == 0, 'the stress must detect store-conditionals that ignore breaks'
    print('PASS PPU open reservation counts: breaks, fibers, plain stores and stress, counts off and on')


if __name__ == '__main__':
    main()
