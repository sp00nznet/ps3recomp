#!/usr/bin/env python3
"""SMC micro-interpreter: lqa/stqa address the local store at I16 << 2.

Builds the real ``spu_smc_microstep`` (pulled verbatim out of
runtime/spu/spu_channels.c) into a small host program with the real SPU
headers. The program writes a register save/restore stub into the local store
the way a job manager writes its interrupt stub at runtime: 76 ``stqa`` of
r0..r75 to 0x3F9C0.., a ``bi``, then 76 ``lqa`` back from the same addresses
and a second ``bi``. It runs both halves through the micro-interpreter and
checks that every register lands at its own 16 bytes, that the restore brings
every register back, and that no local-store byte outside the save area and
the stub itself changes.
"""
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: fixture currently requires a POSIX C compiler')
    raise SystemExit(0)

cc = os.environ.get('CC') or shutil.which('cc') or shutil.which('gcc')
if not cc:
    print('SKIP: no C compiler')
    raise SystemExit(0)

root = Path(__file__).resolve().parents[1]
src = (root / 'runtime/spu/spu_channels.c').read_text()
m = re.search(r'static int spu_smc_microstep\(spu_context\* ctx\)\s*\{', src)
assert m, 'spu_smc_microstep not found'
i, depth = m.end(), 1
while depth:
    depth += {'{': 1, '}': -1}.get(src[i], 0)
    i += 1
func = src[m.start():i]

HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spu_context.h"
#include "spu_helpers.h"
#include "spu_interp.h"

SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);
int g_spu_ls_watch_n = 0;   /* LS watch off */
void spu_ls_watch_slow(uint32_t lsa, int is_write, const uint8_t* p, uint32_t pc, uint32_t lr)
{ (void)lsa; (void)is_write; (void)p; (void)pc; (void)lr; }
void spu_indirect_branch(spu_context* ctx) { (void)ctx; }
uint32_t spu_interp_run_until(spu_context* ctx, uint32_t a, uint32_t b)
{ (void)ctx; (void)a; (void)b; return 0; }
spu_lifted_fn spu_lifted_lookup(const spu_context* ctx, uint32_t lsa)   /* #266's rejoin check */
{ (void)ctx; (void)lsa; return NULL; }

%s

enum { NREG = 76, SAVE = 0x3F9C0, STUB = 0x2000 };

static void put(spu_context* c, uint32_t lsa, uint32_t w)
{
    c->ls[lsa] = (uint8_t)(w >> 24); c->ls[lsa + 1] = (uint8_t)(w >> 16);
    c->ls[lsa + 2] = (uint8_t)(w >> 8); c->ls[lsa + 3] = (uint8_t)w;
}
/* RI16 form: op9 | I16 | rt, with I16 = LSA >> 2 for the absolute forms. */
static uint32_t stqa(uint32_t rt, uint32_t lsa) { return (0x041u << 23) | (((lsa >> 2) & 0xFFFF) << 7) | rt; }
static uint32_t lqa(uint32_t rt, uint32_t lsa)  { return (0x061u << 23) | (((lsa >> 2) & 0xFFFF) << 7) | rt; }
static uint32_t bi(uint32_t ra)                  { return (0x1A8u << 21) | (ra << 7); }

int main(void)
{
    static spu_context c;
    static uint8_t before[SPU_LS_SIZE];
    spu_context_init(&c, 0);
    for (uint32_t i = 0; i < SPU_LS_SIZE; i++) c.ls[i] = (uint8_t)(i * 7 + 3);

    uint32_t save_pc = STUB, restore_pc = STUB + 4 * (NREG + 1);
    for (uint32_t r = 0; r < NREG; r++) put(&c, save_pc + 4 * r, stqa(r, SAVE + 16 * r));
    put(&c, save_pc + 4 * NREG, bi(0));
    for (uint32_t r = 0; r < NREG; r++) put(&c, restore_pc + 4 * r, lqa(r, SAVE + 16 * r));
    put(&c, restore_pc + 4 * NREG, bi(0));
    memcpy(before, c.ls, SPU_LS_SIZE);

    for (uint32_t r = 0; r < NREG; r++)
        for (int k = 0; k < 4; k++) c.gpr[r]._u32[k] = 0xA0000000u | (r << 8) | (uint32_t)k;
    c.gpr[0]._u32[0] = restore_pc;   /* bi r0 target; r0 is saved with this value */

    int fails = 0;
    c.pc = save_pc;
    if (!spu_smc_microstep(&c)) { printf("[FAIL] save stub did not run to its bi\n"); return 1; }

    int bad_slots = 0;
    for (uint32_t r = 0; r < NREG; r++) {
        u128 got = spu_ls_read128(&c, SAVE + 16 * r);
        if (memcmp(&got, &c.gpr[r], 16)) bad_slots++;
    }
    printf("[%%s] save: %%d of %%d registers at their own slot\n", bad_slots ? "FAIL" : "PASS", NREG - bad_slots, NREG);
    fails += bad_slots != 0;

    u128 saved[NREG];
    memcpy(saved, c.gpr, sizeof saved);
    for (uint32_t r = 1; r < NREG; r++) memset(&c.gpr[r], 0, 16);
    c.pc = restore_pc;
    if (!spu_smc_microstep(&c)) { printf("[FAIL] restore stub did not run to its bi\n"); return 1; }
    int bad_regs = 0;
    for (uint32_t r = 0; r < NREG; r++) if (memcmp(&c.gpr[r], &saved[r], 16)) bad_regs++;
    printf("[%%s] restore: %%d of %%d registers restored\n", bad_regs ? "FAIL" : "PASS", NREG - bad_regs, NREG);
    fails += bad_regs != 0;

    int stray = 0;
    for (uint32_t i = 0; i < SPU_LS_SIZE; i++) {
        if (i >= SAVE && i < SAVE + 16 * NREG) continue;
        if (c.ls[i] != before[i]) stray++;
    }
    printf("[%%s] %%d local-store bytes changed outside the save area\n", stray ? "FAIL" : "PASS", stray);
    fails += stray != 0;
    return fails ? 1 : 0;
}
'''

with tempfile.TemporaryDirectory() as tmp:
    c_path = Path(tmp) / 'smc_lqa_stqa.c'
    exe = Path(tmp) / 'smc_lqa_stqa'
    c_path.write_text(HARNESS % func)
    cmd = [cc, '-std=gnu17', '-O1', '-w',
           '-I', str(root / 'runtime/spu'), '-I', str(root / 'runtime'),
           '-I', str(root / 'include'), str(c_path), '-o', str(exe)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print(r.stdout + r.stderr)
        raise SystemExit('FAIL: harness did not compile')
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    print(r.stdout, end='')
    if r.returncode < 0:
        raise SystemExit(f'FAIL: harness crashed (signal {-r.returncode})')
    if r.returncode:
        raise SystemExit('FAIL: lqa/stqa in spu_smc_microstep')
    print('PASS: lqa/stqa address the local store at I16 << 2')
