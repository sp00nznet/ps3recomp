#!/usr/bin/env python3
"""sys_lwmutex_trylock on a mutex another thread holds returns CELL_EBUSY.

lv2 answers a contended trylock with CELL_EBUSY (0x8001000A). The context-style
HLE in runtime/ppu/ppu_sysprx.cpp returned 0x8001000B, CELL_ETIMEDOUT, which a
real kernel only gives a lock whose timeout expired. Titles that poll with
trylock and test for EBUSY then treat the busy case as a hard failure; FIOS in
PlayStation All-Stars Battle Royale (BCUS98472) logs
"sys_lwmutex_trylock failed on mutex 'dearchiver_opticalFunnel'" on every
contended poll.

Builds the real sys_lwmutex_trylock (pulled verbatim out of ppu_sysprx.cpp,
along with its LWM_OWNER / LWM_RECUR / LWM_SELF definitions) into a small host
program over stubbed guest memory and a single stub semaphore, and checks:
  - a trylock on a free mutex succeeds and stamps owner/recursion;
  - a trylock by a second thread while it is held returns CELL_EBUSY, not
    CELL_ETIMEDOUT, and leaves the holder's owner/recursion untouched.
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


def define(src, name):
    m = re.search(r'^#define %s\b.*$' % name, src, re.M)
    assert m, name + ' not found'
    return m.group(0)


HARNESS = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
static uint8_t mem[0x10000];
static void vm_write32(uint32_t a, uint32_t v) { for (int i = 0; i < 4; i++) mem[a + i] = (uint8_t)(v >> (24 - 8 * i)); }
static uint32_t vm_read32(uint32_t a) { return (uint32_t)mem[a] << 24 | mem[a + 1] << 16 | mem[a + 2] << 8 | mem[a + 3]; }
struct ppu_context { uint64_t gpr[32]; uint64_t thread_id; };
%s
%s
%s
/* One host-side slot standing in for the semaphore table: lwm_try takes it
 * if free. Hold tracking (lwm_trace) stays off. */
struct LwmSlot { long holder; long long acq_us; long long acq_fences; unsigned long long acq_cpu_us; int held; };
static LwmSlot s_slot;
static struct LwmSlot* lwm_slot(uint32_t) { return &s_slot; }
static struct LwmSlot* lwm_find(uint32_t) { return &s_slot; }
static int lwm_try(struct LwmSlot* sl) { if (sl->held) return 0; sl->held = 1; return 1; }
static int lwm_trace(void) { return 0; }
static long long lwm_now_us(void) { return 0; }
static volatile long long g_gcm_ref_pub_count = 0;
static unsigned long long ppu_thread_cpu_us(unsigned) { return 0; }
%s
static int32_t ret(const ppu_context& c) { return (int32_t)(uint32_t)c.gpr[3]; }
int main() {
    const uint32_t LWM = 0x1000, A = 0x101, B = 0x102;
    int fails = 0, ok;
    memset(mem, 0, sizeof mem);

    ppu_context c = {};
    c.gpr[3] = LWM; c.thread_id = A;
    sys_lwmutex_trylock(&c);
    ok = ret(c) == 0 && vm_read32(LWM + LWM_OWNER) == A && vm_read32(LWM + LWM_RECUR) == 1;
    printf("[%%s] free mutex: thread A acquires (ret 0x%%08X, owner 0x%%X, recur %%u)\n",
           ok ? "PASS" : "FAIL", (uint32_t)ret(c), vm_read32(LWM + LWM_OWNER), vm_read32(LWM + LWM_RECUR));
    fails += !ok;

    c = ppu_context{};
    c.gpr[3] = LWM; c.thread_id = B;
    sys_lwmutex_trylock(&c);
    ok = (uint32_t)ret(c) == 0x8001000Au && c.gpr[3] == 0xFFFFFFFF8001000Aull;
    printf("[%%s] held by A: thread B gets CELL_EBUSY (ret 0x%%08X)%%s\n",
           ok ? "PASS" : "FAIL", (uint32_t)ret(c),
           (uint32_t)ret(c) == 0x8001000Bu ? " -- that is CELL_ETIMEDOUT" : "");
    fails += !ok;
    ok = vm_read32(LWM + LWM_OWNER) == A && vm_read32(LWM + LWM_RECUR) == 1;
    printf("[%%s] held by A: owner and recursion unchanged (owner 0x%%X, recur %%u)\n",
           ok ? "PASS" : "FAIL", vm_read32(LWM + LWM_OWNER), vm_read32(LWM + LWM_RECUR));
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
    code = HARNESS % (define(src, 'LWM_OWNER'), define(src, 'LWM_RECUR'), define(src, 'LWM_SELF'),
                      extract(src, 'sys_lwmutex_trylock'))
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
            raise SystemExit('FAIL: contended sys_lwmutex_trylock')
    print('PASS: contended sys_lwmutex_trylock returns CELL_EBUSY')


if __name__ == '__main__':
    main()
