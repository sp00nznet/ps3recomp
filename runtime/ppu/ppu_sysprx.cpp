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
#ifdef _WIN32
#include <windows.h>        /* CRITICAL_SECTION for real lwmutex exclusion */
#endif

extern "C" uint8_t* vm_base;
extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
extern "C" uint32_t vm_read32(uint64_t a);
extern "C" uint64_t vm_read64(uint64_t a);
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
/* Owner id = ctx->thread_id, which is nonzero for every thread since main
 * registers as id 1 (ppu_thread_register_main). The old 0->1 fallback made
 * main alias the FIRST CREATED thread: each passed the other's recursive
 * re-lock check, both "owned" the lock, and (LBP) main + bringup emitted GCM
 * concurrently -- fences vanished mid-ring. A zero id now means an
 * unregistered context (bug); stamp a sentinel that matches no real thread. */
#define LWM_SELF(ctx) ((uint32_t)(ctx)->thread_id ? (uint32_t)(ctx)->thread_id : 0x7FFFFFFEu)

#ifdef _WIN32
static HANDLE lwm_sem(uint32_t addr);   /* fwd (defined below) */
#endif
static void sys_lwmutex_create(ppu_context* ctx)
{
    uint32_t lwm  = (uint32_t)ctx->gpr[3];
    uint32_t attr = (uint32_t)ctx->gpr[4];
    { static long long _n=0; _n++;
      if (getenv("LWM_COUNT") && (_n<=24 || (_n%50000)==0))
        fprintf(stderr, "[LWM] create #%lld lwm=0x%08X attr=0x%08X\n", _n, lwm, attr); }
    uint32_t protocol = attr ? vm_read32(attr + 0) : 0;
    vm_write32(lwm + 0x00, 0);          /* owner */
    vm_write32(lwm + 0x04, 0);          /* waiter */
    vm_write32(lwm + LWM_ATTR, protocol);
    vm_write32(lwm + LWM_RECUR, 0);     /* recursive_count */
    vm_write32(lwm + 0x10, 0);          /* sleep_queue */
    vm_write32(lwm + 0x14, 0);
#ifdef _WIN32
    /* A recreate at a reused address must not inherit a locked slot (e.g. the
     * previous holder exited while holding). Force the semaphore signaled;
     * over-release of an already-free sem fails harmlessly at max count 1. */
    { HANDLE s = lwm_sem(lwm); if (s) ReleaseSemaphore(s, 1, NULL); }
#endif
    ctx->gpr[3] = 0;
}
/* REAL mutual exclusion. The old no-op ("boot is single-threaded") corrupted
 * every lwmutex-protected structure once LBP spun up its worker/loader threads --
 * notably the dlmalloc mspace behind the game's big-allocator, whose tree then
 * fell apart and reported OOM on a tiny request with 100+ MB free.
 *
 * Backed by a binary SEMAPHORE, not a CRITICAL_SECTION: a CS may only be
 * released by its owning thread, but guest code passes lwmutex ownership
 * between threads (LBP's job system) and threads exit while holding -- one
 * cross-thread unlock silently failed and the still-owned CS parked the next
 * locker forever (the Network-node hang). A semaphore releases from any
 * thread. Recursion is handled explicitly via the guest owner/recur fields
 * we stamp (only the holder ever writes owner=self, so the re-lock check is
 * race-free). Keyed by guest address in an open-addressed table. */
