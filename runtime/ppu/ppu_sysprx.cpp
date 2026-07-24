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
#include <stdlib.h>
#include <string.h>
#include <mutex>
#include <map>

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

/* sys_time_get_system_time() -> microseconds since boot, REAL time.
 * This was a fake counter advancing 1 ms PER CALL ("so callers see time
 * progress") -- so any guest clock built on it ran at call-rate, not
 * wall-time. LBP's Bink movie clock (sysGetSystemTime import 0x8461E528
 * lands here) paced the intro at ~1/10th speed: video decoded at <1 fps,
 * the audio preload threshold took forever to fill (movie stayed silent),
 * and every frontend animation timed off it crawled. */
static void sys_time_get_system_time(ppu_context* ctx)
{
#ifdef _WIN32
    static LARGE_INTEGER s_freq, s_base;
    if (!s_freq.QuadPart) { QueryPerformanceFrequency(&s_freq); QueryPerformanceCounter(&s_base); }
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    ctx->gpr[3] = (uint64_t)((now.QuadPart - s_base.QuadPart) * 1000000ull / (uint64_t)s_freq.QuadPart);
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    ctx->gpr[3] = (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
#endif
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

/* YDKJ_REALLWM: real per-lwmutex host mutex. The lock impl below was a no-op
 * (single-thread boot assumption) providing NO mutual exclusion — fatal now that
 * SPURSKERNEL/GThreads/AsyncLoad run concurrently and share GFx heap structures
 * bracketed by sys_lwmutex_lock/unlock (func_00470710/730). A real mutex keyed on
 * the guest lwmutex EA serializes them, fixing the free-list + pointer races
 * systemically (supersedes the coarse YDKJ_HEAPLOCK band-aid in the recomp).
 * ponytail: map<EA,recursive_mutex>; a crashed guest thread holding a lock still
 *           deadlocks (same as real hardware) — that's a separate lifter bug. */
static std::mutex g_lwm_map_mtx;
static std::map<uint32_t, std::recursive_mutex> g_lwm_mtxs;
static inline int ydkj_reallwm(){ static int v=-1; if(v<0) v=getenv("YDKJ_REALLWM")?1:0; return v; }
static std::recursive_mutex& lwm_host_mutex(uint32_t lwm){
    std::lock_guard<std::mutex> g(g_lwm_map_mtx);
    return g_lwm_mtxs[lwm];
}

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
    if (ydkj_reallwm()) lwm_host_mutex(lwm).lock();
    vm_write32(lwm + LWM_OWNER, LWM_TID);
    vm_write32(lwm + LWM_RECUR, vm_read32(lwm + LWM_RECUR) + 1);
    ctx->gpr[3] = 0;   /* CELL_OK */
}
static void sys_lwmutex_trylock(ppu_context* ctx)
{
    uint32_t lwm = (uint32_t)ctx->gpr[3];
    if (ydkj_reallwm() && !lwm_host_mutex(lwm).try_lock()) { ctx->gpr[3] = 0x80010005u; return; } /* EBUSY */
    vm_write32(lwm + LWM_OWNER, LWM_TID);
    vm_write32(lwm + LWM_RECUR, vm_read32(lwm + LWM_RECUR) + 1);
    ctx->gpr[3] = 0;
}
static void sys_lwmutex_unlock(ppu_context* ctx)
{
    uint32_t lwm = (uint32_t)ctx->gpr[3];
    uint32_t rc = vm_read32(lwm + LWM_RECUR);
    if (rc) vm_write32(lwm + LWM_RECUR, rc - 1);
    if (rc <= 1) vm_write32(lwm + LWM_OWNER, 0);
    if (ydkj_reallwm()) lwm_host_mutex(lwm).unlock();
    ctx->gpr[3] = 0;
}

/* sys_lwcond (sysPrxForUser) — guest-side condition variable, paired with an
 * lwmutex. The CRT and (newly) libsre's cellSpurs create/wait/signal these. Like
 * sys_lwmutex above, model it directly in guest memory so the args stay GUEST
 * EAs (the generic adapter would pass them raw and the C sysPrxForUser impl
 * deref'd them as host pointers -> AV during cellSpurs init). A no-op wait is
 * adequate here: the CRT/SPURS paths that reach us use these for one-shot init
 * handshakes, not long-term blocking. sys_lwcond_t: +0x00 lwmutex EA (be64),
 * +0x08 lwcond_queue id. */
static void sys_lwcond_create(ppu_context* ctx)
{
    static uint32_t s_lwcond_id = 0x4C000000u;
    uint32_t lwcond  = (uint32_t)ctx->gpr[3];
    uint32_t lwmutex = (uint32_t)ctx->gpr[4];
    vm_write64(lwcond + 0x00, (uint64_t)lwmutex);
    vm_write32(lwcond + 0x08, ++s_lwcond_id);
    ctx->gpr[3] = 0;
}
static void sys_lwcond_destroy(ppu_context* ctx)    { ctx->gpr[3] = 0; }
static void sys_lwcond_signal(ppu_context* ctx)     { ctx->gpr[3] = 0; }
static void sys_lwcond_signal_all(ppu_context* ctx) { ctx->gpr[3] = 0; }
static void sys_lwcond_signal_to(ppu_context* ctx)  { ctx->gpr[3] = 0; }
static void sys_lwcond_wait(ppu_context* ctx)       { ctx->gpr[3] = 0; }

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
/* id -> size so sys_mmapper_search_and_map (lv2 337) can lay blocks out
 * without overlap. Ids are dense from 0x1000. */
static uint32_t s_mm_sizes[256];
static uint32_t s_mmapper_next_id = 0x1000;
extern "C" uint32_t ps3_mmapper_block_size(uint32_t mem_id)
{
    uint32_t i = mem_id - 0x1000u;
    return (i < 256) ? s_mm_sizes[i] : 0;
}

static uint32_t mmapper_new_id(uint32_t size)
{
    uint32_t id = s_mmapper_next_id++;
    if (id - 0x1000u < 256) s_mm_sizes[id - 0x1000u] = size;
    return id;
}

static void sys_mmapper_allocate_memory(ppu_context* ctx)
{
    uint32_t size       = (uint32_t)ctx->gpr[3];
    uint32_t mem_id_ptr = (uint32_t)ctx->gpr[5];
    uint32_t id         = mmapper_new_id(size);
    if (getenv("FLOW_MEMTRACE"))
        fprintf(stderr, "[mmapper] allocate_memory(size=0x%X flags=0x%llX id_ptr=0x%X) -> id 0x%X\n",
                size, (unsigned long long)ctx->gpr[4], mem_id_ptr, id);
    if (mem_id_ptr) vm_write32(mem_id_ptr, id);
    ctx->gpr[3] = 0;
}
/* sys_mmapper_allocate_memory_from_container(u32 size, u32 container, u64 flags,
 * vm::ptr<u32> mem_id) -> id in *r6. flОw's CRT uses this for its heap/mutex pool;
 * it was previously UNregistered (CRT saw failure -> "not enough memory"). */
static void sys_mmapper_allocate_memory_from_container(ppu_context* ctx)
{
    uint32_t size = (uint32_t)ctx->gpr[3];
    uint32_t mem_id_ptr = (uint32_t)ctx->gpr[6];
    uint32_t id = mmapper_new_id(size);
    if (getenv("FLOW_MEMTRACE"))
        fprintf(stderr, "[mmapper] alloc_from_container(size=0x%X cid=0x%X flags=0x%llX id_ptr=0x%X) -> id 0x%X\n",
                size, (uint32_t)ctx->gpr[4], (unsigned long long)ctx->gpr[5], mem_id_ptr, id);
    if (mem_id_ptr) vm_write32(mem_id_ptr, id);
    ctx->gpr[3] = 0;
}

/* A handful of CRT helpers the early boot tends to hit; accept and continue. */
static void crt_ok(ppu_context* ctx) { ctx->gpr[3] = 0; }

/* Real preemptive thread create/exit live in the lv2 syscall layer
 * (syscalls/sys_ppu_thread.c) and spawn a host thread that runs the guest
 * entry through the recompiled code. The CRT also reaches them as
 * sysPrxForUser import NIDs (gen_hle_nids can't see them — they're not defined
 * in the sysPrxForUser lib), so bridge the NIDs to the same implementation.
 * Without this the CRT's thread/static-init runs through an uninitialised
 * object table and calls heap addresses as function pointers. */
extern "C" int64_t sys_ppu_thread_create(ppu_context* ctx);
extern "C" int64_t sys_ppu_thread_exit(ppu_context* ctx);
/* Must write the int64 result into r3 -- the game checks it (0 == CELL_OK).
 * Dropping it left r3 = the tid_out arg, read as a nonzero "create failed"
 * (flОw's PSSGSPUPrintfServerInitialize aborted PhyreEngine init on this). */
static void hle_ppu_thread_create(ppu_context* ctx) { ctx->gpr[3] = (uint64_t)sys_ppu_thread_create(ctx); }
static void hle_ppu_thread_exit(ppu_context* ctx)   { sys_ppu_thread_exit(ctx); }

/* _cellGcmInitBody (NID 0x15BAE46B) -- the GCM init every PS3 game calls via the
 * cellGcmInit() SDK macro. cellGcmSys.c provides the layout-correct core
 * (cellGcmSetupContext) but needs the owning vm to allocate the guest
 * CellGcmContextData and write the game's context-out pointer; supply those as
 * callbacks. Without this the game's GCM context stays null -> null deref. */
typedef unsigned int (*CellGcmGuestAlloc)(unsigned int, unsigned int);
typedef void (*CellGcmGuestWrite32)(unsigned int, unsigned int);
extern "C" unsigned int cellGcmSetupContext(unsigned int ctx_out_addr,
    unsigned int cmdSize, unsigned int ioSize, unsigned int ioAddress,
    CellGcmGuestAlloc galloc, CellGcmGuestWrite32 gwrite32);

static unsigned int gcm_guest_alloc(unsigned int size, unsigned int align)
{
    /* Bump from a small scratch region below the main stack (0x0FF00000) and
     * above the TLS image -- a few control structs, never freed. */
    static unsigned int bump = 0x0F800000u;
    if (align < 16) align = 16;
    bump = (bump + align - 1) & ~(align - 1);
    unsigned int a = bump;
    bump += (size + 15u) & ~15u;
    return a;
}
static void gcm_guest_write32(unsigned int addr, unsigned int val) { vm_write32(addr, val); }

/* FIFO command-buffer-full callback. cellGcmSetupContext points the guest
 * context's callback OPD at GCM_FIFO_CALLBACK_SENTINEL_EA; the title's inline
 * gcmReserve calls context->callback(context, count) on ring wrap, which the
 * indirect dispatcher routes here. r3 = guest context EA. Must match the sentinel
 * define in libs/video/cellGcmSys.c. */
#define GCM_FIFO_CALLBACK_SENTINEL_EA 0x03002F00u
extern "C" void cellGcm_fifo_recycle(unsigned int ctx_ea);
extern "C" void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
static void hle_gcm_callback(ppu_context* ctx)
{
    cellGcm_fifo_recycle((unsigned int)ctx->gpr[3]);   /* r3 = context EA */
    ctx->gpr[3] = 0;                                   /* CELL_OK */
}

static void hle_cellGcmInitBody(ppu_context* ctx)
{
    uint32_t ctx_out = (uint32_t)ctx->gpr[3];
    uint32_t cmdSize = (uint32_t)ctx->gpr[4];
    uint32_t ioSize  = (uint32_t)ctx->gpr[5];
    uint32_t ioAddr  = (uint32_t)ctx->gpr[6];
    fprintf(stderr, "[HLE] _cellGcmInitBody(ctx_out=0x%08X, cmdSize=0x%X, ioSize=0x%X, ioAddr=0x%X)\n",
            ctx_out, cmdSize, ioSize, ioAddr);
    cellGcmSetupContext(ctx_out, cmdSize, ioSize, ioAddr, gcm_guest_alloc, gcm_guest_write32);
    ctx->gpr[3] = 0;   /* CELL_OK */
}

/* _sys_spu_image_import (sysPrxForUser NID 0xEBE5F72F) -- the user-space wrapper libsre
 * uses to parse the SPURS-kernel SPU ELF into a sys_spu_image (entry+segs) WITHOUT the
 * syscall. Ported from D:/recomp/ps3. Without it the NID is unresolved -> returns 0 ->
 * the SPU image is never parsed -> the 5 cellSpurs SPU threads come up with a garbage
 * entry (e.g. 0x5B555253) instead of the real kernel entry (0x818) -> SPURS never
 * bootstraps -> CellSpurs instance @0x40009F00 stays empty -> menu SPU work stalls. */
static void hle_sys_spu_image_import(ppu_context* ctx)
{
    uint32_t img_ea = (uint32_t)ctx->gpr[3];
    uint32_t src_ea = (uint32_t)ctx->gpr[4];
    fprintf(stderr, "[HLE] _sys_spu_image_import(img=0x%08X src=0x%08X r5=0x%08X r6=0x%08X)\n",
            img_ea, src_ea, (uint32_t)ctx->gpr[5], (uint32_t)ctx->gpr[6]);
    if (!img_ea || !src_ea || !vm_base) { ctx->gpr[3] = 0; return; }
    const uint8_t* e = vm_base + src_ea;
    if (!(e[0]==0x7F && e[1]=='E' && e[2]=='L' && e[3]=='F')) {
        fprintf(stderr, "[HLE] _sys_spu_image_import: src not an ELF -> no-op\n");
        fflush(stderr); ctx->gpr[3] = 0; return;
    }
    uint16_t machine = (uint16_t)((e[0x12] << 8) | e[0x13]);   /* 23 = SPU */
    uint32_t entry   = vm_read32(src_ea + 0x18);
    uint32_t phoff   = vm_read32(src_ea + 0x1C);
    uint16_t phentsz = (uint16_t)((e[0x2A] << 8) | e[0x2B]); if (!phentsz) phentsz = 0x20;
    uint16_t phnum   = (uint16_t)((e[0x2C] << 8) | e[0x2D]);
    static uint32_t s_seg_bump = 0x0D000000u;
    uint32_t segs_ea = s_seg_bump; int nsegs = 0;
    for (uint16_t i = 0; i < phnum && nsegs < 32; i++) {
        uint32_t ph = phoff + (uint32_t)i * phentsz;
        if (vm_read32(src_ea + ph + 0x00) != 1) continue;      /* PT_LOAD */
        uint32_t p_off = vm_read32(src_ea + ph + 0x04);
        uint32_t p_va  = vm_read32(src_ea + ph + 0x08);
        uint32_t p_fsz = vm_read32(src_ea + ph + 0x10);
        uint32_t p_msz = vm_read32(src_ea + ph + 0x14);
        uint32_t seg = segs_ea + (uint32_t)nsegs * 0x18;       /* COPY */
        vm_write32(seg + 0x00, 1); vm_write32(seg + 0x04, p_va);
        vm_write32(seg + 0x08, p_fsz); vm_write32(seg + 0x10, 0);
        vm_write32(seg + 0x14, src_ea + p_off); nsegs++;
        if (p_msz > p_fsz && nsegs < 32) {                     /* BSS tail -> FILL 0 */
            seg = segs_ea + (uint32_t)nsegs * 0x18;
            vm_write32(seg + 0x00, 2); vm_write32(seg + 0x04, p_va + p_fsz);
            vm_write32(seg + 0x08, p_msz - p_fsz);
            vm_write32(seg + 0x10, 0); vm_write32(seg + 0x14, 0); nsegs++;
        }
    }
    s_seg_bump += (uint32_t)nsegs * 0x18;
    if (s_seg_bump >= 0x0E000000u) s_seg_bump = 0x0D000000u;
    vm_write32(img_ea + 0x00, 0);                              /* type = USER */
    vm_write32(img_ea + 0x04, entry);
    vm_write32(img_ea + 0x08, nsegs ? segs_ea : 0);
    vm_write32(img_ea + 0x0C, (uint32_t)nsegs);
    fprintf(stderr, "[HLE] _sys_spu_image_import -> entry=0x%05X nsegs=%d machine=%u (SPU=23)\n",
            entry, nsegs, machine);
    fflush(stderr);
    ctx->gpr[3] = 0;
}

/* Diagnostic: libsre's internal assert/error path. _sys_printf(0x9F04F7AF) and
 * the abort NID 0x9FB6228E are both currently unresolved no-ops, so libsre's
 * failure message is swallowed and the SPURS group gets torn down blind. Read
 * the guest strings so we can see WHAT libsre is asserting on. */
static void hle_dbg_read_gstr(uint32_t p, char* buf, int cap) {
    int i = 0; if (p && vm_base) for (; i < cap-1; i++) { uint8_t c = vm_base[p+i]; if (!c) break; buf[i] = (char)c; }
    buf[i] = 0;
}
static void hle_dbg_sys_printf(ppu_context* ctx) {
    char fmt[192], s5[160], s6[160];
    hle_dbg_read_gstr((uint32_t)ctx->gpr[3], fmt, sizeof fmt);
    hle_dbg_read_gstr((uint32_t)ctx->gpr[5], s5, sizeof s5);
    hle_dbg_read_gstr((uint32_t)ctx->gpr[6], s6, sizeof s6);
    fprintf(stderr, "[libsre-printf] fmt=\"%s\" | r4=0x%llX r5-str=\"%s\" r6-str=\"%s\" r7=%lld r8=0x%llX\n",
            fmt, (unsigned long long)ctx->gpr[4], s5, s6,
            (long long)(int32_t)ctx->gpr[7], (unsigned long long)ctx->gpr[8]);
    fflush(stderr); ctx->gpr[3] = 0;
}
static void hle_dbg_abort_9FB6(ppu_context* ctx) {
    char s3[192];
    hle_dbg_read_gstr((uint32_t)ctx->gpr[3], s3, sizeof s3);
    fprintf(stderr, "[libsre-abort 0x9FB6228E] r3-str=\"%s\" r3=0x%08X r4=0x%llX lr=0x%08X\n",
            s3, (uint32_t)ctx->gpr[3], (unsigned long long)ctx->gpr[4], (uint32_t)ctx->lr);
    fflush(stderr); ctx->gpr[3] = 0;
}

extern "C" void ppu_sysprx_register(void)
{
    if (getenv("YDKJ_GFXSCAN")) {
        ps3_hle_register_ctx(0x9F04F7AFu, "_sys_printf(dbg)", hle_dbg_sys_printf);
        ps3_hle_register_ctx(0x9FB6228Eu, "libsre_abort(dbg)", hle_dbg_abort_9FB6);
    }
    ps3_hle_register_ctx(0x15BAE46Bu, "_cellGcmInitBody", hle_cellGcmInitBody);
    ps3_hle_register_ctx(0xEBE5F72Fu, "_sys_spu_image_import", hle_sys_spu_image_import);
    /* Route the GCM command-buffer-full callback (invoked indirectly via the
     * context OPD) into cellGcm_fifo_recycle so the FIFO ring recycles on wrap. */
    ppu_register_function(GCM_FIFO_CALLBACK_SENTINEL_EA, hle_gcm_callback);
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

    ps3_hle_register_ctx(ps3_compute_nid("sys_lwcond_create"),     "sys_lwcond_create",     sys_lwcond_create);
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwcond_destroy"),    "sys_lwcond_destroy",    sys_lwcond_destroy);
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwcond_signal"),     "sys_lwcond_signal",     sys_lwcond_signal);
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwcond_signal_all"), "sys_lwcond_signal_all", sys_lwcond_signal_all);
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwcond_signal_to"),  "sys_lwcond_signal_to",  sys_lwcond_signal_to);
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwcond_wait"),       "sys_lwcond_wait",       sys_lwcond_wait);

    /* Thread id + memory manager (high-frequency boot imports). The flat VM
     * means map/unmap/free are no-ops: the memory already exists everywhere. */
    ps3_hle_register_ctx(ps3_compute_nid("sys_ppu_thread_get_id"),      "sys_ppu_thread_get_id",      sys_ppu_thread_get_id);
    ps3_hle_register_ctx(ps3_compute_nid("sys_ppu_thread_create"),      "sys_ppu_thread_create",      hle_ppu_thread_create);
    ps3_hle_register_ctx(ps3_compute_nid("sys_ppu_thread_exit"),        "sys_ppu_thread_exit",        hle_ppu_thread_exit);
    ps3_hle_register_ctx(ps3_compute_nid("sys_mmapper_allocate_memory"), "sys_mmapper_allocate_memory", sys_mmapper_allocate_memory);
    ps3_hle_register_ctx(ps3_compute_nid("sys_mmapper_allocate_memory_from_container"), "sys_mmapper_allocate_memory_from_container", sys_mmapper_allocate_memory_from_container);
    ps3_hle_register_ctx(ps3_compute_nid("sys_mmapper_map_memory"),     "sys_mmapper_map_memory",     crt_ok);
    ps3_hle_register_ctx(ps3_compute_nid("sys_mmapper_unmap_memory"),   "sys_mmapper_unmap_memory",   crt_ok);
    ps3_hle_register_ctx(ps3_compute_nid("sys_mmapper_free_memory"),    "sys_mmapper_free_memory",    crt_ok);
}
