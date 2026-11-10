/* Real dispatcher, synthetic handlers and VM: no lifted game code or assets. */
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
void ps3_hle_register_fast(uint32_t,int(*)(ppu_context*));
extern const char* g_hle_inflight[64];
void ps3_hle_call(uint32_t,ppu_context*);
uint32_t prx_resolve_export(uint32_t) { return 0; }
void ps3_indirect_call(ppu_context*) { std::abort(); }
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

static ppu_context* current;
static unsigned calls;
static void clobber(ppu_context* c) {
    ++calls;
    c->gpr[3]=0x76543210;
    c->gpr[2]=0x30039AB0;
    vm_write64(c->gpr[1]+0x28,0);
}
static void ordinary(ppu_context* c) { ++calls; c->gpr[3]=0x89ABCDEF; }
/* Fast forms (ps3_hle_register_fast): one finishes the call itself, one
 * declines it so the full handler registered under the same NID runs. */
static unsigned fast_calls;
static int fast_done(ppu_context* c) { ++fast_calls; c->gpr[3]=0x13579BDF; return 1; }
static int fast_declined(ppu_context*) { ++fast_calls; return 0; }
static uint64_t generic(uint64_t,uint64_t,uint64_t,uint64_t,
                        uint64_t,uint64_t,uint64_t,uint64_t) {
    clobber(current);
    return 0x76543210;
}
int main() {
    static uint8_t memory[0x10000];vm_base=memory;
    ps3_hle_register(0xF0010101,"toc-generic",(void*)generic);
    ps3_hle_register_ctx(0xF0010102,"toc-context",ordinary);
    ps3_hle_register_ctx(0xF0010103,"toc-context-clobber",clobber);
    setenv("YDKJ_TUNERFIX","1",1);
    unsigned cases=0,failures=0;
    for(unsigned mode=0;mode<4;++mode) {
        for(uint64_t slot: {UINT64_C(0),UINT64_C(0xDEADBEEFCAFEBABE)}) {
            std::memset(memory,0xA5,sizeof memory);
            ppu_context c;std::memset(&c,0x5A,sizeof c);
            c.gpr[1]=0x8000;c.gpr[2]=0x12345678;c.thread_id=1;c.lr=0x11223344;
            vm_write64(c.gpr[1]+0x28,slot);
            std::vector<uint8_t> before(memory,memory+sizeof memory);
            ppu_context expected=c;
            expected.gpr[3]=mode==1?0x89ABCDEF:mode==3?
                uint64_t(int64_t(int32_t(0x8001112E))):0x76543210;
            const unsigned start=calls;current=&c;errno=EDOM;
            ps3_hle_call(mode==3?0xE0998DBFu:0xF0010101+mode,&c);
            const bool live=c.gpr[2]==expected.gpr[2];
            const bool spill=vm_read64(0x8028)==expected.gpr[2];
            // Generated import callers execute ld r2,0x28(r1) after return.
            c.gpr[2]=vm_read64(c.gpr[1]+0x28);
            const bool regs=std::memcmp(&c,&expected,sizeof c)==0;
            const bool err=errno==EDOM;
            std::memcpy(before.data()+0x8028,memory+0x8028,8);
            const bool unchanged=std::memcmp(before.data(),memory,sizeof memory)==0;
            const bool dispatch=calls-start==(mode==3?0u:1u);
            const bool inflight=g_hle_inflight[1]==nullptr;
            const bool ok=live&&spill&&regs&&err&&unchanged&&dispatch&&inflight;
            ++cases;if(!ok)++failures;
            std::printf("mode=%u slot=%016llx live=%d spill=%d regs=%d errno=%d "
                "outside_slot=%d dispatch=%d inflight=%d %s\n",mode,
                (unsigned long long)slot,live,spill,regs,err,unchanged,dispatch,inflight,
                ok?"PASS":"FAIL");
        }
    }
    /* A fast form returns before the full dispatcher, but the generated
     * caller still reloads r2 from 0x28(r1): the slot has to hold the TOC on a
     * fresh stack too, or the caller runs on with r2 = whatever was there. */
    ps3_hle_register_fast(0xF0010104,fast_done);
    ps3_hle_register_fast(0xF0010105,fast_declined);
    ps3_hle_register_ctx(0xF0010105,"toc-fast-declined",ordinary);
    for(unsigned mode=0;mode<2;++mode) {
        for(uint64_t slot: {UINT64_C(0),UINT64_C(0xDEADBEEFCAFEBABE)}) {
            std::memset(memory,0xA5,sizeof memory);
            ppu_context c;std::memset(&c,0x5A,sizeof c);
            c.gpr[1]=0x8000;c.gpr[2]=0x12345678;c.thread_id=1;c.lr=0x11223344;
            vm_write64(c.gpr[1]+0x28,slot);
            std::vector<uint8_t> before(memory,memory+sizeof memory);
            ppu_context expected=c;
            expected.gpr[3]=mode==0?0x13579BDF:0x89ABCDEF;
            const unsigned start=calls,fast_start=fast_calls;
            ps3_hle_call(0xF0010104+mode,&c);
            const bool live=c.gpr[2]==expected.gpr[2];
            const bool spill=vm_read64(0x8028)==expected.gpr[2];
            c.gpr[2]=vm_read64(c.gpr[1]+0x28);
            const bool regs=std::memcmp(&c,&expected,sizeof c)==0;
            std::memcpy(before.data()+0x8028,memory+0x8028,8);
            const bool unchanged=std::memcmp(before.data(),memory,sizeof memory)==0;
            const bool dispatch=fast_calls-fast_start==1u&&calls-start==(mode==0?0u:1u);
            const bool ok=live&&spill&&regs&&unchanged&&dispatch;
            ++cases;if(!ok)++failures;
            std::printf("fast=%s slot=%016llx live=%d spill=%d regs=%d outside_slot=%d "
                "dispatch=%d %s\n",mode==0?"done":"declined",(unsigned long long)slot,
                live,spill,regs,unchanged,dispatch,ok?"PASS":"FAIL");
        }
    }
    std::printf("TOC guard: %u cases, %u failures\n",cases,failures);
    return failures?1:0;
}
