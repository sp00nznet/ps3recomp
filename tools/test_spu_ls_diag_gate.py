#!/usr/bin/env python3
"""The LS diagnostics sit behind one gate, and it still opens.

spu_ls_read128/write128 reach every LS diagnostic (SPU_LS_WATCH,
SPU_LS_LOWREAD, SPU_SMC_WATCH, SPU_WWS_PROBE) through g_spu_ls_diag. This
builds a small host program on the real SPU header and checks, through the
SPU_LS_WATCH hook, that:

- with no switch set the gate reads 0 and no access reaches the watch;
- with SPU_LS_WATCH set the gate reads 1 and every access does;
- a stored quadword reads back the same, big-endian in the local store, in
  both runs.
"""
import os
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

HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spu_context.h"

int g_spu_ls_watch_n = 1;   /* a line is watched: the hook runs when the gate lets it */
static int s_hits;
void spu_ls_watch_slow(uint32_t lsa, int is_write, const uint8_t* p, uint32_t pc, uint32_t lr)
{ (void)lsa; (void)is_write; (void)p; (void)pc; (void)lr; s_hits++; }

static int same(u128 a, u128 b) { return !memcmp(&a, &b, sizeof a); }

int main(void)
{
    static spu_context c;
    spu_context_init(&c, 0);
    u128 v;
    for (int i = 0; i < 4; i++) v._u32[i] = 0x01020304u * (uint32_t)(i + 1);
    int bad = 0;

    spu_ls_write128(&c, 0x100, v);
    if (!same(spu_ls_read128(&c, 0x100), v)) { printf("[FAIL] round trip at 0x100\n"); bad++; }
    spu_ls_write128(&c, 0x110, v);
    if (!same(spu_ls_read128(&c, 0x110), v)) { printf("[FAIL] round trip at 0x110\n"); bad++; }
    if (c.ls[0x100] != 0x01 || c.ls[0x103] != 0x04) { printf("[FAIL] LS is not big-endian\n"); bad++; }
    printf("gate=%d hits=%d\n", g_spu_ls_diag, s_hits);
    return bad;
}
'''


def run(exe, env_extra):
    env = {k: v for k, v in os.environ.items()
           if k not in ('SPU_LS_WATCH', 'SPU_LS_LOWREAD', 'SPU_SMC_WATCH', 'SPU_WWS_PROBE')}
    env.update(env_extra)
    r = subprocess.run([str(exe)], capture_output=True, text=True, env=env)
    print(r.stdout, end='')
    if r.returncode:
        raise SystemExit('FAIL: harness')
    gate, hits = (int(x.split('=')[1]) for x in r.stdout.split()[-2:])
    return gate, hits


with tempfile.TemporaryDirectory() as tmp:
    c_path = Path(tmp) / 'ls_diag_gate.c'
    exe = Path(tmp) / 'ls_diag_gate'
    c_path.write_text(HARNESS)
    cmd = [cc, '-std=gnu17', '-O1', '-w',
           '-I', str(root / 'runtime/spu'), '-I', str(root / 'runtime'),
           '-I', str(root / 'include'), str(c_path), '-o', str(exe)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print(r.stdout + r.stderr)
        raise SystemExit('FAIL: harness did not compile')

    gate, hits = run(exe, {})
    if gate != 0 or hits != 0:
        raise SystemExit(f'FAIL: no switch set, but gate={gate} and the watch saw {hits} accesses')
    gate, hits = run(exe, {'SPU_LS_WATCH': '0x100'})
    if gate != 1 or hits != 4:
        raise SystemExit(f'FAIL: SPU_LS_WATCH set, but gate={gate} and the watch saw {hits} of 4 accesses')
    print('PASS: the LS diagnostics stay closed by default and open with their switch')
