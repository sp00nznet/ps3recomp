#!/usr/bin/env python3
"""sys_lwmutex_destroy under PS3_LWCOND_REAL: EBUSY while held, release + clear
when free (runtime/ppu/ppu_sysprx.cpp). Builds the real function over stubbed
guest memory and a stubbed lwm slot table."""
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
#include <cstdlib>
static uint8_t mem[0x10000];
static void vm_write32(uint32_t a, uint32_t v) { for (int i = 0; i < 4; i++) mem[a + i] = (uint8_t)(v >> (24 - 8 * i)); }
static uint32_t vm_read32(uint32_t a) { return (uint32_t)mem[a] << 24 | mem[a + 1] << 16 | mem[a + 2] << 8 | mem[a + 3]; }
struct ppu_context { uint64_t gpr[32]; uint32_t thread_id; };
#define LWM_OWNER 0x00
#define LWM_RECUR 0x0C
static int g_free_calls, g_free_rc;
static int lwm_slot_free(uint32_t) { g_free_calls++; return g_free_rc; }
static int lwc_real() { return 1; }
%s
int main() {
    const uint32_t LWM = 0x1000;
    ppu_context c = {};
    c.gpr[3] = LWM;
    vm_write32(LWM + LWM_OWNER, 7); vm_write32(LWM + LWM_RECUR, 1);
    g_free_calls = 0; g_free_rc = 1;
    sys_lwmutex_destroy_counted(&c);
    int fails = 0;
    int ok = (uint32_t)c.gpr[3] == 0x8001000Au && g_free_calls == 0;
    printf("[%%s] held mutex -> EBUSY, slot untouched\n", ok ? "PASS" : "FAIL"); fails += !ok;
    vm_write32(LWM + LWM_OWNER, 0); vm_write32(LWM + LWM_RECUR, 0);
    c.gpr[3] = LWM; g_free_calls = 0; g_free_rc = 0;
    sys_lwmutex_destroy_counted(&c);
    ok = (uint32_t)c.gpr[3] == 0x8001000Au && g_free_calls == 1;
    printf("[%%s] parked waiter (slot busy) -> EBUSY\n", ok ? "PASS" : "FAIL"); fails += !ok;
    for (int i = 0; i < 0x18; i += 4) vm_write32(LWM + i, 0xAAAAAAAAu);
    vm_write32(LWM + LWM_OWNER, 0); vm_write32(LWM + LWM_RECUR, 0);
    c.gpr[3] = LWM; g_free_calls = 0; g_free_rc = 1;
    sys_lwmutex_destroy_counted(&c);
    ok = c.gpr[3] == 0 && g_free_calls == 1;
    for (int i = 0; i < 0x18; i += 4) ok = ok && vm_read32(LWM + i) == 0;
    printf("[%%s] free mutex -> slot released, guest struct cleared\n", ok ? "PASS" : "FAIL"); fails += !ok;
    return fails ? 1 : 0;
}
'''


def main():
    cxx = os.environ.get('CXX') or shutil.which('c++') or shutil.which('g++') or shutil.which('clang++')
    if not cxx:
        print('SKIP: no C++ compiler')
        return
    src = (ROOT / 'runtime/ppu/ppu_sysprx.cpp').read_text()
    code = HARNESS % extract(src, 'sys_lwmutex_destroy_counted')
    with tempfile.TemporaryDirectory() as tmp:
        c = Path(tmp) / 't.cpp'
        exe = Path(tmp) / 't'
        c.write_text(code)
        r = subprocess.run([cxx, '-std=c++17', '-O1', '-w', str(c), '-o', str(exe)],
                           capture_output=True, text=True)
        if r.returncode:
            print(r.stderr[-2000:])
            raise SystemExit('FAIL: harness did not compile')
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        print(r.stdout, end='')
        if r.returncode:
            raise SystemExit('FAIL: sys_lwmutex_destroy')
    print('PASS: sys_lwmutex_destroy EBUSY/free/clear')


if __name__ == '__main__':
    main()
