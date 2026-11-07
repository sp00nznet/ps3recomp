#!/usr/bin/env python3
"""sys_lwcond_t layout in the context-style HLE (runtime/ppu/ppu_sysprx.cpp).

sys_lwcond_t is 8 bytes: +0x00 u32 lwmutex EA, +0x04 u32 lwcond queue id (the
same layout libs/system/sysPrxForUser.h declares as sys_lwcond_t_hle). Builds
the real sys_lwcond_create and sys_lwcond_wait (pulled verbatim out of
ppu_sysprx.cpp) into a small host program over stubbed guest memory, creates
an lwcond, and checks:
  - +0x00 holds the lwmutex EA, +0x04 a nonzero id;
  - nothing is written past the 8-byte structure;
  - sys_lwcond_wait finds the lwmutex the lwcond was created with.
"""
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def extract(src, name):
    m = re.search(r'static void %s\(ppu_context\* ctx\)\s*\{' % name, src)
    assert m, name + ' not found'
    i, depth = m.end(), 1
    while depth:
        depth += {'{': 1, '}': -1}.get(src[i], 0)
        i += 1
    return src[m.start():i]


HARNESS = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
static uint8_t mem[0x10000];
static void vm_write32(uint32_t a, uint32_t v) { for (int i = 0; i < 4; i++) mem[a + i] = (uint8_t)(v >> (24 - 8 * i)); }
static void vm_write64(uint32_t a, uint64_t v) { vm_write32(a, (uint32_t)(v >> 32)); vm_write32(a + 4, (uint32_t)v); }
static uint32_t vm_read32(uint32_t a) { return (uint32_t)mem[a] << 24 | mem[a + 1] << 16 | mem[a + 2] << 8 | mem[a + 3]; }
static uint64_t vm_read64(uint32_t a) { return (uint64_t)vm_read32(a) << 32 | vm_read32(a + 4); }
struct ppu_context { uint64_t gpr[32]; uint32_t thread_id; };
/* The lwmutex side of sys_lwcond_wait: record which lwmutex it asked for and
 * hand back no slot, so the wait returns at once. */
#define LWM_OWNER 0x00
#define LWM_RECUR 0x04
static uint32_t s_asked = 0xFFFFFFFFu;
struct LwmSlot;
static struct LwmSlot* lwm_slot(uint32_t lwmutex) { s_asked = lwmutex; return nullptr; }
static void lwm_release(struct LwmSlot*) {}
static int lwm_acquire(struct LwmSlot*, uint64_t) { return 1; }
static void Sleep(unsigned) {}
static void ppu_thread_note_block(uint64_t) {}
/* PS3_LWCOND_REAL off: the poll path, the one the layout checks run on. */
typedef struct lwc_slot lwc_slot;
static int lwc_real(void) { return 0; }
static lwc_slot* lwc_slot_get(uint32_t, int) { return nullptr; }
static void sys_lwcond_wait_real(ppu_context*, struct LwmSlot*, lwc_slot*) {}
%s
%s
int main() {
    const uint32_t LWC = 0x1000, MTX = 0x2468;
    memset(mem, 0xAA, sizeof mem);
    ppu_context c = {};
    c.gpr[3] = LWC; c.gpr[4] = MTX; c.gpr[5] = 0x3000;
    sys_lwcond_create(&c);
    int fails = 0;
    uint32_t w0 = vm_read32(LWC), w4 = vm_read32(LWC + 4);
    int ok = w0 == MTX && w4 != 0 && w4 != 0xAAAAAAAAu && c.gpr[3] == 0;
    printf("[%%s] create: +0x00 = 0x%%08X (lwmutex 0x%%08X), +0x04 = 0x%%08X (queue id)\n",
           ok ? "PASS" : "FAIL", w0, MTX, w4);
    fails += !ok;
    int guard = 1;
    for (int i = 8; i < 16; i++) guard &= mem[LWC + i] == 0xAA;
    printf("[%%s] create: nothing written past the 8-byte sys_lwcond_t\n", guard ? "PASS" : "FAIL");
    fails += !guard;
    c = ppu_context{}; c.gpr[3] = LWC;
    sys_lwcond_wait(&c);
    ok = s_asked == MTX;
    printf("[%%s] wait: uses lwmutex 0x%%08X\n", ok ? "PASS" : "FAIL", s_asked);
    fails += !ok;
    return fails ? 1 : 0;
}
'''


def main():
    cxx = os.environ.get('CXX') or shutil.which('c++') or shutil.which('g++') or shutil.which('clang++')
    if not cxx:
        print('SKIP: no C++ compiler')
        return
    src = (ROOT / 'runtime/ppu/ppu_sysprx.cpp').read_text()
    code = HARNESS % (extract(src, 'sys_lwcond_create'), extract(src, 'sys_lwcond_wait'))
    with tempfile.TemporaryDirectory() as tmp:
        c = Path(tmp) / 't.cpp'
        exe = Path(tmp) / 't'
        c.write_text(code)
        r = subprocess.run([cxx, '-std=c++17', '-O1', '-w', str(c), '-o', str(exe)], capture_output=True, text=True)
        if r.returncode:
            print(r.stderr[-2000:])
            raise SystemExit('FAIL: harness did not compile')
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        print(r.stdout, end='')
        if r.returncode:
            raise SystemExit('FAIL: sys_lwcond_t layout')
    print('PASS: sys_lwcond_t is +0x00 lwmutex EA, +0x04 queue id')


if __name__ == '__main__':
    main()
