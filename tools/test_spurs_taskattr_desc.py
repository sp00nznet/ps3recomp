#!/usr/bin/env python3
"""Descriptor-form auto-detect for _cellSpursTaskAttributeInitialize.

MLB The Show 16's Bink task calls the initializer with r7 = a {context EA,
context size, lsPattern} descriptor and r8 = 0.  The R8-form default then
leaves sizeContext/lsPattern zero, and the SPU task library parks the task in
WAIT_SIGNAL forever.  The auto-detect must decode that shape, while the
YDKJ/Jackbox R8 form (r8 = a CellSpursTaskArgument*) and an implausible
six-argument context must stay on the R8 path.

The fixture compiles the real libs/spurs/cellSpurs.c; only the guest VM and
the two globals it reads are supplied.  No game run.
"""
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    print('SKIP: standalone fixture requires the POSIX C/C++ toolchain')
    raise SystemExit(0)

FIXTURE = r'''
#include "libs/spurs/cellSpurs.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
extern "C" {
uint8_t* vm_base;
uint32_t ppu_vm_size;
int32_t _cellSpursTaskAttributeInitialize(CellSpursTaskAttribute*, uint32_t,
        uint32_t, uint64_t, uint64_t, uint32_t, const void*, const void*);
}
static uint8_t memory[0x40000];
static unsigned checks;
static void expect(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::abort(); }
    ++checks;
}
static void put32(uint32_t ea, uint32_t v) {
    memory[ea+0]=uint8_t(v>>24); memory[ea+1]=uint8_t(v>>16);
    memory[ea+2]=uint8_t(v>>8);  memory[ea+3]=uint8_t(v);
}
int main() {
    unsetenv("SPURS_TASKATTR_DESC");
    unsetenv("SPURS_TASKATTR_LEGACY");
    vm_base = memory; ppu_vm_size = sizeof(memory);
    std::memset(memory, 0, sizeof memory);

    /* MLB Bink shape: r7 = descriptor, r8 = 0.  The context EA is a heap
     * address in 0x10..0x2F (here 0x200), the size a real save-area size. */
    put32(0x10000, 0x12000); /* [0] context EA  */
    put32(0x10004, 0xC00);   /* [1] context size */
    put32(0x10008, 0x000);   /* [2] lsPattern (may be 0) */
    CellSpursTaskAttribute* A = reinterpret_cast<CellSpursTaskAttribute*>(memory + 0x8000);
    auto call = [&](uint64_t ctx, uint32_t sz) {
        std::memset(A, 0, sizeof *A);
        return _cellSpursTaskAttributeInitialize(
            reinterpret_cast<CellSpursTaskAttribute*>(static_cast<uintptr_t>(0x8000)),
            1, 0x330000, 0x1234, ctx, sz, nullptr, nullptr);
    };
    expect(call(0x10000, 0) == 0, "descriptor call succeeds");
    expect(A->eaElf == 0x1234, "eaElf carried");
    expect(A->eaContext == 0x12000, "descriptor context EA decoded");
    expect(A->sizeContext == 0xC00, "descriptor context size decoded");
    expect(A->lsPattern_ea == 0, "zero lsPattern preserved");
    expect(A->argument_ea == 0, "r8 remains the argument");

    /* A heap-range context EA (0x16xxxxxx) must also be accepted: the earlier
     * <0x10000000 bound was wrong for this title's heap. */
    put32(0x10000, 0x16000000u);
    expect(call(0x10000, 0) == 0, "heap-EA descriptor call succeeds");
    expect(A->eaContext == 0x16000000u, "heap-range context EA decoded");
    expect(A->sizeContext == 0xC00, "descriptor size decoded with heap EA");

    /* YDKJ/Jackbox R8 form: r8 = CellSpursTaskArgument* (nonzero).  It must
     * not be mistaken for a descriptor even though r7 looks descriptor-like. */
    expect(call(0x10000, 0x300) == 0, "R8-form call succeeds");
    expect(A->eaContext == 0x10000, "R8 form keeps r7 as the context EA");
    expect(A->sizeContext == 0x300, "R8 form keeps r8 as the size/arg");
    expect(A->argument_ea == 0x300, "R8 form uses r8 as the argument");
    expect(A->lsPattern_ea == 0, "R8 form leaves lsPattern zero");

    /* r8 = 0 but implausible descriptor: stay on the R8 path. */
    put32(0x10000, 0); put32(0x10004, 0xC00); put32(0x10008, 0);
    expect(call(0x10000, 0) == 0, "implausible-descriptor call succeeds");
    expect(A->eaContext == 0x10000, "implausible descriptor keeps r7");
    expect(A->sizeContext == 0, "implausible descriptor leaves size 0");
    expect(A->argument_ea == 0, "implausible descriptor leaves arg 0");

    std::printf("PASS: %u task-attribute descriptor checks\n", checks);
}
'''

with tempfile.TemporaryDirectory(prefix='spurs-taskattr-desc-') as name:
    tmp = Path(name)
    fixture = tmp / 'fixture.cpp'
    fixture.write_text(FIXTURE)
    obj = tmp / 'spurs.o'
    binary = tmp / 'fixture'
    common = ['-O1', '-UNDEBUG', '-ffunction-sections', '-fdata-sections',
              '-I' + str(ROOT), '-I' + str(ROOT / 'include'),
              '-I' + str(ROOT / 'runtime/spu')]
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', *common, '-c',
                    str(ROOT / 'libs/spurs/cellSpurs.c'), '-o', str(obj)],
                   check=True)
    linker = (['-Wl,-dead_strip'] if platform.system() == 'Darwin'
              else ['-Wl,--gc-sections'])
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', *common,
                    str(fixture), str(obj), *linker, '-pthread', '-lm',
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10, cwd=tmp)
