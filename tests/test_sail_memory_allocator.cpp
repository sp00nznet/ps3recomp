/* Exercise generated NID registration and the real PPU HLE bridge. */
#include "ppu_context.h"
#include "../libs/codec/cellSail.h"
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
int cellfs_translate_path(const char*,char*,size_t) { return -1; }
int32_t cellPamfReaderInitialize(void*,void*,uint64_t,uint32_t) { std::abort(); }
int32_t cellPamfReaderGetStreamIndex(void*) { std::abort(); }
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

static_assert(sizeof(CellSailMemAllocatorFuncs)==8, "guest callback pair");
static_assert(sizeof(CellSailMemAllocator)==12, "guest allocator record");
static unsigned checks;
#define CHECK(x) do { ++checks; if(!(x)) { std::fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);return 1; } } while(0)
static uint32_t call(uint32_t nid,uint64_t a=0,uint64_t b=0,uint64_t c=0,uint64_t d=0,uint64_t e=0) {
 ppu_context ctx{};ctx.gpr[1]=0x8000;ctx.gpr[2]=0x12345678;
 ctx.gpr[3]=a;ctx.gpr[4]=b;ctx.gpr[5]=c;ctx.gpr[6]=d;ctx.gpr[7]=e;
 ps3_hle_call(nid,&ctx);if(ctx.gpr[2]!=0x12345678)std::abort();return uint32_t(ctx.gpr[3]);
}
int main() {
 static uint8_t memory[0x10000];vm_base=memory;ppu_hle_register_all();
 constexpr uint32_t INIT=0x346EBBA3,ARG=0x80610701;
 vm_write32(0x1000,0xDEADBEEF);vm_write32(0x1004,0xDEADBEEF);vm_write32(0x1008,0xDEADBEEF);vm_write32(0x100C,0x55667788);
 vm_write32(0x2000,0x3000);vm_write32(0x2004,0x4000);
 CHECK(ps3_hle_has(INIT));CHECK(call(INIT,0x1000,0x2000,0x11223344)==0);
 CHECK(vm_read32(0x1000)==0x3000);CHECK(vm_read32(0x1004)==0x4000);CHECK(vm_read32(0x1008)==0x11223344);CHECK(vm_read32(0x100C)==0x55667788);
 CHECK(call(INIT,0,0x2000)==ARG);CHECK(call(INIT,0x1001,0x2000)==ARG);
 CHECK(call(INIT,0xFFFC,0x2000)==ARG);CHECK(call(INIT,0x1000,0)==ARG);
 CHECK(call(INIT,0x1000,0x2001)==ARG);CHECK(call(INIT,0x1000,0xFFFC)==ARG);
 CHECK(vm_read32(0x1000)==0x3000);CHECK(vm_read32(0x1004)==0x4000);CHECK(vm_read32(0x1008)==0x11223344);
 // Copy callbacks before storing: destination may overlap the input table.
 vm_write32(0x3004,0x5000);vm_write32(0x3008,0x6000);
 CHECK(call(INIT,0x3000,0x3004,0x77889900)==0);
 CHECK(vm_read32(0x3000)==0x5000);CHECK(vm_read32(0x3004)==0x6000);CHECK(vm_read32(0x3008)==0x77889900);
 CHECK(call(INIT,0xFFF8,0x2000)==ARG);
 std::printf("%u allocator layout checks PASS\n",checks);return 0;
}
