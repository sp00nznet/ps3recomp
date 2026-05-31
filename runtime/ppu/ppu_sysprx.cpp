/*
 * ps3recomp - sysPrxForUser CRT (boot-critical HLE)
 *
 * The first firmware functions a PS3 program calls at startup come from
 * sysPrxForUser (the libc/CRT bridge). Some need the full ppu_context (e.g.
 * sys_initialize_tls sets the thread pointer r13), so they register as
 * context-aware handlers (ps3_hle_register_ctx) rather than through the generic
 * integer-ABI table.
 *
 * NIDs are computed from the names (ps3_compute_nid), so this stays correct
 * without hand-written NID literals.
 */
#include "ppu_recomp.h"     /* ppu_context */
#include "ps3emu/nid.h"     /* ps3_compute_nid */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern "C" uint8_t* vm_base;
extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
extern "C" uint32_t vm_read32(uint64_t a);
extern "C" void     vm_write32(uint64_t a, uint32_t v);
extern "C" void     vm_write64(uint64_t a, uint64_t v);

/* Simple bump allocator for TLS areas, in a free vm region below the stack. */
static uint32_t s_tls_next = 0x0E000000u;

/* sys_initialize_tls(u64 main_thread_id, u32 tls_seg_addr, u32 tls_seg_size,
 *                     u32 tls_mem_size) -- set up the main thread's TLS block
 * and point r13 (the PPC64 thread pointer) at it. TLS variables are accessed
 * at r13 - 0x7000 (the static TLS block bias). */
static void sys_initialize_tls(ppu_context* ctx)
{
    uint32_t seg_addr = (uint32_t)ctx->gpr[4];
    uint32_t seg_size = (uint32_t)ctx->gpr[5];
    uint32_t mem_size = (uint32_t)ctx->gpr[6];

    uint32_t block = s_tls_next;
    uint32_t total = ((mem_size + 0x7000u + 0x1000u) + 0xFFFu) & ~0xFFFu;
    s_tls_next += total;

    if (seg_addr && seg_size) memcpy(vm_base + block, vm_base + seg_addr, seg_size);
    if (mem_size > seg_size)  memset(vm_base + block + seg_size, 0, mem_size - seg_size);

    ctx->gpr[13] = block + 0x7000u;   /* thread pointer; TLS data at r13-0x7000 */
    ctx->gpr[3]  = 0;                  /* CELL_OK */
    fprintf(stderr, "[crt] sys_initialize_tls: block 0x%08X, r13=0x%08X (seg 0x%X+%u, mem %u)\n",
            block, (uint32_t)ctx->gpr[13], seg_addr, seg_size, mem_size);
}

/* sys_time_get_system_time() -> microseconds since boot (monotonic-ish). */
static void sys_time_get_system_time(ppu_context* ctx)
{
    static uint64_t t = 0;
    t += 1000;                         /* advance so callers see time progress */
    ctx->gpr[3] = t;
}

/* sys_process_is_stack(u32 addr) -> 1 if addr is in the stack region. We model
 * a single stack just below the TLS region; good enough for boot checks. */
