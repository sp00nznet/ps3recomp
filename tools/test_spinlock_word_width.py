#!/usr/bin/env python3
"""Exercise the production sys_spinlock helpers on four-byte guest words.

Extract the complete spinlock section, as test_lwcond_layout.py does for its
handlers, and use the real platform atomics. An eight-byte exchange must not
overwrite the neighboring word or fault on a four-but-not-eight-aligned lock.
"""
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "win32_compat.h"

alignas(8) static uint8_t memory[512];
static uint8_t* vm_base = memory;
struct ppu_context { uint64_t gpr[32]; uint64_t lr; };

// PRODUCTION_SPINLOCK_HELPERS

static void check(uint32_t ea, uint8_t locked, const char* operation)
{
    const uint8_t expected[4] = {0, 0, 0, locked};
    if (memcmp(memory + ea, expected, sizeof expected) != 0) {
        fprintf(stderr, "FAIL: %s: wrong big-endian lock value\n", operation);
        exit(1);
    }
    for (uint32_t i = 0; i < sizeof memory; ++i) {
        if ((i < ea || i >= ea + 4) && memory[i] != 0xA5) {
            fprintf(stderr, "FAIL: %s: modified byte outside four-byte lock at +%d\n",
                    operation, (int)i - (int)ea);
            exit(1);
        }
    }
}

static void exercise(uint32_t ea)
{
    memset(memory, 0xA5, sizeof memory);
    ppu_context ctx = {};
    ctx.gpr[3] = ea;
    sys_spinlock_initialize(&ctx);
    check(ea, 0, "initialize");

    ctx.gpr[3] = ea;
    sys_spinlock_lock(&ctx);
    check(ea, 1, "lock");

    ctx.gpr[3] = ea;
    sys_spinlock_trylock(&ctx);
    if (ctx.gpr[3] != UINT64_C(0xFFFFFFFF8001000A)) {
        fprintf(stderr, "FAIL: trylock while held must return sign-extended EBUSY\n");
        exit(1);
    }
    check(ea, 1, "busy trylock");

    ctx.gpr[3] = ea;
    sys_spinlock_unlock(&ctx);
    check(ea, 0, "unlock");

    ctx.gpr[3] = ea;
    sys_spinlock_trylock(&ctx);
    if (ctx.gpr[3] != 0) {
        fprintf(stderr, "FAIL: trylock after unlock must return CELL_OK\n");
        exit(1);
    }
    check(ea, 1, "successful trylock");

    ctx.gpr[3] = ea;
    sys_spinlock_unlock(&ctx);
    check(ea, 0, "final unlock");
    printf("PASS: spinlock at host address modulo 8 = %u\n",
           (unsigned)((uintptr_t)(memory + ea) % 8));
}

int main()
{
    // The aligned case diagnoses adjacent-word corruption even on hosts where
    // the misaligned eight-byte exchange would terminate with SIGBUS.
    exercise(0x100);
    exercise(0x104);
    return 0;
}
'''


def main():
    if os.name == 'nt':
        print('SKIP: LP64 regression harness uses a POSIX compiler driver')
        return
    cxx = os.environ.get('CXX') or shutil.which('c++') or shutil.which('clang++') or shutil.which('g++')
    if not cxx:
        raise SystemExit('FAIL: no C++ compiler available')
    source = (ROOT / 'runtime/ppu/ppu_sysprx.cpp').read_text()
    start = source.index('#define SPINLOCK_BE1')
    end = source.index('extern "C" void ppu_sysprx_register', start)
    code = HARNESS.replace('// PRODUCTION_SPINLOCK_HELPERS', source[start:end])
    with tempfile.TemporaryDirectory() as tmp:
        cpp = Path(tmp) / 'spinlock.cpp'
        exe = Path(tmp) / 'spinlock'
        cpp.write_text(code)
        subprocess.run([cxx, '-std=c++17', '-O2', '-pthread',
                        '-I', str(ROOT / 'runtime/platform'), str(cpp),
                        '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=10)


if __name__ == '__main__':
    main()
