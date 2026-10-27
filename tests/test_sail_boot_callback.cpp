/* Exercise generated NID registration and the real PPU HLE bridge. */
#include "ppu_context.h"
#include "ps3emu/guest_call.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

extern "C" {
uint8_t* vm_base;
uint32_t ppu_vm_size=0x10000;
uint32_t g_ww_lo=0,g_ww_hi=0;
int g_resv_store_active=0;
ps3_guest_caller_fn g_ps3_guest_caller=nullptr;
void ppu_hle_register_all();
int ps3_hle_has(uint32_t);
void ps3_hle_call(uint32_t,ppu_context*);
uint32_t prx_resolve_export(uint32_t) { return 0; }
void ps3_indirect_call(ppu_context*) { std::abort(); }
uint64_t ppu_guest_call(uint32_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t) { std::abort(); }
void ppu_prof_stamp(void*,unsigned) {}
void ppu_gcm_pump() {}
uint32_t ppu_prof_resolve_host(void*) { return 0; }
void ps3_msf(const char*,...) {}
int spu_coh_is_reserved(uint32_t) { return 0; }
void spu_coh_notify_write(uint32_t) {}
void spu_lockline_lock() {}
void spu_lockline_unlock() {}
void ppu_resv_break_store(uint64_t) {}
void ps3_ww_report_inline(uint32_t,uint64_t,int) {}
uint32_t vm_read32(uint64_t a) { uint32_t x;std::memcpy(&x,vm_base+a,4);return __builtin_bswap32(x); }
void vm_write32(uint64_t a,uint32_t x) { x=__builtin_bswap32(x);std::memcpy(vm_base+a,&x,4); }
}
extern "C" uint64_t vm_read64(uint64_t a) { uint64_t x;std::memcpy(&x,vm_base+a,8);return __builtin_bswap64(x); }
void vm_write64(uint64_t a,uint64_t x) { x=__builtin_bswap64(x);std::memcpy(vm_base+a,&x,8); }
extern "C" void ppu_dump_guest_stack(ppu_context*,const char*) {}
extern "C" void ppu_dump_bctrl_ring(uint32_t,const char*) {}
extern "C" void ppu_guest_callstack(const char*) {}
uint32_t g_ydkj_real_spurs_ea=0,g_ydkj_real_taskset_ea=0;

#include <array>
#include <vector>
struct Event { uint32_t opd; std::array<uint64_t,8> args; };
static std::vector<Event> events;
static void callback(uint32_t opd,uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f,uint64_t g,uint64_t h) {
    events.push_back({opd,{a,b,c,d,e,f,g,h}});
}
static unsigned checks;
#define CHECK(x) do { ++checks; if(!(x)) { std::fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);return 1; } } while(0)
static uint32_t call(uint32_t nid,uint64_t a=0,uint64_t b=0,uint64_t c=0,uint64_t d=0) {
    ppu_context ctx{};ctx.gpr[1]=0x8000;ctx.gpr[2]=0x12345678;
    ctx.gpr[3]=a;ctx.gpr[4]=b;ctx.gpr[5]=c;ctx.gpr[6]=d;
    ps3_hle_call(nid,&ctx);
    if(ctx.gpr[2]!=0x12345678)std::abort();
    return uint32_t(ctx.gpr[3]);
}
static bool event_is(size_t i,uint64_t major,uint64_t minor,uint64_t arg0,uint64_t arg1,uint32_t opd=0x4000,uint32_t user=0x4567) {
    return i<events.size() && events[i].opd==opd && events[i].args==std::array<uint64_t,8>{user,(major<<32)|minor,arg0,arg1,0,0,0,0};
}
int main() {
    static uint8_t memory[0x10000];vm_base=memory;ppu_hle_register_all();g_ps3_guest_caller=callback;
    constexpr uint32_t INIT=0x23654375,BOOT=0xBDF21B0F,CLOSE=0x85BEFFCC,STOP=0xEBA8D4EC,DESTROY=0x24F4E900;
    CHECK(ps3_hle_has(INIT));CHECK(ps3_hle_has(BOOT));
    CHECK(call(INIT,0x1000,0,0x4000,0x4567)==0);
    for(uint64_t param : {UINT64_C(0xFEDCBA9876543210),UINT64_C(0),UINT64_C(0x0FEF7800)}) {
        events.clear();CHECK(call(BOOT,0x1000,param)==0);CHECK(events.size()==3);
        CHECK(event_is(0,3,0,1,0));
        CHECK(event_is(1,2,1,0,param)); // Completion must echo the whole call userParam.
        CHECK(event_is(2,3,0,2,0));
    }
    events.clear();CHECK(call(STOP,0x1000)==0);CHECK(events.size()==3);
    CHECK(event_is(0,3,0,7,0));CHECK(event_is(1,2,11,0,0));CHECK(event_is(2,3,0,4,0));
    events.clear();CHECK(call(CLOSE,0x1000)==0);CHECK(events.size()==3);
    CHECK(event_is(0,3,0,8,0));CHECK(event_is(1,2,3,0,0));CHECK(event_is(2,3,0,2,0));
    CHECK(call(INIT,0x2000,0,0x5000,0x9876)==0);
    events.clear();CHECK(call(BOOT,0x2000,UINT64_C(0x1234567800000001))==0);CHECK(events.size()==3);
    CHECK(event_is(1,2,1,0,UINT64_C(0x1234567800000001),0x5000,0x9876));
    events.clear();CHECK(call(DESTROY,0x1000)==0);CHECK(call(BOOT,0x1000,0x1234)!=0);CHECK(events.empty());
    g_ps3_guest_caller=nullptr;CHECK(call(BOOT,0x2000,0x4321)==0);CHECK(events.empty());
    g_ps3_guest_caller=callback;CHECK(call(INIT,0x3000,0,0,0x1111)==0);
    CHECK(call(BOOT,0x3000,0x3333)==0);CHECK(events.empty());
    std::printf("%u checks PASS\n",checks);return 0;
}
