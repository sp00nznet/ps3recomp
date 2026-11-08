#!/usr/bin/env python3
"""Execute real lifted synthetic setjmp/longjmp code, never a replacement restore."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ppu_lifter as L
from ppu_disasm import Instruction
ROOT = Path(__file__).resolve().parents[1]

OWNER = 0x1000
RESUME = 0x101C
RESTORE = 0x3000
CONFIG = {'nonlocal_return_functions': [hex(RESTORE)], 'continuations': [{'function': hex(OWNER), 'address': hex(RESUME)}]}


def instructions(base, ops):
    return [Instruction(base+4*i, 0, m, o) for i, (m, o) in enumerate(ops)]


PROGRAM = {
    OWNER: [('stdu','r1,-0x300(r1)'), ('mflr','r0'), ('std','r0,0x310(r1)'),
            ('std','r14,0x2f0(r1)'), ('li','r14,0x1234'), ('addi','r3,r1,0x40'),
            ('bl','0x2000'), ('cmpwi','cr7,r3,0'), ('bne','cr7,0x1040'),
            ('addi','r3,r1,0x40'), ('bl','0x4000'), ('li','r3,99'),
            ('stw','r3,0x44(r0)'), ('b','0x1040'), ('nop',''), ('nop',''),
            ('stw','r3,0x40(r0)'), ('ld','r0,0x310(r1)'), ('mtlr','r0'),
            ('ld','r14,0x2f0(r1)'), ('addi','r1,r1,0x300'), ('blr','')],
    0x2000: [('std','r1,0(r3)'), ('std','r2,8(r3)'), ('mflr','r0'), ('std','r0,16(r3)'),
             ('mfcr','r0'), ('std','r0,24(r3)'), ('std','r14,32(r3)'),
             ('stfd','f14,40(r3)'), ('addi','r5,r3,48'), ('stvx','v20,0,r5'), ('li','r3,0'), ('blr','')],
    RESTORE: [('ld','r1,0(r3)'), ('ld','r2,8(r3)'), ('ld','r0,16(r3)'), ('mtlr','r0'),
              ('ld','r0,24(r3)'), ('mtcrf','0xff,r0'), ('ld','r14,32(r3)'),
              ('lfd','f14,40(r3)'), ('addi','r5,r3,48'), ('lvx','v20,0,r5'),
              ('li','r3,0'), ('subfc','r0,r4,r3'), ('addze','r3,r4'), ('blr','')],
    0x4000: [('blr','')],
    0x5000: [('b','0x3000')],
}

DRIVER = r'''
#include "ppu_recomp.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <setjmp.h>
#include <thread>
static unsigned char memory[0x10000];
extern "C" { uint8_t* vm_base=memory; PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*)=nullptr; }
// Memory-model globals the generated header reads; inline access stays off.
extern "C" { int vm_inline_ok=0; int vm_inline_st_ok=0; uint32_t ppu_vm_size=sizeof(memory); uint32_t ppu_hle_inject_base=0; }
extern "C" uint64_t vm_read64(uint64_t p){assert(p+8<=sizeof(memory));uint64_t v=0;for(int i=0;i<8;i++)v=v*256+memory[p+i];return v;}
extern "C" uint32_t vm_read32(uint64_t p){assert(p+4<=sizeof(memory));uint32_t v=0;for(int i=0;i<4;i++)v=v*256+memory[p+i];return v;}
extern "C" void vm_write64(uint64_t p,uint64_t v){assert(p+8<=sizeof(memory));for(int i=7;i>=0;i--){memory[p+i]=v;v>>=8;}}
extern "C" void vm_write32(uint64_t p,uint32_t v){assert(p+4<=sizeof(memory));for(int i=3;i>=0;i--){memory[p+i]=v;v>>=8;}}
static const char* mode;
static int depth, callbacks, bad_fallthrough;
static uint64_t outer_buffer;
static ppu_context* active;
static jmp_buf host_exit;
static void pending(void*) { std::abort(); }
extern "C" { PPU_THREAD_LOCAL ppu_context* g_active_ctx=nullptr; }
static thread_local uint32_t g_vcall_stk[128];
static thread_local int g_vcall_sp, observed_depth;
static thread_local int g_dispatch_depth;
#define PS3_LOG_FIRST(ctr, limit) ((ctr) < (limit) && ++(ctr))
// ACTUAL_ACTIVE_SCOPE
void module_func_00003000(ppu_context*);
static void dispatched(ppu_context* ctx) {
    if(!std::strcmp(mode,"crossmodule")){module_func_00003000(ctx);return;}
    if(!std::strcmp(mode,"unrelated")){g_trampoline_fn=pending;throw 42;}
    func_00005000(ctx);
}
extern "C" void ps3_indirect_call(ppu_context* ctx) {
    uint32_t addr=0x5000; auto fn=dispatched;
    // ACTUAL_DISPATCH_BLOCK
}

extern "C" void ps3_hle_call(unsigned int,ppu_context* ctx){
    ++callbacks;
    uint64_t buffer=ctx->gpr[3];
    if(!std::strcmp(mode,"stale")) return;
#ifdef HAVE_NONLOCAL
    if(!std::strcmp(mode,"hostexit")){ps3_ppu_notify_host_unwind();longjmp(host_exit,1);}
#endif
    if(!depth && (!std::strcmp(mode,"recursive") || !std::strcmp(mode,"outer"))){
        outer_buffer=buffer; ++depth; func_00001000(ctx); --depth;
        assert(ctx->gpr[3]==7);ctx->gpr[3]=buffer;
    }
    if(!std::strcmp(mode,"outer") && depth) buffer=outer_buffer;
    if(!std::strcmp(mode,"wrongsp")) vm_write64(buffer,0xdead);
    if(!std::strcmp(mode,"wrongtarget")) vm_write64(buffer+16,0x1020);
    if(!std::strcmp(mode,"wrongthread")) ++ctx->thread_id;
    ppu_context other=*ctx;
    if(!std::strcmp(mode,"wrongctx"))ctx=&other;
    ctx->gpr[3]=buffer; ctx->gpr[4]=!std::strcmp(mode,"zero")?0:7;
    ctx->gpr[2]=0; ctx->gpr[14]=0; ctx->cr=0;ctx->fpr[14]=0;std::memset(&ctx->vr[20],0,16);
    g_trampoline_fn=pending;
    // A tail trampoline callee must not destroy the still-live owner.
    ps3_indirect_call(ctx);
    while(g_trampoline_fn){auto f=g_trampoline_fn;g_trampoline_fn=nullptr;f(ctx);}
    ++bad_fallthrough;
}
int main(int argc,char** argv){
    assert(argc==2);mode=argv[1];ppu_context ctx{};active=&ctx;
    ctx.gpr[1]=0x8000;ctx.gpr[2]=0x12345678;ctx.gpr[14]=0xabcdef;ctx.lr=0x9998;ctx.thread_id=17;
    ctx.cr=0xaabbccdd;ctx.fpr[14]=17.25;std::memset(&ctx.vr[20],0x5a,16);
    if(!std::strcmp(mode,"hostexit")){
        if(!setjmp(host_exit))func_00001000(&ctx);
        mode="basic";ctx.gpr[1]=0x8000;ctx.lr=0x9998;func_00001000(&ctx);
        // The abandoned checkpoint must be gone, including after same-context reentry.
        ctx.gpr[3]=0x7d40;ctx.gpr[4]=7;func_00003000(&ctx);return 90;
    }
    try{func_00001000(&ctx);}catch(int n){assert(n==42 && !std::strcmp(mode,"unrelated"));assert(g_trampoline_fn==pending && g_vcall_sp==0 && observed_depth==0);return 0;}
    if(!std::strcmp(mode,"stale")){ctx.gpr[3]=0x7d40;ctx.gpr[4]=7;func_00003000(&ctx);return 91;}
    assert(!bad_fallthrough && "guest longjmp returned to the throwing host call");
    assert(ctx.gpr[3]==(!std::strcmp(mode,"zero")?1:7));
    assert(vm_read32(0x40)==ctx.gpr[3]);
    assert(ctx.gpr[1]==0x8000 && ctx.lr==0x9998 && ctx.gpr[14]==0xabcdef);
    assert(ctx.gpr[2]==0x12345678 && ctx.fpr[14]==17.25);
    assert((ctx.cr&0xfffffff0u)==0xaabbcc d0u);
    for(int i=0;i<16;i++)assert(ctx.vr[20].b[i]==0x5a);
    assert(!g_trampoline_fn && g_vcall_sp==0 && observed_depth==0);
    // Repeated nonlocal calls must not accumulate dispatcher recursion depth.
    mode="basic";
    for(int i=0;i<4100;i++){ctx.lr=0x9998;func_00001000(&ctx);assert(observed_depth==0 && g_vcall_sp==0);}
    ppu_context scratch{};g_active_ctx=&ctx;
    try { ppu_active_context_scope guard(&scratch); assert(g_active_ctx==&scratch); throw 42; }
    catch(int n){assert(n==42 && g_active_ctx==&ctx);}
    // The same guest identity on another host thread cannot see this registry.
    ps3recomp_nonlocal::checkpoint slot{0x1234,ctx.gpr[1],true};
    { ps3recomp_nonlocal::frame owner(&ctx,&slot,1);
      std::thread worker([&]{assert(ps3recomp_nonlocal::live==nullptr);});worker.join(); }

    return 0;
}
'''.replace('0xaabbcc d0u','0xaabbccd0u')


def make_lifter(enabled=True, prefix=""):
    # On the RED baseline these fields are inert: execute its real incorrect return.
    lifter=L.PPULifter(prefix=prefix, nonlocal_return_functions={RESTORE} if enabled else set(),
                       nonlocal_continuations={OWNER:{RESUME}} if enabled else {})
    lifter.hle_stub_nids={0x4000:1}
    for addr,ops in PROGRAM.items():
        lifter.lift_function(instructions(addr,ops),addr,addr+4*len(ops))
    return lifter


def runtime_driver():
    source=(ROOT/'runtime/ppu/ppu_loader.cpp').read_text()
    start=source.index('struct ppu_active_context_scope {')
    guard=source[start:source.index('\n};',start)+3]
    start=source.index('    if (fn) {',source.index('static void ps3_indirect_call_impl(ppu_context* ctx)\n{'))
    end=source.index('    /* Garbage virtual-call guard:',start)
    block=source[start:end]
    anchor='int& s_depth = g_dispatch_depth;'
    assert anchor in block, 'dispatch depth anchor moved'
    block=block.replace(anchor, anchor+' observed_depth=s_depth;')
    return DRIVER.replace('// ACTUAL_ACTIVE_SCOPE',guard).replace('// ACTUAL_DISPATCH_BLOCK',block)


def expect_invalid(action):
    try: action()
    except ValueError: return
    raise AssertionError('invalid nonlocal configuration accepted')


def validation_tests(out):
    path=out/'config.json'
    path.write_text(json.dumps(CONFIG));assert L.load_nonlocal_control_flow(path)==({RESTORE},{OWNER:{RESUME}})
    for bad in [{}, {'nonlocal_return_functions':[], 'continuations':CONFIG['continuations']},
                {'nonlocal_return_functions':['0x3001'],'continuations':CONFIG['continuations']},
                {'nonlocal_return_functions':[True],'continuations':CONFIG['continuations']},
                {'nonlocal_return_functions':['0x3000','0x3000'],'continuations':CONFIG['continuations']},
                {'nonlocal_return_functions':['0x3000'],'continuations':CONFIG['continuations']*2}]:
        path.write_text(json.dumps(bad));expect_invalid(lambda:L.load_nonlocal_control_flow(path))
    for call in [('bctrl',''),('blrl',''),('nop',''),('bl','symbol')]:
        lifter=L.PPULifter(nonlocal_return_functions={RESTORE},nonlocal_continuations={OWNER:{OWNER+4}})
        expect_invalid(lambda:lifter.lift_function(instructions(OWNER,[call,('blr','')]),OWNER,OWNER+8))
    lifter=L.PPULifter(nonlocal_return_functions={RESTORE},nonlocal_continuations={OWNER:{OWNER+4}})
    lifter.code_hi=0x2000
    expect_invalid(lambda:lifter.lift_function(instructions(OWNER,[('bl','0x9000'),('blr','')]),OWNER,OWNER+8))
    for change in ['missing','hle','tail_owner','tail_restore','fallthrough','duplicate','indirect_tail','ordinary_restore_return']:
        lifter=make_lifter()
        if change=='missing': lifter.functions=[f for f in lifter.functions if f.start_addr!=RESTORE]
        if change=='hle': lifter.hle_stub_nids[OWNER]=1
        if change=='tail_owner': lifter.functions[0].body_lines.append('g_trampoline_fn = nullptr; return;')
        if change=='tail_restore': next(f for f in lifter.functions if f.start_addr==RESTORE).body_lines.append('g_trampoline_fn = nullptr; return;')
        if change=='fallthrough': lifter.functions[0].fallthrough_to=0x1100
        if change=='duplicate': lifter.nonlocal_continuations[0x2000]={RESUME}
        if change=='indirect_tail': lifter.functions[0].body_lines.append('ps3_indirect_call(ctx); return;')
        if change=='ordinary_restore_return': next(f for f in lifter.functions if f.start_addr==RESTORE).body_lines.append('return; /* b -> non-code */')
        expect_invalid(lifter.emit_header)
    # Check the public CLI and actual multiprocessing serialization with decoded PPC.
    raw=out/'input.bin';raw.write_bytes(bytes.fromhex('480000094e8000204e8000204e800020'))
    funcs=out/'functions.json';funcs.write_text(json.dumps([{'start':4096,'end':4104},{'start':4104,'end':4108},{'start':4108,'end':4112}]))
    path.write_text(json.dumps({'nonlocal_return_functions':['0x100c'],'continuations':[{'function':'0x1000','address':'0x1004'}]}))
    results=[]
    for jobs in [1,2]:
        dest=out/f'jobs{jobs}'
        run=subprocess.run([sys.executable,str(ROOT/'tools/ppu_lifter.py'),str(raw),'--raw','--base','0x1000','--functions',str(funcs),'--jobs',str(jobs),'--single-file','--nonlocal-control-flow',str(path),'--output',str(dest)],capture_output=True,text=True)
        assert run.returncode==0,run.stdout+run.stderr
        results.append({p.name:p.read_bytes() for p in dest.iterdir()})
    assert results[0]==results[1],'worker configuration/output differs'
    assert b'ps3recomp_nonlocal::raise(ctx)' in results[0]['ppu_recomp.c']


def main():
    compiler=os.environ.get('CXX') or shutil.which('clang++') or shutil.which('g++')
    assert compiler,'C++ compiler required'
    with tempfile.TemporaryDirectory(prefix='ppu-nonlocal-') as directory:
        out=Path(directory);validation_tests(out)
        if os.name=='nt':
            print('PASS configuration checks; SKIP native POSIX run (tools/test_ppu_nonlocal_windows.py covers Windows)');return
        lifter=make_lifter();(out/'ppu_recomp.h').write_text(lifter.emit_header())
        module=make_lifter(prefix='module_');module.header_name='module.h'
        (out/'module.h').write_text(module.emit_header());(out/'module.cpp').write_text(module.emit_source())
        (out/'lifted.cpp').write_text(lifter.emit_source());(out/'driver.cpp').write_text(runtime_driver())
        command=[compiler,'-std=c++20','-O2','-pthread','-DPPU_NO_INLINE_LOADS','-I'+str(ROOT/'runtime/ppu'),str(out/'lifted.cpp'),str(out/'module.cpp'),str(out/'driver.cpp'),'-o',str(out/'test')]
        hook=ROOT/'runtime/syscalls/ppu_host_unwind.c'
        if hook.exists():
            obj=out/'hook.o';subprocess.run([shutil.which('clang') or 'cc','-c',str(hook),'-o',str(obj)],check=True)
            command.extend(['-DHAVE_NONLOCAL',str(obj)])
        subprocess.run(command,check=True)
        result=subprocess.run([str(out/'test'),'basic'],capture_output=True,text=True)
        assert result.returncode==0, 'protected nonlocal return failed: '+result.stderr
        for mode in ['zero','recursive','outer','unrelated','crossmodule']:
            result=subprocess.run([str(out/'test'),mode],capture_output=True,text=True)
            assert result.returncode==0,(mode,result.stderr)
        for mode in ['wrongsp','wrongtarget','wrongctx','wrongthread','stale','hostexit']:
            result=subprocess.run([str(out/'test'),mode],capture_output=True,text=True)
            assert result.returncode!=0 and 'no live continuation' in result.stderr,(mode,result.returncode,result.stderr)
    print('PASS real generated nonlocal transfer, registers, recursion, identity, teardown and host-unwind reentry')

if __name__=='__main__':main()
