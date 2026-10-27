/* Extended unload through generated NID registration and real PPU dispatch. */
#include "ppu_context.h"
#include "cellSysmodule.h"
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

static unsigned checks;
#define CHECK(x) do { ++checks; if(!(x)) { std::fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);return 1; } } while(0)
static constexpr uint32_t UNLOAD=0xFD0E1CAC, LOAD=0x1A267F98, QUERY=0x70E319B5;
static uint32_t call(uint32_t nid,uint64_t id,uint64_t unused=0) {
    ppu_context ctx{};ctx.gpr[1]=0x8000;ctx.gpr[2]=0x12345678;
    ctx.gpr[3]=id;ctx.gpr[4]=unused;ctx.gpr[5]=unused;ctx.gpr[6]=unused;
    ctx.gpr[7]=unused;ctx.gpr[8]=unused;ctx.gpr[9]=unused;ctx.gpr[10]=unused;
    unsigned char before[0x10000];std::memcpy(before,vm_base,sizeof before);
    // The production bridge saves r2 to the ABI linkage area on every call.
    uint64_t toc=__builtin_bswap64(ctx.gpr[2]);std::memcpy(before+0x8028,&toc,8);
    ps3_hle_call(nid,&ctx);
    if(ctx.gpr[2]!=0x12345678 || std::memcmp(before,vm_base,sizeof before))std::abort();
    for(unsigned r=4;r<=10;++r)if(ctx.gpr[r]!=unused)std::abort();
    return uint32_t(ctx.gpr[3]);
}
int main() {
    static uint8_t memory[0x10000];vm_base=memory;std::memset(memory,0xA5,sizeof memory);
    ppu_hle_register_all();
    CHECK(ps3_hle_has(UNLOAD));
    CHECK(ps3_hle_has(LOAD));
    CHECK(ps3_hle_has(QUERY));
    // Synthetic lifecycle: internal module loaded, queried, then unloaded.
    CHECK(call(LOAD,0xF044)==0);
    CHECK(call(QUERY,0xF044)==0);
    CHECK(call(UNLOAD,0xF044,0xDEADBEEF)==0);
    CHECK(call(QUERY,0xF044)==0x80012003);
    CHECK(call(UNLOAD,0xF044)==0x80012003);
    CHECK(call(LOAD,0xF044)==0);
    CHECK(call(UNLOAD,0xF044,UINT64_MAX)==0); // Extra GPRs are not pointers or arguments.
    // Independent literal allowlist from RPCS3 LOADABLE_INTERNAL_MODULES_EX.
    const uint16_t allowed[]={
        0xF005,0xF00A,0xF010,0xF013,0xF019,0xF01B,0xF01D,0xF01E,
        0xF020,0xF021,0xF022,0xF023,0xF024,0xF026,0xF028,0xF029,
        0xF02A,0xF02B,0xF02C,0xF02E,0xF02F,0xF030,0xF032,0xF033,
        0xF034,0xF035,0xF036,0xF039,0xF03A,0xF03D,0xF03E,0xF03F,
        0xF040,0xF041,0xF043,0xF044,0xF045,0xF046,0xF047,0xF048,
        0xF049,0xF04A,0xF04D,0xF04E,0xF04F,0xF050,0xF051,0xF052,
        0xF054,0xF055,0xF056};
    for(uint32_t id=0xF000;id<0xF057;++id) {
        bool accepts=false;for(uint16_t a:allowed)if(id==a)accepts=true;
        const bool loadable=accepts && id!=0xF020 && id!=0xF024 && id!=0xF03E;
        CHECK(call(LOAD,id)==(loadable?0u:0x80012002u));
        CHECK(call(QUERY,id)==(loadable?0u:0x80012003u));
        if(loadable)CHECK(cellSysmoduleUnloadModule(uint16_t(id))==0);
        // The pre-existing public tracker admits all internal IDs. This lets
        // rejection tests prove that Ex cannot unload a disallowed loaded ID.
        CHECK(cellSysmoduleLoadModule(uint16_t(id))==0);
        CHECK(call(UNLOAD,id)==(accepts?0u:0x80012002u));
        CHECK(call(QUERY,id)==(accepts?0x80012003u:0u));
        if(!accepts)CHECK(cellSysmoduleUnloadModule(uint16_t(id))==0);
        CHECK(call(UNLOAD,id)==(accepts?0x80012003u:0x80012002u));
    }
    CHECK(cellSysmoduleLoadModule(0x44)==0);
    CHECK(call(LOAD,0xF044)==0);
    const uint16_t invalid[]={0,0x44,0xEFFF,0xF057,0xF100,0xFFFF};
    for(uint16_t id:invalid)CHECK(call(UNLOAD,id,UINT64_MAX)==0x80012002);
    CHECK(cellSysmoduleIsLoaded(0x44)==0);
    CHECK(call(QUERY,0xF044)==0);
    // Integer argument is u16; upper bits are outside its PPU ABI value.
    CHECK(call(UNLOAD,0x12340000F044ull)==0);
    CHECK(cellSysmoduleIsLoaded(0x44)==0);
    CHECK(call(QUERY,0xF044)==0x80012003);
    std::printf("PASS: extended unload through generated NID/PPU dispatch, state/error policy, namespace and canaries (%u checks)\n",checks);
}
