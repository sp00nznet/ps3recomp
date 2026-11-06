#!/usr/bin/env python3
"""Compile the actual resolved guest-call body with an isolated function registry."""
import os,subprocess,tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
s=(root/'runtime/ppu/ppu_loader.cpp').read_text()
def function(name):
    start=s.index('extern "C"',s.index(name)-30);body=s.index('{',start);depth=1;i=body+1
    while depth:
        depth+=(s[i]=='{')-(s[i]=='}');i+=1
    return s[start:i]
def main():
    if os.name=='nt': print('SKIP standalone POSIX C++ fixture');return
    legacy=function('uint64_t ppu_guest_call_ct(')
    checked=function('int ppu_guest_call_ct_checked(') if 'int ppu_guest_call_ct_checked(' in s else ''
    begin=s.find('struct ppu_active_context_scope');scope=s[begin:s.index('};',begin)+2] if begin >= 0 else ''
    baseline="""// Baseline preserves the old ambiguous return contract so RED is behavioral.
extern "C" int ppu_guest_call_ct_checked(uint32_t c,uint32_t t,uint64_t a,uint64_t b,uint64_t d,uint64_t e,uint64_t f,uint64_t g,uint64_t h,uint64_t i,uint64_t*out){*out=ppu_guest_call_ct(c,t,a,b,d,e,f,g,h,i);return 1;}
"""
    pre="""#include "ppu_context.h"
#include <cassert>
#include <cstdio>
#include <stdexcept>
using ppu_fn=void(*)(ppu_context*);
static ppu_context* g_active_ctx=nullptr;
static void(*g_trampoline_fn)(void*)=nullptr;
#define PPU_TLS_TP 0x74000130
static uint32_t ppu_cb_stack_sp(ppu_context*) { return 0x50000; }
static unsigned calls=0;static bool throwing=false,trampoline=false;
static void tail(void*p){auto*c=(ppu_context*)p;c->gpr[3]=0xFEDCBA9876543210ULL;}
static void guest(ppu_context*c){++calls;assert(c==g_active_ctx);assert(c->gpr[2]==0x12345678);assert(c->thread_id==99);assert(c->gpr[13]==PPU_TLS_TP);for(unsigned i=0;i<8;++i)assert(c->gpr[3+i]==0x100000000ULL+i);if(throwing){g_trampoline_fn=tail;throw std::runtime_error("callback did not complete");}c->gpr[3]=0;if(trampoline)g_trampoline_fn=tail;}
static ppu_fn ppu_lookup(uint32_t c){return c==0x100?guest:nullptr;}
"""
    tests="""static int invoke(uint32_t code,uint64_t*out){return ppu_guest_call_ct_checked(code,0x12345678,0x100000000ULL,0x100000001ULL,0x100000002ULL,0x100000003ULL,0x100000004ULL,0x100000005ULL,0x100000006ULL,0x100000007ULL,out);}
int main(){ppu_context owner{};owner.thread_id=99;g_active_ctx=&owner;uint64_t out=0xA5;
assert(invoke(0x200,&out)==0);assert(!calls&&out==0&&g_active_ctx==&owner);
assert(invoke(0,&out)==0);assert(!calls);
assert(invoke(0x100,nullptr)==0);assert(!calls);
assert(invoke(0x100,&out)==1);assert(calls==1&&out==0&&g_active_ctx==&owner);
trampoline=true;assert(invoke(0x100,&out)==1);assert(out==0xFEDCBA9876543210ULL&&g_active_ctx==&owner);
g_trampoline_fn=tail;trampoline=false;assert(invoke(0x100,&out)==1&&out==0&&g_trampoline_fn==tail);g_trampoline_fn=nullptr;
throwing=true;out=0xA5;bool escaped=false;try{invoke(0x100,&out);}catch(const std::runtime_error&){escaped=true;}
assert(escaped&&out==0&&g_active_ctx==&owner);assert(!g_trampoline_fn);throwing=false;trampoline=false;assert(invoke(0x100,&out)==1&&out==0);puts("PASS never-dispatched vs completed zero,64-bit result/args,trampoline,exception context restore");}
"""
    with tempfile.TemporaryDirectory(prefix='guest-call-completion-') as tmp:
        tmp=Path(tmp);cpp=tmp/'fixture.cpp';cpp.write_text(pre+scope+'\n'+checked+'\n'+legacy+'\n'+('' if checked else baseline)+tests)
        subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-O1','-UNDEBUG','-I'+str(root/'runtime/ppu'),str(cpp),'-o',str(tmp/'fixture')],check=True,timeout=30)
        subprocess.run([str(tmp/'fixture')],check=True,timeout=5)
if __name__=='__main__':main()
