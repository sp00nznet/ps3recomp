#!/usr/bin/env python3
"""Run the real interpreter's byte XOR immediate vector semantics and existing self-test."""
import os
import subprocess
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: fixture currently requires a POSIX C compiler')
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
spu = root / 'runtime/spu'
env = {k: v for k, v in os.environ.items()
       if not k.startswith(('SPU_', 'SPURS_', 'YDKJ_', 'LBP_', 'GH3_'))}
with tempfile.TemporaryDirectory(prefix='spu-xorbi-') as tmp:
    for name in ['test_spu_interpreter_xorbi', 'spu_interp_selftest']:
        binary = Path(tmp) / name
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                        '-I'+str(root/'include'), '-I'+str(spu),
                        str(spu/'tests'/f'{name}.c'), str(spu/'spu_interp.c'),
                        '-lm', '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=5, env=env, cwd=tmp)

    # Real toolchain output: decode and lift all encoded cases, compile the
    # generated instruction bodies, then check the same literal vectors in C.
    from spu_disasm import spu_decode
    from spu_lifter import SPULifter
    sample = spu_decode(0x46000000 | (16 << 14) | (8 << 7) | 7, 0x100)
    assert sample.mnemonic == 'xorbi' and sample.operands == '$r7, $r8, 16'
    source = ['#include "spu_helpers.h"', '#include <stdlib.h>']
    entries = []
    for imm in [-512, -257, -256, -255, -128, -1, 0, 1, 16, 127, 128, 255, 256, 511]:
        for rt in [7, 8]:
            opcode = 0x46000000 | ((imm & 1023) << 14) | (8 << 7) | rt
            insn = spu_decode(opcode, 0x100)
            assert insn.mnemonic == 'xorbi'
            function = SPULifter(prefix=f'xorbi_{opcode:08x}_').lift_function([insn], 0x100, 0x104)
            source.append(f'static void {function.name}(spu_context* ctx) {{')
            source.extend(function.body_lines)
            source.append('}')
            entries.append(f'case 0x{opcode:08X}u: {function.name}(ctx); return;')
    source += ['static void lifted_run(uint32_t opcode,spu_context* ctx) {', 'switch(opcode) {']
    source += entries + ['default: abort();', '}', '}']
    generated = Path(tmp) / 'xorbi_lifted.h'
    generated.write_text('\n'.join(source) + '\n')
    binary = Path(tmp) / 'xorbi_lifted'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG', '-DXORBI_LIFTED',
                    '-I'+str(root/'include'), '-I'+str(spu), '-include', str(generated),
                    str(spu/'tests/test_spu_interpreter_xorbi.c'), str(spu/'spu_interp.c'),
                    '-lm', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=5, env=env, cwd=tmp)
    regenerated = subprocess.check_output([os.environ.get('PYTHON', 'python3'), str(root/'tools/gen_spu_interp.py')])
    assert regenerated == (spu/'spu_interp_tables.inc').read_bytes()
    print('PASS: XORBI decoder operands, compiled lifted vectors and generated-table reproducibility')
