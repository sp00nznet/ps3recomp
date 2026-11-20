/*
 * Host stubs for the fast-HLE TOC unit test.
 *
 * ppu_hle.cpp is normally linked with ppu_loader.cpp and the rest of the PPU
 * scaffold. This test only needs ps3_hle_call's dispatch and the new TOC spill,
 * so the loader-side symbols it references are stubbed here. None of the
 * stubbed paths are entered by test_hle_fast_toc.
 */
#define PPU_NO_INLINE_LOADS   /* use the out-of-line accessors stubbed below */
#define PPU_NO_INLINE_STORES
#include "ppu_recomp.h"

#include <stdint.h>
#include <string.h>

static uint8_t g_vm[0x10000];

static inline uint32_t bswap32(uint32_t v) { return __builtin_bswap32(v); }
static inline uint64_t bswap64(uint64_t v) { return __builtin_bswap64(v); }

extern "C" {

uint8_t* vm_base = g_vm;

uint32_t vm_read32(uint64_t addr)
{
    uint32_t v;
    memcpy(&v, vm_base + (uint32_t)addr, sizeof(v));
    return bswap32(v);
}

uint64_t vm_read64(uint64_t addr)
{
    uint64_t v;
    memcpy(&v, vm_base + (uint32_t)addr, sizeof(v));
    return bswap64(v);
}

void vm_write32(uint64_t addr, uint32_t v)
{
    v = bswap32(v);
    memcpy(vm_base + (uint32_t)addr, &v, sizeof(v));
}

void vm_write64(uint64_t addr, uint64_t v)
{
    v = bswap64(v);
    memcpy(vm_base + (uint32_t)addr, &v, sizeof(v));
}

uint32_t prx_resolve_export(uint32_t nid) { (void)nid; return 0; }
void     ps3_indirect_call(ppu_context* ctx) { (void)ctx; }
void     ppu_dump_guest_stack(ppu_context* ctx, const char* tag) { (void)ctx; (void)tag; }
void     ppu_dump_bctrl_ring(uint32_t tid, const char* tag) { (void)tid; (void)tag; }
void     ppu_guest_callstack(const char* tag) { (void)tag; }
void     ppu_prof_stamp(void* ctx, unsigned lr) { (void)ctx; (void)lr; }
uint32_t ppu_prof_resolve_host(void* ra) { (void)ra; return 0; }
void     ps3_msf(const char* fmt, ...) { (void)fmt; }

uint64_t ppu_guest_call(uint32_t opd,
                        uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7)
{
    (void)opd; (void)a0; (void)a1; (void)a2; (void)a3;
    (void)a4; (void)a5; (void)a6; (void)a7;
    return 0;
}

uint32_t g_ydkj_real_spurs_ea = 0;
uint32_t g_ydkj_real_taskset_ea = 0;

} /* extern "C" */