#ifdef _WIN32
#define LWM_HASH 65536u
static struct LwmSlot { volatile long addr; HANDLE sem; } g_lwm[LWM_HASH];
static volatile long g_lwm_tab_lock = 0;
static HANDLE lwm_sem(uint32_t addr)
{
    if (!addr) return nullptr;
    uint32_t h = (addr * 2654435761u) & (LWM_HASH - 1);
    for (uint32_t i = 0; i < LWM_HASH; i++) {
        uint32_t idx = (h + i) & (LWM_HASH - 1);
        long cur = g_lwm[idx].addr;
        if ((uint32_t)cur == addr) return g_lwm[idx].sem;
        if (cur == 0) {
            while (_InterlockedExchange(&g_lwm_tab_lock, 1)) YieldProcessor();
            HANDLE r = nullptr;
            if (g_lwm[idx].addr == 0) {
                g_lwm[idx].sem = CreateSemaphoreA(NULL, 1, 1, NULL); /* free */
                g_lwm[idx].addr = (long)addr;   /* publish AFTER init (x86 TSO: readers see init) */
                r = g_lwm[idx].sem;
            } else if ((uint32_t)g_lwm[idx].addr == addr) {
                r = g_lwm[idx].sem;
            }
            _InterlockedExchange(&g_lwm_tab_lock, 0);
            if (r) return r;
            /* someone else claimed this slot for a different addr -> keep probing */
        }
    }
    return nullptr;   /* table full (raise LWM_HASH) */
}
#else
static void* lwm_sem(uint32_t) { return nullptr; }
#endif
/* Contention-probe window flag: 0 by default (prints stay bounded). A title's
 * diagnostic code may set it around a suspect wait to uncap the [LWM-BLOCK]
 * logging during that window only (park hunts: gate on state, not counts). */
