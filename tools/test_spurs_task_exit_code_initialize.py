#!/usr/bin/env python3
"""Exercise the generated NID entry and real exit-container initializer.

Missing registration, wrong validation order/codes, host-pointer use, and a
short/overlong clear must fail this fixture. No task is created or completed.
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
static_assert(sizeof(CellSpursTaskExitCode)==128);
static_assert(alignof(CellSpursTaskExitCode)==128);
extern "C" {
uint8_t* vm_base;
uint32_t ppu_vm_size;
void register_exit_code_fixture(void);
}
static void* handler;
extern "C" void ps3_hle_register(unsigned nid, const char*, void* fn) {
    assert(nid == 0x34552FA6u);
    assert(!handler);
    handler = fn;
}
static uint8_t memory[0x400];
static unsigned checks;
static void expect(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::abort(); }
    ++checks;
}
static bool all_bytes(unsigned begin, unsigned end, uint8_t value) {
    for (unsigned i=begin; i<end; ++i) if (memory[i]!=value) return false;
    return true;
}
static uint32_t call(uint64_t ea) {
    using Entry = int32_t (*)(CellSpursTaskExitCode*);
    return static_cast<uint32_t>(reinterpret_cast<Entry>(handler)(
        reinterpret_cast<CellSpursTaskExitCode*>(static_cast<uintptr_t>(ea))));
}
int main() {
    register_exit_code_fixture();
    expect(handler != nullptr, "generated NID 34552FA6 is registered");
    vm_base=memory; ppu_vm_size=sizeof(memory);
    std::memset(memory,0xA5,sizeof(memory));
    expect(call(0)==0x80410911u,"null pointer error");
    expect(all_bytes(0,sizeof(memory),0xA5),"null has no writes");
    expect(call(0x81)==0x80410910u,"one-byte misalignment error");
    expect(call(0xC0)==0x80410910u,"64-byte alignment is insufficient");
    expect(all_bytes(0,sizeof(memory),0xA5),"alignment failures have no writes");
    expect(call(0x80)==0,"aligned initialization succeeds");
    expect(all_bytes(0x80,0x100,0),"all 128 container bytes cleared");
    expect(all_bytes(0,0x80,0xA5)&&all_bytes(0x100,sizeof(memory),0xA5),
           "both outside canaries preserved");
    std::memset(memory+0x80,0x5A,128);
    expect(call(0x80)==0&&all_bytes(0x80,0x100,0),"reinitialization clears again");
    expect(call(0x100000080ull)==0,"pointer arguments use guest low 32 bits");
    expect(call(0x100000000ull)==0x80410911u,"guest low-32 null takes priority");
    expect(call(0x380)==0&&all_bytes(0x380,0x400,0),"last complete backed container");
    expect(call(0x400)==0x8041090Du,"outside backing rejected before access");
    ppu_vm_size=0x3FF;
    expect(call(0x380)==0x8041090Du,"partial backing rejected before access");
    ppu_vm_size=sizeof(memory);
    expect(call(0xFFFFFF80u)==0x8041090Du,"high aligned EA cannot wrap backing check");
    expect(call(0xFFFFFFFFu)==0x80410910u,"alignment error precedes backing check");
    vm_base=reinterpret_cast<uint8_t*>(UINTPTR_MAX-0x40u); ppu_vm_size=0;
    expect(call(0x80)==0x8041090Du,"host address overflow rejected before access");
    vm_base=nullptr; ppu_vm_size=0;
    expect(call(0x80)==0x8041090Du,"uninitialized VM rejected");
    expect(call(0)==0x80410911u,"null error precedes VM readiness");
    expect(call(0x81)==0x80410910u,"alignment error precedes VM readiness");
    vm_base=memory; ppu_vm_size=0; // existing runtime convention: full guest VM
    std::memset(memory+0x80,0x5A,128);
    expect(call(0x80)==0&&all_bytes(0x80,0x100,0),"zero size accepts backed full-VM mode");
    std::printf("PASS: %u exit-container checks\n",checks);
}
'''

with tempfile.TemporaryDirectory(prefix='spurs-exit-initialize-') as name:
    tmp=Path(name)
    generated=tmp/'nids.cpp'
    subprocess.run([sys.executable,str(ROOT/'tools/gen_hle_nids.py'),
                    'cellSpurs','--out',str(generated)],check=True)
    # Compile the actual generator's target registration statement, omitting
    # unrelated registrations whose handlers need worker/runtime dependencies.
    entries=[line for line in generated.read_text().splitlines()
             if 'ps3_hle_register(0x34552FA6u,' in line]
    assert len(entries)<=1
    registration=tmp/'registration.cpp'
    registration.write_text('extern "C" void ps3_hle_register(unsigned,const char*,void*);\n'
        'extern "C" void cellSpursTaskExitCodeInitialize(void);\n'
        'extern "C" void register_exit_code_fixture(void) {\n'+
        '\n'.join(entries)+'\n}\n')
    fixture=tmp/'fixture.cpp'; fixture.write_text(FIXTURE)
    obj=tmp/'spurs.o'; binary=tmp/'fixture'
    common=['-O1','-UNDEBUG','-ffunction-sections','-fdata-sections',
            '-I'+str(ROOT),'-I'+str(ROOT/'include'),'-I'+str(ROOT/'runtime/spu')]
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu17',*common,'-c',
                    str(ROOT/'libs/spurs/cellSpurs.c'),'-o',str(obj)],check=True)
    linker=['-Wl,-dead_strip'] if platform.system()=='Darwin' else ['-Wl,--gc-sections']
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++20',*common,
        str(fixture),str(registration),str(obj),*linker,'-pthread','-lm','-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10,cwd=tmp)
