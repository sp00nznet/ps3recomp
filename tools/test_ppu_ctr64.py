#!/usr/bin/env python3
"""Compile actual PPU lifter emissions to check 64-bit CTR data and counts.

A 32-bit MTCTR/MFCTR round trip loses high command flags held temporarily in
CTR. Branch-count instructions must also decrement the full register, including
borrow/wrap, while existing guest-address dispatch remains a separate operation.
This fixture contains synthetic instruction words and values only.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from ppu_disasm import Instruction, decode
from ppu_lifter import PPULifter, LiftedFunction, FENCE_PREAMBLE

ROOT = Path(__file__).resolve().parent.parent


def spr_word(xo, reg, spr=9):
    field = ((spr & 31) << 5) | (spr >> 5)
    return (31 << 26) | (reg << 21) | (field << 11) | (xo << 1)


def emitted_driver():
    lifter = PPULifter()
    func = LiftedFunction(name="ctr_test", start_addr=0x1000, end_addr=0x2000)
    def emit(ins):
        return lifter._translate_op(ins, func)
    mt = decode(spr_word(467, 10), 0x1000)
    mf = decode(spr_word(339, 9), 0x1004)
    assert mt.mnemonic == "mtctr" and mf.mnemonic == "mfctr"
    pre = lifter._preamble_lines()
    pre[0] = pre[0].replace('#include "ppu_recomp.h"',
                          '#include "ppu_context.h"\n' + FENCE_PREAMBLE)
    lines = list(pre) + ['#include <stdio.h>', '#include <inttypes.h>']
    for name, ins in [('alias', mt),
                      ('numeric', Instruction(0x1000, 0, 'mtspr', '9, r10')),
                      ('named', Instruction(0x1000, 0, 'mtspr', 'CTR, r10'))]:
        lines.append(f'static void roundtrip_{name}(ppu_context* ctx) {{ {emit(ins)} {emit(mf)} }}')
    for name, mn in [('nonzero', 'bdnz'), ('zero', 'bdz')]:
        ins = Instruction(0x1000, 0, mn, '0x1100')
        lines.append(f'static bool branch_{name}(ppu_context* ctx) {{ {emit(ins)} return false; loc_00001100: return true; }}')
    # A real emitted command-flags sequence: shift a flag into the high word,
    # hold it in CTR, clear its old source, then OR its MFCTR value into a word.
    words = [0x38002000, 0x780A07C6, spr_word(467, 10),
             0x39400000, spr_word(339, 9), 0x7D295378]
    expected = ['li', 'rldicr', 'mtctr', 'li', 'mfctr', 'or']
    insns = [decode(word, 0x1200+i*4) for i, word in enumerate(words)]
    assert [x.mnemonic for x in insns] == expected
    # OR r9,r9,r10 would be zero after the clear; use a separate command value.
    insns[-1] = decode(0x7C004B78, 0x1214)  # or r0,r0,r9
    lines.append('static void flag_chain(ppu_context* ctx) { ' + ' '.join(emit(x) for x in insns) + ' }')
    lifter.jump_tables[0x1300] = [0x1400]
    dispatch = emit(Instruction(0x1300, 0, 'bctr', ''))
    lines.append('extern "C" void ps3_indirect_call(ppu_context*) {}')
    lines.append('static bool address_dispatched; static void dispatch_address(ppu_context* ctx) { '+dispatch+' loc_00001400: address_dispatched = true; }')
    lines.append(r'''
static unsigned checks, failures;
static void check(const char* name, uint64_t got, uint64_t want) {
    ++checks;
    if (got != want) {
        ++failures;
        fprintf(stderr, "FAIL %s got=%016" PRIx64 " want=%016" PRIx64 "\n", name, got, want);
    }
}
int main() {
    const uint64_t values[] = {0, 1, 0xFFFFFFFF, 0x100000000, 0x100000001,
        0x200000000000, 0x1234567887654321, 0x8000000000000000, UINT64_MAX};
    void (*roundtrips[])(ppu_context*) = {roundtrip_alias, roundtrip_numeric, roundtrip_named};
    for (auto roundtrip : roundtrips) for (uint64_t value : values) {
        ppu_context ctx{}; ctx.gpr[10] = value; ctx.cr = 0xFEDCBA98; ctx.lr = 0x9876543210123456;
        roundtrip(&ctx);
        check("CTR roundtrip", ctx.ctr, value);
        check("MFCTR roundtrip", ctx.gpr[9], value);
        check("unrelated CR", ctx.cr, 0xFEDCBA98);
        check("unrelated LR", ctx.lr, 0x9876543210123456);
    }
    ppu_context flags{}; flag_chain(&flags);
    check("high command flag retained", flags.gpr[0], 0x200000002000);
    for (uint64_t value : values) {
        ppu_context ctx{}; ctx.ctr = value;
        bool taken = branch_nonzero(&ctx);
        check("BDNZ full decrement", ctx.ctr, value-1);
        check("BDNZ condition", taken, (value-1)!=0);
        ctx.ctr = value; taken = branch_zero(&ctx);
        check("BDZ full decrement", ctx.ctr, value-1);
        check("BDZ condition", taken, (value-1)==0);
    }
    ppu_context address{}; address.ctr = 0x1234567800001400;
    dispatch_address(&address);
    check("guest address dispatch", address_dispatched, true);
    check("dispatch does not truncate architectural CTR", address.ctr, 0x1234567800001400);
    printf("CTR64 checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
''')
    return '\n'.join(lines)


def main():
    with tempfile.TemporaryDirectory(prefix='ppu-ctr64-') as temporary:
        root = Path(temporary); source = root/'ctr64.cpp'
        executable = root/('ctr64.exe' if os.name == 'nt' else 'ctr64')
        source.write_text(emitted_driver())
        if os.name == 'nt':
            from msvc_env import find_vcvars
            vcvars = find_vcvars()
            if not vcvars:
                raise SystemExit('Visual Studio environment is required; no silent skip.')
            batch = root/'run.bat'
            batch.write_text('@echo off\n'
                             + f'call "{vcvars}" >nul 2>nul\n'
                             + f'cd /d "{root}"\n'
                             + f'cl /nologo /std:c++17 /O1 /W3 /I "{ROOT / "runtime/ppu"}" /Fe:"{executable}" "{source}"\n'
                             + 'if errorlevel 1 exit /b 2\n'
                             + f'"{executable}"\n')
            return subprocess.run(['cmd', '/c', str(batch)]).returncode
        cxx = os.environ.get('CXX') or shutil.which('c++') or shutil.which('clang++')
        if not cxx:
            raise SystemExit('A C++ compiler is required; no silent skip.')
        subprocess.run([cxx, '-std=c++17', '-O1', '-Wno-parentheses', '-I',
                        str(ROOT/'runtime/ppu'), str(source), '-o', str(executable)], check=True)
        return subprocess.run([str(executable)]).returncode

if __name__ == '__main__':
    raise SystemExit(main())