volatile int g_nd_inpump = 0;
static void sys_lwmutex_lock(ppu_context* ctx)
{
    uint32_t lwm = (uint32_t)ctx->gpr[3];
    uint32_t self = LWM_SELF(ctx);
    /* lv2 ABI: r4 = timeout in microseconds, 0 = infinite. The real kernel
     * returns ETIMEDOUT (0x8001000B) when the wait expires; games rely on that
     * (e.g. LBP's resource loader locks with a 2s timeout in a retry loop so a
     * contended lock yields to other threads instead of hard-blocking). We had
     * been ignoring r4 and always waiting INFINITE, which defeats that pattern. */
    uint64_t timeout_us = ctx->gpr[4];
#ifdef _WIN32
    HANDLE s = lwm_sem(lwm);
    if (s) {
        /* Recursive re-lock by the current holder: bump the count, no wait.
         * Only the holder ever stamps owner=self, so this check is race-free. */
        if (vm_read32(lwm + LWM_OWNER) == self && vm_read32(lwm + LWM_RECUR) > 0) {
            vm_write32(lwm + LWM_RECUR, vm_read32(lwm + LWM_RECUR) + 1);
            ctx->gpr[3] = 0;
            return;
        }
        if (WaitForSingleObject(s, 0) != WAIT_OBJECT_0) {
            /* Contended: log who we're stuck behind (owner stamped at acquire),
             * then block. Bounded diagnostics for park hunts; uncapped while the
             * probe window is open (g_nd_inpump). */
            static long _bl = 0; long _b = ++_bl;
            if (g_nd_inpump || _b <= 40) fprintf(stderr, "[LWM-BLOCK] tid=%llu lwm=0x%08X owner=%u recur=%u tmo=%lluus\n",
                (unsigned long long)ctx->thread_id, lwm, vm_read32(lwm + LWM_OWNER), vm_read32(lwm + LWM_RECUR),
                (unsigned long long)timeout_us);
            DWORD ms = INFINITE;
            if (timeout_us) { uint64_t m = (timeout_us + 999) / 1000; ms = m > 0xFFFFFFFEull ? 0xFFFFFFFEu : (DWORD)m; }
            DWORD wr = WaitForSingleObject(s, ms);
            if (wr == WAIT_TIMEOUT) {           /* honor the timeout: ETIMEDOUT, no acquire */
                ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x8001000Bu;
                return;
            }
            if (g_nd_inpump || _b <= 40) fprintf(stderr, "[LWM-GOT] tid=%llu lwm=0x%08X\n", (unsigned long long)ctx->thread_id, lwm);
        }
    }
#endif
    vm_write32(lwm + LWM_OWNER, self);
    vm_write32(lwm + LWM_RECUR, 1);
    ctx->gpr[3] = 0;   // CELL_OK
}
static void sys_lwmutex_trylock(ppu_context* ctx)
{
    uint32_t lwm = (uint32_t)ctx->gpr[3];
    uint32_t self = LWM_SELF(ctx);
#ifdef _WIN32
    HANDLE s = lwm_sem(lwm);
    if (s) {
        if (vm_read32(lwm + LWM_OWNER) == self && vm_read32(lwm + LWM_RECUR) > 0) {
            vm_write32(lwm + LWM_RECUR, vm_read32(lwm + LWM_RECUR) + 1);
            ctx->gpr[3] = 0;
            return;
        }
        if (WaitForSingleObject(s, 0) != WAIT_OBJECT_0) { ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x8001000Bu; return; } // EBUSY
    }
#endif
    vm_write32(lwm + LWM_OWNER, self);
    vm_write32(lwm + LWM_RECUR, 1);
    ctx->gpr[3] = 0;
}
static void sys_lwmutex_unlock(ppu_context* ctx)
{
    uint32_t lwm = (uint32_t)ctx->gpr[3];
    uint32_t rc = vm_read32(lwm + LWM_RECUR);
    if (rc > 1) {                       /* recursive hold: count down, keep the lock */
        vm_write32(lwm + LWM_RECUR, rc - 1);
        ctx->gpr[3] = 0;
        return;
    }
    vm_write32(lwm + LWM_RECUR, 0);
    vm_write32(lwm + LWM_OWNER, 0);
#ifdef _WIN32
    HANDLE s = lwm_sem(lwm);
    /* Semaphore release works from ANY thread (unlike a CS) -- guest code
     * hands lwmutex ownership across threads. Over-release (unlock of a free
     * mutex) fails harmlessly at the max count of 1. */
    if (s) ReleaseSemaphore(s, 1, NULL);
#endif
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
/* Now that the lwmutex is REAL, a no-op wait that keeps holding it deadlocks the
 * signaler. Release the paired lwmutex, wait briefly, reacquire (poll-style: the
 * guest's while(!predicate) loop re-checks; signalers stay no-ops). Handles the
 * common single (non-recursive) hold. */
static void sys_lwcond_wait(ppu_context* ctx)
{
    uint32_t lwcond  = (uint32_t)ctx->gpr[3];
    uint32_t lwmutex = (uint32_t)vm_read64(lwcond + 0x00);
#ifdef _WIN32
    HANDLE s = lwm_sem(lwmutex);
    if (s) {
        uint32_t own = vm_read32(lwmutex + LWM_OWNER);
        uint32_t rc  = vm_read32(lwmutex + LWM_RECUR);
        vm_write32(lwmutex + LWM_RECUR, 0);
        vm_write32(lwmutex + LWM_OWNER, 0);
        ReleaseSemaphore(s, 1, NULL);
        Sleep(1);
        WaitForSingleObject(s, INFINITE);
        vm_write32(lwmutex + LWM_OWNER, own);
        vm_write32(lwmutex + LWM_RECUR, rc ? rc : 1);
    }
#endif
    ctx->gpr[3] = 0;
}

/* sys_ppu_thread_get_id(vm::ptr<u64> id) -> *id = calling thread's real id.
 * The old fixed "1" broke every am-I-the-designated-thread check in
 * multithreaded titles (LBP's job system routes work by thread identity, so
 * its queues were never serviced and network-init parked forever). */
static void sys_ppu_thread_get_id(ppu_context* ctx)
{
    uint32_t p = (uint32_t)ctx->gpr[3];
    /* Every registered thread has a nonzero id (main = 1 via
     * ppu_thread_register_main); 0 = unregistered scratch ctx, report the same
     * never-a-real-thread sentinel the lwmutex owner stamps use. */
    if (p) vm_write64(p, ctx->thread_id ? (uint64_t)ctx->thread_id : 0x7FFFFFFEull);
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
static void sys_lwmutex_destroy_counted(ppu_context* ctx)
{
    { static long long _n=0; _n++;
      if (getenv("LWM_COUNT") && (_n<=24 || (_n%50000)==0))
        fprintf(stderr, "[LWM] destroy #%lld lwm=0x%08X r4=0x%08X r5=0x%08X\n", _n,
                (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5]); }
    ctx->gpr[3] = 0;
}

/* Real preemptive thread create/exit live in the lv2 syscall layer
 * (syscalls/sys_ppu_thread.c) and spawn a host thread that runs the guest
 * entry through the recompiled code. The CRT also reaches them as
 * sysPrxForUser import NIDs (gen_hle_nids can't see them — they're not defined
 * in the sysPrxForUser lib), so bridge the NIDs to the same implementation.
 * Without this the CRT's thread/static-init runs through an uninitialised
 * object table and calls heap addresses as function pointers. */
extern "C" int64_t sys_ppu_thread_create(ppu_context* ctx);
extern "C" int64_t sys_ppu_thread_exit(ppu_context* ctx);
/* The ctx-aware dispatch (ppu_hle.cpp) does NOT propagate a handler return value
 * into gpr[3] -- each ctx handler must set gpr[3] itself. sys_ppu_thread_create
 * signals success by *returning* CELL_OK(0) (it never writes gpr[3]), so we must
 * store that return into gpr[3]. Otherwise gpr[3] is left as the incoming out-ptr
 * (&tid, nonzero) and the guest wrapper reads it as "create failed" -- e.g. LBP's
 * sub_52613C does `v6 = (ret==0); return v6 ? tid : 0`, so a nonzero ret makes it
 * hand back 0 and the caller's init (sub_C1484 / KdConvert) bails. */
static void hle_ppu_thread_create(ppu_context* ctx) { ctx->gpr[3] = sys_ppu_thread_create(ctx); }
static void hle_ppu_thread_exit(ppu_context* ctx)   { ctx->gpr[3] = sys_ppu_thread_exit(ctx); }

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

/* --- sys_net offline model ---------------------------------------------
 * LBP's net-services tick (sub_11A864) drains its UDP socket with
 * non-blocking recvfrom until it returns -1 (empty socket = EWOULDBLOCK on
 * real firmware). These NIDs were unresolved, and the unresolved default of
 * r3=0 reads as "received a 0-byte packet": the drain loop spins forever
 * while holding the net-manager lwmutex, wedging the whole boot at the
 * Network init node. Model the offline truth instead: no data, no sockets --
 * every receive/poll would-block. errno lives in a guest scratch cell since
 * _sys_net_errno_loc returns a POINTER the game dereferences. */
#define SYS_NET_EWOULDBLOCK_V 35
static uint32_t g_net_errno_ea = 0;
static void hle_net_errno_loc(ppu_context* ctx)
{
    if (!g_net_errno_ea) g_net_errno_ea = gcm_guest_alloc(4, 4);
    vm_write32(g_net_errno_ea, SYS_NET_EWOULDBLOCK_V);
    ctx->gpr[3] = g_net_errno_ea;
}
static void hle_net_wouldblock(ppu_context* ctx)
{
    if (g_net_errno_ea) vm_write32(g_net_errno_ea, SYS_NET_EWOULDBLOCK_V);
    ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)-1;
}
static void hle_net_zero(ppu_context* ctx) { ctx->gpr[3] = 0; }
/* select/poll: nothing is ever ready offline -- but a real select BLOCKS for
 * the caller's timeout before saying so. Returning instantly turned LBP's
 * 30Hz net pump (sub_3A5548: select(1, r/w/e sets, {0s, 33333us})) into a
 * 100%-CPU busy-spin that also dominated the guest-PC profiler, masquerading
 * as a boot hang. Honor the timeout and clear the fd sets (1024-bit each). */
static void hle_net_select(ppu_context* ctx)
{
    uint32_t rd = (uint32_t)ctx->gpr[4], wr = (uint32_t)ctx->gpr[5];
    uint32_t ex = (uint32_t)ctx->gpr[6], tv = (uint32_t)ctx->gpr[7];
    uint64_t us = 10000;              /* NULL timeout = block forever: tick at 10ms instead */
    if (tv) us = vm_read64(tv) * 1000000ull + vm_read64(tv + 8);   /* {s64 sec, s64 usec} BE */
    if (us > 100000) us = 100000;     /* cap so shutdown stays responsive */
    if (us) Sleep((DWORD)((us + 999) / 1000));
    const uint32_t sets[3] = { rd, wr, ex };
    for (int s = 0; s < 3; s++)
        if (sets[s]) for (uint32_t i = 0; i < 128; i += 4) vm_write32(sets[s] + i, 0);
    ctx->gpr[3] = 0;                  /* 0 fds ready */
}
static void hle_net_poll(ppu_context* ctx)
{
    uint32_t fds  = (uint32_t)ctx->gpr[3];
    uint32_t nfds = (uint32_t)ctx->gpr[4];
    int32_t  ms   = (int32_t)(uint32_t)ctx->gpr[5];
    if (ms < 0 || ms > 100) ms = (ms < 0) ? 10 : 100;   /* -1 = infinite: tick at 10ms */
    if (ms) Sleep((DWORD)ms);
    for (uint32_t i = 0; i < nfds && i < 64; i++)       /* pollfd = {s32 fd, s16 ev, s16 rev} */
        if (fds) vm_write32(fds + i * 8 + 4, vm_read32(fds + i * 8 + 4) & 0xFFFF0000u);
    ctx->gpr[3] = 0;                  /* 0 fds ready */
}
/* Distinct small fds: the unresolved default handed EVERY socket() call fd 0,
 * making all sockets alias one id in the game's tables. */
static void hle_net_socket(ppu_context* ctx) { static uint32_t s_fd = 3; ctx->gpr[3] = s_fd++; }
/* sendto: report the full length as sent (packets vanish into the void, matching
 * the RPCS3-offline oracle where broadcasts go out and nothing answers). */
static void hle_net_sendto(ppu_context* ctx) { ctx->gpr[3] = (uint32_t)ctx->gpr[5]; }

extern "C" void ppu_sysprx_register(void)
{
    ps3_hle_register_ctx(0x15BAE46Bu, "_cellGcmInitBody", hle_cellGcmInitBody);

    /* sys_net offline model (NIDs from PSL1GHT libnet exports). Covers every
     * sys_net NID LBP imports so none fall to the unresolved-NID default. */
    ps3_hle_register_ctx(0x6005CDE1u, "_sys_net_errno_loc",     hle_net_errno_loc);
    ps3_hle_register_ctx(0x1F953B9Fu, "sys_net_bnet_recvfrom",  hle_net_wouldblock);
    ps3_hle_register_ctx(0xFBA04F37u, "sys_net_bnet_recv",      hle_net_wouldblock);
    ps3_hle_register_ctx(0xC9D09C34u, "sys_net_bnet_recvmsg",   hle_net_wouldblock);
    ps3_hle_register_ctx(0x051EE3EEu, "sys_net_bnet_poll",      hle_net_poll);
    ps3_hle_register_ctx(0x3F09E20Au, "sys_net_bnet_select",    hle_net_select);
    ps3_hle_register_ctx(0x139A9E9Bu, "netInitializeNetworkEx", hle_net_zero);   /* lib init ok */
    ps3_hle_register_ctx(0x9C056962u, "netSocket",              hle_net_socket);
    ps3_hle_register_ctx(0xB0A59804u, "netBind",                hle_net_zero);
    ps3_hle_register_ctx(0x88F03575u, "netSetSockOpt",          hle_net_zero);
    ps3_hle_register_ctx(0x9647570Bu, "netSendTo",              hle_net_sendto);
    ps3_hle_register_ctx(0x6DB6E8CDu, "netClose",               hle_net_zero);
    ps3_hle_register_ctx(0x71F4C717u, "netGetHostByName",       hle_net_zero);   /* NULL: DNS down */
    ps3_hle_register_ctx(0xB68D5625u, "netFinalizeNetwork",     hle_net_zero);
    ps3_hle_register_ctx(0xFDB8F926u, "netFreethreadContext",   hle_net_zero);
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
    ps3_hle_register_ctx(ps3_compute_nid("sys_lwmutex_destroy"), "sys_lwmutex_destroy", sys_lwmutex_destroy_counted);
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
