/* Real dispatcher, a fake PRX export: arguments past the eighth reach a PRX
 * callee through the parameter save area (#664 item 2). No game code. */
#include "ppu_context.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cerrno>
#include <vector>
#include <initializer_list>

extern "C" {
uint8_t* vm_base;
uint32_t ppu_vm_size=0x10000;
void ps3_hle_register(uint32_t,const char*,void*);
void ps3_hle_register_ctx(uint32_t,const char*,void(*)(ppu_context*));
extern const char* g_hle_inflight[64];
void ps3_hle_call(uint32_t,ppu_context*);
uint32_t prx_resolve_export(uint32_t nid) { return nid==0xF0020001u ? 0x100u : 0; }
void ps3_indirect_call(ppu_context*);
uint64_t ppu_guest_call(uint32_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t) { std::abort(); }
void ppu_prof_stamp(void*,unsigned) {}
void ppu_gcm_pump() {}
uint32_t ppu_prof_resolve_host(void*) { return 0; }
void ps3_msf(const char*,...) {}
uint32_t vm_read32(uint64_t a) { uint32_t x;std::memcpy(&x,vm_base+a,4);return __builtin_bswap32(x); }
void vm_write32(uint64_t a,uint32_t x) { x=__builtin_bswap32(x);std::memcpy(vm_base+a,&x,4); }
}
extern "C" uint64_t vm_read64(uint64_t a) { uint64_t x;std::memcpy(&x,vm_base+a,8);return __builtin_bswap64(x); }
void vm_write64(uint64_t a,uint64_t x) { x=__builtin_bswap64(x);std::memcpy(vm_base+a,&x,8); }
extern "C" void ppu_dump_guest_stack(ppu_context*,const char*) {}
extern "C" void ppu_dump_bctrl_ring(uint32_t,const char*) {}
extern "C" void ppu_guest_callstack(const char*) {}
uint32_t g_ydkj_real_spurs_ea=0,g_ydkj_real_taskset_ea=0;


enum : uint64_t { CALLER_R1=0x8000, ARG9=0x1111222233334444ull, ARG10=0x5555666677778888ull };
static uint64_t seen_r1, seen9, seen10, seen_ctr;
static unsigned entered;
extern "C" void ps3_indirect_call(ppu_context* c) {
    ++entered;
    seen_r1=c->gpr[1]; seen_ctr=c->ctr;
    seen9=vm_read64(c->gpr[1]+0x70);   /* ELF ABI v1: ninth argument */
    seen10=vm_read64(c->gpr[1]+0x78);
    /* A callee may home its register arguments and store its TOC in its
     * caller's frame; here that frame is the bridge's copy, not the caller's. */
    for(unsigned o=0x28;o<0x70;o+=8) vm_write64(c->gpr[1]+o,0);
    c->gpr[3]=0;
}
int main() {
    static uint8_t memory[0x10000]; vm_base=memory;
    std::memset(memory,0xA5,sizeof memory);
    vm_write32(0x100,0x2000); vm_write32(0x104,0x3000);   /* OPD: code, TOC */
    ppu_context c; std::memset(&c,0,sizeof c);
    c.gpr[1]=CALLER_R1; c.gpr[2]=0x12345678; c.thread_id=1; c.lr=0x11223344;
    vm_write64(CALLER_R1+0x70,ARG9); vm_write64(CALLER_R1+0x78,ARG10);
    std::vector<uint8_t> caller(memory+CALLER_R1+0x30,memory+CALLER_R1+0x130);
    ps3_hle_call(0xF0020001u,&c);
    unsigned failures=0;
    auto check=[&](const char* what,bool ok){ if(!ok){++failures;std::printf("FAIL %s\n",what);} };
    check("the PRX export was entered once",entered==1);
    check("entered at the export's code",seen_ctr==0x2000);
    check("r1 is isolated below the caller's frame",seen_r1<CALLER_R1 && seen_r1+0x130<=CALLER_R1);
    check("ninth argument at entry r1 + 0x70",seen9==ARG9);
    check("tenth argument at entry r1 + 0x78",seen10==ARG10);
    check("r1 restored after the call",c.gpr[1]==CALLER_R1);
    check("caller's parameter save area untouched by the callee",
          std::memcmp(caller.data(),memory+CALLER_R1+0x30,caller.size())==0);
    std::printf("PRX stack args: %s (r1 0x%llx -> 0x%llx, arg9 %016llx)\n",failures?"FAIL":"PASS",
        (unsigned long long)CALLER_R1,(unsigned long long)seen_r1,(unsigned long long)seen9);
    return failures?1:0;
}
