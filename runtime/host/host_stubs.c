/*
 * Symbols the HLE library expects from the PPU loader sources.
 *
 * runtime/ppu/ and runtime/host/ are both excluded from the library target
 * (CMakeLists.txt), so a host that runs no lifted game still has to satisfy the
 * handful of out-of-line references the HLE modules make into the PPU loader.
 * These are inert but correct: a game-less host has no reservation state to
 * track, and the guest stores still have to be big-endian.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../ppu/ppu_context.h"   /* ppu_context, PPU_THREAD_LOCAL */

extern uint8_t* vm_base;

/* ppu_loader.cpp - PPU reservation bookkeeping. The ppu_memory.h inlines call
 * these on every store so lwarx/stwcx reservations can be broken; with no PPU
 * threads there is never a live reservation. */
int  g_resv_store_active = 0;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }

/* ppu_loader.cpp - each guest thread registers its context so a concurrent
 * stwcx can break its reservation. With g_resv_store_active off there is
 * nothing to register into. */
void ppu_resv_register(void* ctx) { (void)ctx; }

/* ppu_loader.cpp - the size of the guest address space the loader mapped.
 * This host calloc's its own VM and passes bounds explicitly, so the bounds
 * checks keyed off this stay out of the way (see VM_READY in vm.h, which
 * takes vm_base != 0 as the other way to be ready). */
uint32_t ppu_vm_size = 0;

/* ppu_loader.cpp - out-of-line big-endian guest stores. */
void vm_write8(uint64_t addr, uint8_t v)
{
    vm_base[(uint32_t)addr] = v;
}

void vm_write32(uint64_t addr, uint32_t v)
{
    v = __builtin_bswap32(v);
    memcpy(vm_base + (uint32_t)addr, &v, 4);
}

void vm_write16(uint64_t addr, uint16_t v)
{
    v = __builtin_bswap16(v);
    memcpy(vm_base + (uint32_t)addr, &v, 2);
}

void vm_write64(uint64_t addr, uint64_t v)
{
    v = __builtin_bswap64(v);
    memcpy(vm_base + (uint32_t)addr, &v, 8);
}

/* ppu_loader.cpp - out-of-line big-endian guest loads. The loader's version
 * bounds-checks against the mapped image; this host owns the whole arena. */
uint32_t vm_read32(uint64_t addr)
{
    uint32_t v;
    memcpy(&v, vm_base + (uint32_t)addr, 4);
    return __builtin_bswap32(v);
}

/* ppu_fs.cpp - VFS root consulted by cellGame/cellFs path mapping. */
const char* ppu_vfs_root = ".";

/* ---------------------------------------------------------------------------
 * Title-diagnostic hooks the HLE modules call into the PPU loader.
 *
 * Every one of these reports something about a lifted game: its guest call
 * chain, its host call chain, a breadcrumb table, a guard page in its address
 * space. A host with no game has nothing to report, and none of them feeds a
 * decision -- they print and return. cellAudio pulls sys_event.c in, which is
 * where most of these are reached from.
 * -----------------------------------------------------------------------*/
uint32_t g_barrier_sync_watch = 0;

void lbp_breadcrumb_dump(const char* tag)   { (void)tag; }
void ppu_log_host_chain(const char* tag)    { (void)tag; }
void ppu_guest_callstack(const char* tag)   { (void)tag; }
void ppu_guard_page(uint32_t guest_ea)      { (void)guest_ea; }
void ppu_dump_guest_stack(void* ctx, const char* tag) { (void)ctx; (void)tag; }

void ppu_guest_caller(char* out, size_t n)
{
    if (out && n) out[0] = '\0';
}

/* ppu_sysprx.cpp - the HLE registration table a lifted game's imports are
 * bound through. Nothing here imports anything. */
void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(void*))
{
    (void)nid; (void)name; (void)fn;
}

/* Inline write-watch hooks. ppu_memory.h expands every vm_write* into a range
 * test against g_ww_lo/g_ww_hi plus a call to ps3_ww_report_inline, and those
 * live in runtime/ppu/ppu_loader.cpp -- which CMake excludes from the runtime
 * library because it is compiled per-game. A game link therefore supplies them
 * and this host does not, so it failed to link with three undefined symbols.
 *
 * lo == hi leaves the watch permanently empty, so the test is a compare that
 * never fires and the reporter is never reached. */
unsigned int g_ww_lo = 0, g_ww_hi = 0;

void ps3_ww_report_inline(unsigned int addr, unsigned long long val, int width)
{
    (void)addr; (void)val; (void)width;
}

/* ppu_loader.cpp - the link register of the guest context the calling host
 * thread is running, for the env-gated lwmutex trace in sysPrxForUser.c. A
 * host with no guest has no such register; zero is what the trace prints for
 * a caller it cannot name. */
unsigned int ppu_active_lr(void) { return 0; }

PPU_THREAD_LOCAL ppu_context* g_active_ctx = 0;
uint32_t g_spu_image_src_ea = 0;
uint32_t g_spu_image_ls_start = 0;
uint32_t g_spu_image_span = 0;
void ppu_dump_bctrl_ring(uint32_t a, const char* tag) { (void)a; (void)tag; }
uint32_t ps3_spu_image_source_ea(uint32_t img_ea) { return img_ea; }