static void sys_process_is_stack(ppu_context* ctx)
{
    uint32_t a = (uint32_t)ctx->gpr[3];
    ctx->gpr[3] = (a >= 0x0E000000u && a < 0x10000000u) ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * Lightweight mutex (sys_lwmutex) — sysPrxForUser.
 *
 * The CRT guards global/singleton initialization with lwmutexes. If create is
 * a no-op that never initializes the structure, the guarded init is skipped
 * and the protected registry is left with null function pointers (the early
 * boot then spins calling a null vtable entry). We model the structure for
 * real; locking is a no-op owner stamp (the boot is single-threaded).
 *
 * sys_lwmutex_t (big-endian, 24 bytes):
 *   +0x00 owner (u32)   +0x04 waiter (u32)   +0x08 attribute (u32)
 *   +0x0C recursive_count (u32)   +0x10 sleep_queue (u32)   +0x14 pad
 * sys_lwmutex_attribute_t: +0x00 protocol  +0x04 recursive  +0x08 name[8]
 * -----------------------------------------------------------------------*/
#define LWM_OWNER  0x00
#define LWM_ATTR   0x08
#define LWM_RECUR  0x0C
#define LWM_TID    1u   /* single-thread boot: one fixed owner id */

static void sys_lwmutex_create(ppu_context* ctx)
{
    uint32_t lwm  = (uint32_t)ctx->gpr[3];
    uint32_t attr = (uint32_t)ctx->gpr[4];
    uint32_t protocol = attr ? vm_read32(attr + 0) : 0;
    vm_write32(lwm + 0x00, 0);          /* owner */
    vm_write32(lwm + 0x04, 0);          /* waiter */
    vm_write32(lwm + LWM_ATTR, protocol);
    vm_write32(lwm + LWM_RECUR, 0);     /* recursive_count */
    vm_write32(lwm + 0x10, 0);          /* sleep_queue */
    vm_write32(lwm + 0x14, 0);
    ctx->gpr[3] = 0;
}
static void sys_lwmutex_lock(ppu_context* ctx)
{
    uint32_t lwm = (uint32_t)ctx->gpr[3];
    vm_write32(lwm + LWM_OWNER, LWM_TID);
    vm_write32(lwm + LWM_RECUR, vm_read32(lwm + LWM_RECUR) + 1);
    ctx->gpr[3] = 0;   /* CELL_OK */
}
static void sys_lwmutex_trylock(ppu_context* ctx) { sys_lwmutex_lock(ctx); }
static void sys_lwmutex_unlock(ppu_context* ctx)
{
    uint32_t lwm = (uint32_t)ctx->gpr[3];
    uint32_t rc = vm_read32(lwm + LWM_RECUR);
    if (rc) vm_write32(lwm + LWM_RECUR, rc - 1);
    if (rc <= 1) vm_write32(lwm + LWM_OWNER, 0);
    ctx->gpr[3] = 0;
}

/* sys_ppu_thread_get_id(vm::ptr<u64> id) -> *id = main thread id (1). */
static void sys_ppu_thread_get_id(ppu_context* ctx)
{
    uint32_t p = (uint32_t)ctx->gpr[3];
    if (p) vm_write64(p, 1);
    ctx->gpr[3] = 0;
}

/* sys_mmapper_allocate_memory(u32 size, u64 flags, vm::ptr<u32> mem_id) ->
 * hand back a unique opaque id; the backing is the flat VM, so the later
 * search_and_map just needs a non-zero id to track. */
static void sys_mmapper_allocate_memory(ppu_context* ctx)
{
    static uint32_t s_next_id = 0x1000;
    uint32_t mem_id_ptr = (uint32_t)ctx->gpr[5];
    if (mem_id_ptr) vm_write32(mem_id_ptr, s_next_id);
    s_next_id++;
    ctx->gpr[3] = 0;
}

/* A handful of CRT helpers the early boot tends to hit; accept and continue. */
static void crt_ok(ppu_context* ctx) { ctx->gpr[3] = 0; }

extern "C" void ppu_sysprx_register(void)
{
    ps3_hle_register_ctx(ps3_compute_nid("sys_initialize_tls"),       "sys_initialize_tls",       sys_initialize_tls);
    ps3_hle_register_ctx(ps3_compute_nid("sys_time_get_system_time"), "sys_time_get_system_time", sys_time_get_system_time);
    ps3_hle_register_ctx(ps3_compute_nid("sys_process_is_stack"),     "sys_process_is_stack",     sys_process_is_stack);
    /* Atexit registration: nothing to do at boot, just succeed. */
    ps3_hle_register_ctx(ps3_compute_nid("_sys_process_atexitspawn"), "_sys_process_atexitspawn", crt_ok);
    ps3_hle_register_ctx(ps3_compute_nid("_sys_process_at_Exitspawn"),"_sys_process_at_Exitspawn",crt_ok);

    /* Lightweight mutex family (guards global/singleton init in the CRT). */
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwmutex_create"),  "sys_lwmutex_create",  sys_lwmutex_create);
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwmutex_destroy"), "sys_lwmutex_destroy", crt_ok);
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwmutex_lock"),    "sys_lwmutex_lock",    sys_lwmutex_lock);
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwmutex_unlock"),  "sys_lwmutex_unlock",  sys_lwmutex_unlock);
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwmutex_trylock"), "sys_lwmutex_trylock", sys_lwmutex_trylock);

    /* Thread id + memory manager (high-frequency boot imports). The flat VM
     * means map/unmap/free are no-ops: the memory already exists everywhere. */
    ps3_hle_register_ctx(ps3_compute_nid("sys_ppu_thread_get_id"),      "sys_ppu_thread_get_id",      sys_ppu_thread_get_id);
    ps3_hle_register_ctx(ps3_compute_nid("sys_mmapper_allocate_memory"), "sys_mmapper_allocate_memory", sys_mmapper_allocate_memory);
    ps3_hle_register_ctx(ps3_compute_nid("sys_mmapper_map_memory"),     "sys_mmapper_map_memory",     crt_ok);
    ps3_hle_register_ctx(ps3_compute_nid("sys_mmapper_unmap_memory"),   "sys_mmapper_unmap_memory",   crt_ok);
    ps3_hle_register_ctx(ps3_compute_nid("sys_mmapper_free_memory"),    "sys_mmapper_free_memory",    crt_ok);
}
