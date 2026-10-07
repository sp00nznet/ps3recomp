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
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <atomic>
#include <string>
/* win32_compat.h is <windows.h> on Windows (CRITICAL_SECTION for the real
 * lwmutex exclusion) and the POSIX shims elsewhere -- Sleep, DWORD, QPC. */
#include "../platform/win32_compat.h"
#include "../syscalls/lv2_spu_image.h"
#include "../memory/vm.h"   /* VM_HLE_INJECT_BASE -- not platform-specific */

extern "C" uint8_t* vm_base;
extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
extern "C" uint32_t vm_read32(uint64_t a);
extern "C" uint64_t vm_read64(uint64_t a);
extern "C" void     vm_write32(uint64_t a, uint32_t v);
extern "C" void     vm_write64(uint64_t a, uint64_t v);
extern "C" uint64_t ppu_guest_call(uint32_t opd, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                   uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);

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

/* Real lwmutex/lwcond on every host. They were _WIN32-only: on POSIX lock and
 * unlock merely stamped the owner word, so there was no mutual exclusion at
 * all, and lwcond_wait returned at once. The semaphores come from the
 * win32_compat shim (pthread-backed) off Windows. */
#define PS3_LWM_REAL 1

#if PS3_LWM_REAL
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
#if PS3_LWM_REAL
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
#if PS3_LWM_REAL
#define LWM_HASH 65536u
static struct LwmSlot { volatile long addr; HANDLE sem;
    volatile long holder; volatile long long acq_us; volatile long long acq_fences;
    volatile unsigned long long acq_cpu_us; } g_lwm[LWM_HASH];
extern "C" { extern volatile long long g_gcm_ref_pub_count;    /* cellGcmSys.c */
             unsigned long long ppu_thread_cpu_us(unsigned tid);   /* sys_ppu_thread.c */
             unsigned ppu_thread_prof_pc(unsigned tid); }
static volatile long g_lwm_tab_lock = 0;
/* PS3_LWMUTEX_TRACE=1: reconstruct the lwmutex lock-convoy that stalls LBP's loader.
 * Records, per mutex, the acquiring tid + a QPC microsecond timestamp; on a
 * contended block it logs who holds it and for how long; on unlock it flags a
 * long hold. The leaf holder (the one blocked on a non-lwmutex wait) is the
 * convoy root. Default OFF. */
static int lwm_trace(void){ static std::atomic<int> v=-1; if(v<0){const char*e=getenv("PS3_LWMUTEX_TRACE"); v=e?1:0;} return v; }
static long long lwm_now_us(void){
#ifdef _WIN32
    static LARGE_INTEGER freq={0}; if(!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (long long)(c.QuadPart*1000000ll/freq.QuadPart);
#else
    return 0;
#endif
}
/* Find an EXISTING slot (no create) for hold-tracking. */
static struct LwmSlot* lwm_find(uint32_t addr){
    if(!addr) return nullptr;
    uint32_t h=(addr*2654435761u)&(LWM_HASH-1);
    for(uint32_t i=0;i<LWM_HASH;i++){ uint32_t idx=(h+i)&(LWM_HASH-1);
        long cur=g_lwm[idx].addr;
        if((uint32_t)cur==addr) return &g_lwm[idx];
        if(cur==0) return nullptr; }
    return nullptr;
}
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
#if PS3_LWM_REAL
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
            if (lwm_trace()) { struct LwmSlot* sl = lwm_find(lwm);
                long h = sl ? sl->holder : 0; long long held = (sl && h) ? (lwm_now_us() - sl->acq_us) : 0;
                fprintf(stderr, "[LWM-CONVOY] tid=%llu BLOCKs lwm=0x%08X -> held-by-tid=%ld for %lldus (guest-owner=%u)\n",
                    (unsigned long long)ctx->thread_id, lwm, h, held, vm_read32(lwm + LWM_OWNER)); fflush(stderr); }
            long long _blk_start = lwm_trace() ? lwm_now_us() : 0;
            DWORD ms = INFINITE;
            if (timeout_us) { uint64_t m = (timeout_us + 999) / 1000; ms = m > 0xFFFFFFFEull ? 0xFFFFFFFEu : (DWORD)m; }
            DWORD wr = WaitForSingleObject(s, ms);
            if (wr == WAIT_TIMEOUT) {           /* honor the timeout: ETIMEDOUT, no acquire */
                if (lwm_trace()) fprintf(stderr, "[LWM-CONVOY] tid=%llu TIMED-OUT on lwm=0x%08X after %lldus (ETIMEDOUT, retry)\n",
                    (unsigned long long)ctx->thread_id, lwm, lwm_now_us() - _blk_start);
                ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x8001000Bu;
                return;
            }
            if (lwm_trace()) fprintf(stderr, "[LWM-CONVOY] tid=%llu ACQUIRED lwm=0x%08X after waiting %lldus\n",
                (unsigned long long)ctx->thread_id, lwm, lwm_now_us() - _blk_start);
            if (g_nd_inpump || _b <= 40) fprintf(stderr, "[LWM-GOT] tid=%llu lwm=0x%08X\n", (unsigned long long)ctx->thread_id, lwm);
        }
    }
    if (lwm_trace()) { struct LwmSlot* sl = lwm_find(lwm); if (sl) { sl->holder = (long)self; sl->acq_us = lwm_now_us(); sl->acq_fences = g_gcm_ref_pub_count; sl->acq_cpu_us = ppu_thread_cpu_us(self); } }
#endif
    vm_write32(lwm + LWM_OWNER, self);
    vm_write32(lwm + LWM_RECUR, 1);
    ctx->gpr[3] = 0;   // CELL_OK
}
static void sys_lwmutex_trylock(ppu_context* ctx)
{
    uint32_t lwm = (uint32_t)ctx->gpr[3];
    uint32_t self = LWM_SELF(ctx);
#if PS3_LWM_REAL
    HANDLE s = lwm_sem(lwm);
    if (s) {
        if (vm_read32(lwm + LWM_OWNER) == self && vm_read32(lwm + LWM_RECUR) > 0) {
            vm_write32(lwm + LWM_RECUR, vm_read32(lwm + LWM_RECUR) + 1);
            ctx->gpr[3] = 0;
            return;
        }
        if (WaitForSingleObject(s, 0) != WAIT_OBJECT_0) { ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x8001000Bu; return; } // EBUSY
    }
    if (lwm_trace()) { struct LwmSlot* sl = lwm_find(lwm); if (sl) { sl->holder = (long)self; sl->acq_us = lwm_now_us(); sl->acq_fences = g_gcm_ref_pub_count; sl->acq_cpu_us = ppu_thread_cpu_us(self); } }
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
#if PS3_LWM_REAL
    if (lwm_trace()) { struct LwmSlot* sl = lwm_find(lwm);
        if (sl && sl->holder) { long long held = lwm_now_us() - sl->acq_us;
            if (held > 100000) {
                long long fences = g_gcm_ref_pub_count - sl->acq_fences;
                unsigned long long cpu_now = ppu_thread_cpu_us((unsigned)sl->holder);
                long long cpu_delta = (cpu_now && sl->acq_cpu_us) ? (long long)(cpu_now - sl->acq_cpu_us) : -1;
                fprintf(stderr, "[LWM-CONVOY] tid=%ld RELEASES lwm=0x%08X after holding %lldus (LONG HOLD) fences=%lld cpu=%lldus (%.0f%% cpu-bound) last-hle-from=0x%08X\n",
                    sl->holder, lwm, held, fences, cpu_delta,
                    cpu_delta >= 0 ? 100.0 * (double)cpu_delta / (double)held : -1.0,
                    ppu_thread_prof_pc((unsigned)sl->holder));
                /* Name the critical section: the unlocker IS the holder, so its
                 * guest stack right now is the exit of the long-held region.
                 * Back-chain LR slots are 0 under the DRAIN/fragment model, so
                 * scan the stack for words in the lifted code range instead
                 * (saved return addresses; a lifted func name IS its guest
                 * addr). Same idiom as [exit-chain] in ppu_hle.cpp. */
                uint32_t sp = (uint32_t)ctx->gpr[1];
                char b[1600]; int p = snprintf(b, sizeof b, "[LWM-CONVOY]   holder-bt sp=0x%08X codeptrs:", sp);
                uint32_t prev = 0; int found = 0;
                for (uint32_t a = sp; a < sp + 0x3000 && found < 48; a += 4) {
                    uint32_t v = vm_read32(a);
                    if (v >= 0x00010000u && v < 0x00900000u && v != prev) {
                        p += snprintf(b + p, sizeof(b) - p, " %08X", v); prev = v; found++; }
                }
                fprintf(stderr, "%s\n", b); fflush(stderr);
            }
            sl->holder = 0; } }
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
 * handshakes, not long-term blocking. sys_lwcond_t is 8 bytes (RPCS3
 * sys_lwcond.h): +0x00 lwmutex EA (be32), +0x04 lwcond_queue id (be32). A be64
 * store at +0 left the pointer word 0 -- FIOS (func_00442AB8 in inFamous)
 * reads it as "not created" and reports "wait for invalid cond" -- and the id
 * store at +0x08 overran the struct into the title's next field. */
static void sys_lwcond_create(ppu_context* ctx)
{
    static uint32_t s_lwcond_id = 0x4C000000u;
    uint32_t lwcond  = (uint32_t)ctx->gpr[3];
    uint32_t lwmutex = (uint32_t)ctx->gpr[4];
    vm_write32(lwcond + 0x00, lwmutex);
    vm_write32(lwcond + 0x04, ++s_lwcond_id);
    ctx->gpr[3] = 0;
}
static void sys_lwcond_destroy(ppu_context* ctx)    { ctx->gpr[3] = 0; }

/* Real lwcond semantics. The old wait released the lwmutex, slept 1 ms and
 * returned success with every signal a no-op, on the theory that callers
 * re-check a predicate in a loop. inFamous's movie player does not: it posts a
 * frame-buffer request, waits once, and takes the first wakeup as "serviced"
 * -- so it built Bink's frame planes from a NULL buffer and the decoder SPU
 * DMA'd every video frame over the game's .text (jump tables included).
 *
 * Per lwcond EA: waiters registered and signal tokens granted. A waiter
 * registers BEFORE it releases the lwmutex, and signalers hold that lwmutex,
 * so no signal is lost; signal grants one token (if anyone waits), signal_all
 * one per waiter. PS3_LWCOND_POLL=1 restores the old poll behaviour. */
struct LwcondWaiter { uint32_t ea; bool released; LwcondWaiter* next; };
static std::mutex s_lwc_mu;
static std::condition_variable s_lwc_cv;
static LwcondWaiter* s_lwc_head;   /* FIFO of registered waiters, all lwconds */

static void lwc_enqueue(LwcondWaiter* w)   /* s_lwc_mu held */
{
    LwcondWaiter** p = &s_lwc_head;
    while (*p) p = &(*p)->next;
    w->next = nullptr;
    *p = w;
}

static void lwc_unlink(LwcondWaiter* w)    /* s_lwc_mu held */
{
    for (LwcondWaiter** p = &s_lwc_head; *p; p = &(*p)->next)
        if (*p == w) { *p = w->next; return; }
}

static bool lwc_poll_mode()
{
    static std::atomic<int> m = -1;
    if (m < 0) m = getenv("PS3_LWCOND_POLL") ? 1 : 0;
    return m == 1;
}

static int lwc_log()
{
    static std::atomic<int> n = -1;
    if (n < 0) { const char* e = getenv("PS3_LWCOND_LOG"); n = e ? atoi(e) : 0; }
    return n;
}
static std::atomic<int> s_lwc_logged{0};

/* signal releases the oldest waiter registered on this lwcond, signal_all
 * every one registered now. Released waiters leave the queue at once, so a
 * thread that wakes and waits again can never take another waiter's wakeup. */
static void lwc_signal(uint32_t ea, bool all, uint32_t tid = 0)
{
    std::lock_guard<std::mutex> lk(s_lwc_mu);
    unsigned n = 0;
    for (LwcondWaiter** p = &s_lwc_head; *p; ) {
        LwcondWaiter* w = *p;
        if (w->ea == ea) {
            w->released = true;
            *p = w->next;
            n++;
            if (!all) break;
        } else {
            p = &w->next;
        }
    }
    if (lwc_log() && s_lwc_logged++ < lwc_log())
        fprintf(stderr, "[lwcond] signal%s ea=0x%08X woke=%u tid=%u lwm_owner=0x%X\n", all ? "_all" : "", ea,
                n, tid, vm_read32(vm_read32(ea) + LWM_OWNER));
    if (n) s_lwc_cv.notify_all();
}

static void sys_lwcond_signal(ppu_context* ctx)     { lwc_signal((uint32_t)ctx->gpr[3], false, ctx->thread_id); ctx->gpr[3] = 0; }
static void sys_lwcond_signal_all(ppu_context* ctx) { lwc_signal((uint32_t)ctx->gpr[3], true, ctx->thread_id);  ctx->gpr[3] = 0; }
static void sys_lwcond_signal_to(ppu_context* ctx)  { lwc_signal((uint32_t)ctx->gpr[3], false, ctx->thread_id); ctx->gpr[3] = 0; }

static void sys_lwcond_wait(ppu_context* ctx)
{
    uint32_t lwcond  = (uint32_t)ctx->gpr[3];
    uint64_t timeout = ctx->gpr[4];                 /* microseconds, 0 = forever */
    uint32_t lwmutex = vm_read32(lwcond + 0x00);
    int32_t rc_out = 0;
#if PS3_LWM_REAL
    HANDLE s = lwm_sem(lwmutex);
    if (s) {
        const bool poll = lwc_poll_mode();
        if (lwc_log() && s_lwc_logged++ < lwc_log())
            fprintf(stderr, "[lwcond] wait ea=0x%08X lwm=0x%08X timeout=%llu tid=%u\n", lwcond, lwmutex,
                    (unsigned long long)timeout, (unsigned)ctx->thread_id);
        LwcondWaiter me{lwcond, false, nullptr};
        if (!poll) {
            std::lock_guard<std::mutex> lk(s_lwc_mu);
            lwc_enqueue(&me);
        }
        uint32_t own = vm_read32(lwmutex + LWM_OWNER);
        uint32_t rc  = vm_read32(lwmutex + LWM_RECUR);
        vm_write32(lwmutex + LWM_RECUR, 0);
        vm_write32(lwmutex + LWM_OWNER, 0);
        ReleaseSemaphore(s, 1, NULL);
        if (poll) {
            Sleep(1);
        } else {
            std::unique_lock<std::mutex> lk(s_lwc_mu);
            auto ready = [&] { return me.released; };
            if (timeout) {
                if (!s_lwc_cv.wait_for(lk, std::chrono::microseconds(timeout), ready)) {
                    lwc_unlink(&me);
                    rc_out = (int32_t)0x8001000B;   /* CELL_ETIMEDOUT */
                    if (getenv("PS3_LWCOND_TOLOG")) { static std::atomic<int> n{0}; if (n++ < 400)
                        fprintf(stderr, "[lwcond] TIMEOUT ea=0x%08X tid=%u after %lluus lr=0x%08X\n", lwcond,
                                (unsigned)ctx->thread_id, (unsigned long long)timeout, (uint32_t)ctx->lr); }
                }
            } else {
                s_lwc_cv.wait(lk, ready);
            }
        }
        WaitForSingleObject(s, INFINITE);
        vm_write32(lwmutex + LWM_OWNER, own);
        vm_write32(lwmutex + LWM_RECUR, rc ? rc : 1);
    }
#endif
    ctx->gpr[3] = (uint64_t)(int64_t)rc_out;
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
    if (getenv("PS3_MEMTRACE"))
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
    if (getenv("PS3_MEMTRACE"))
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
extern "C" void _cellSpursWorkloadAttributeInitialize_ctx(uint64_t* gpr);
static void hle_spurs_wkattr_init(ppu_context* ctx) { _cellSpursWorkloadAttributeInitialize_ctx(ctx->gpr); }

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
 * indirect dispatcher routes here. r3 = guest context EA.
 *
 * Derived from VM_HLE_INJECT_BASE, the same base libs/video/cellGcmSys.c
 * builds the OPD from. This was a second hardcoded copy of the address, and
 * moving the injected block out of guest-allocatable memory updated only one
 * of them: the OPD then pointed at an EA with no registered function, the
 * garbage-vcall guard no-opped the callback, the FIFO never recycled and the
 * title stopped flipping. One definition, one place. */
#define GCM_FIFO_CALLBACK_SENTINEL_EA (VM_HLE_INJECT_BASE + 0x2F00u)
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
extern "C" void ps3_net_host_register(unsigned int (*guest_alloc)(unsigned int, unsigned int));
extern "C" void np_score_register_ctx(void);   /* libs/network/sceNpScore.c */
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

/* ---- liblv2 (sysPrxForUser): SPU images, formatted output, SPU printf -----
 *
 * The functions firmware libsre imports from liblv2 that live above the
 * kernel. Each one's behaviour is what Sony's liblv2 does when RPCS3 runs it
 * (LLE), as tests/conformance/spurs/t_sysprx records it.
 *
 * The lv2 side of SPU images (kernel image objects, ELF parsing, loading a
 * segment list into a local store) is runtime/syscalls/lv2_spu_image.c. */

/* Kept defined for the SPU_DSP_IMAGE_EA recovery in spu_dma.h, which reads
 * them. Nothing sets them any more: they recorded the last image the old
 * import parsed, a per-title workaround for a DMA source address read as 0. */
extern "C" uint32_t g_spu_image_src_ea = 0, g_spu_image_ls_start = 0,
                    g_spu_image_span = 0;

extern "C" void  spu_raw_note_image(uint32_t src_ea, uint32_t entry);  /* runtime/spu/spu_raw.c */
extern "C" void* _sys_malloc(uint32_t size);                          /* libs/system/sysPrxForUser.c */
extern "C" int32_t _sys_free(void* ptr);

enum : uint32_t {
    LV2_EINVAL  = 0x80010002u,
    LV2_ESRCH   = 0x80010005u,
    LV2_ESTAT   = 0x8001000Fu,
};

/* sys_spu_image_import(sys_spu_image_t* img, const void* src, u32 type)
 *
 *   type 1 (DIRECT): liblv2 parses the ELF itself. The descriptor becomes
 *     {0 (USER), entry, segs, nsegs}, segs pointing at a list it allocates:
 *     a COPY per loadable segment (addr = the segment's bytes inside the
 *     ELF), followed by a FILL of pattern 0 for a segment's zero-filled tail.
 *   type 0 (PROTECT): the kernel takes a copy (_sys_spu_image_import, syscall
 *     157): {1 (KERNEL), kernel image id, 0, 0}.
 *   Any other type: EINVAL. A source that is not an SPU ELF (an SCE-wrapped
 *   one included): ENOEXEC, either type. */
static void hle_sys_spu_image_import(ppu_context* ctx)
{
    const uint32_t img = (uint32_t)ctx->gpr[3], src = (uint32_t)ctx->gpr[4];
    const uint32_t type = (uint32_t)ctx->gpr[5];
    if (type > 1) { ctx->gpr[3] = LV2_EINVAL; return; }

    lv2_spu_seg segs[LV2_SPU_MAX_SEGS];
    uint32_t entry = 0;
    const int32_t n = lv2_spu_elf_segments(src, 0, segs, LV2_SPU_MAX_SEGS, &entry);
    if (n < 0) { ctx->gpr[3] = (uint32_t)n; return; }

    if (type == 0) {
        ctx->gpr[3] = (uint32_t)lv2_spu_image_import(img, src, lv2_spu_elf_span(src), 0);
        return;
    }
    const uint32_t list = (uint32_t)(uintptr_t)_sys_malloc((uint32_t)n * 0x18u);
    for (int32_t i = 0; i < n; i++) {
        const uint32_t s = list + (uint32_t)i * 0x18u;
        vm_write32(s + 0x00, segs[i].type);
        vm_write32(s + 0x04, segs[i].ls);
        vm_write32(s + 0x08, segs[i].size);
        vm_write32(s + 0x0C, 0);
        vm_write32(s + 0x10, segs[i].addr);
        vm_write32(s + 0x14, 0);
    }
    vm_write32(img + 0x00, 0);
    vm_write32(img + 0x04, entry);
    vm_write32(img + 0x08, list);
    vm_write32(img + 0x0C, (uint32_t)n);
    /* Which ELF the descriptor came from: the lifted-SPU registry is keyed by
     * the ELF's bytes, and a raw SPU (started by an MMIO store, no syscall)
     * can only be matched to its lifted entry here. */
    lv2_spu_image_note_source(img, src);
    spu_raw_note_image(src, entry);
    ctx->gpr[3] = 0;
}

/* sys_spu_image_close(sys_spu_image_t* img): USER frees the segment list
 * liblv2 allocated; KERNEL closes the kernel object (ESRCH once it is gone).
 * The descriptor itself is left as it was. */
static void hle_sys_spu_image_close(ppu_context* ctx)
{
    const uint32_t img = (uint32_t)ctx->gpr[3];
    if (vm_read32(img) == 0) {
        _sys_free((void*)(uintptr_t)vm_read32(img + 8));
        ctx->gpr[3] = 0;
        return;
    }
    ctx->gpr[3] = (uint32_t)lv2_spu_image_close(img);
}

/* Formatted output. A guest variadic call puts argument k in r3+k for k < 8
 * and the rest in the caller's parameter save area at SP + 48 + 8k (64-bit
 * ELF ABI; floating-point arguments occupy the same slots). A guest va_list
 * is a pointer to an array of those 64-bit slots. */
struct lv2_varargs {
    ppu_context* ctx;
    int          k;    /* next argument index (register form) */
    uint32_t     va;   /* nonzero: read from this va_list instead */
    uint64_t next()
    {
        if (va) { uint64_t v = vm_read64(va); va += 8; return v; }
        const int i = k++;
        return i < 8 ? ctx->gpr[3 + i] : vm_read64((uint32_t)ctx->gpr[1] + 48u + 8u * (uint32_t)i);
    }
};

/* liblv2's formatter, conversion by conversion:
 *   flags - + space # 0, width and precision (either may be *), then:
 *   h, hh and l are accepted and change nothing (an int and a long are both
 *   32 bits); ll makes an integer 64-bit;
 *   d i u o x X c s as C;
 *   p prints 0x and lower-case hex, padded to the width;
 *   n stores the count so far (u32) through its argument, unless it is NULL;
 *   anything else prints that character and consumes no argument -- the
 *   floating-point conversions included: liblv2 has none. */
static std::string lv2_format(uint32_t fmt, lv2_varargs& a)
{
    std::string out;
    if (!fmt) return out;
    const char* f = (const char*)(vm_base + fmt);
    char tmp[512];
    while (*f) {
        if (*f != '%') { out += *f++; continue; }
        f++;
        std::string spec = "%";
        while (*f && strchr("-+ #0", *f)) spec += *f++;
        if (*f == '*') { spec += std::to_string((int32_t)a.next()); f++; }
        else while (*f >= '0' && *f <= '9') spec += *f++;
        if (*f == '.') {
            spec += *f++;
            if (*f == '*') { spec += std::to_string((int32_t)a.next()); f++; }
            else while (*f >= '0' && *f <= '9') spec += *f++;
        }
        int longs = 0;
        for (;;) {
            if (*f == 'h') f++;
            else if (*f == 'l') { longs++; f++; }
            else break;
        }
        const char c = *f;
        if (!c) break;
        f++;
        int n = 0;
        switch (c) {
        case '%': out += '%'; continue;
        case 'd': case 'i': {
            const uint64_t v = a.next();
            n = snprintf(tmp, sizeof tmp, (spec + "lld").c_str(),
                         longs >= 2 ? (long long)v : (long long)(int32_t)v);
            break;
        }
        case 'u': case 'o': case 'x': case 'X': {
            const uint64_t v = a.next();
            n = snprintf(tmp, sizeof tmp, (spec + "ll" + c).c_str(),
                         (unsigned long long)(longs >= 2 ? v : (uint32_t)v));
            break;
        }
        case 'c':
            n = snprintf(tmp, sizeof tmp, (spec + 'c').c_str(), (int)(uint8_t)a.next());
            break;
        case 's': {
            const uint32_t s = (uint32_t)a.next();
            n = snprintf(tmp, sizeof tmp, (spec + 's').c_str(),
                         s ? (const char*)(vm_base + s) : "(null)");
            break;
        }
        case 'p': {
            char hex[16];
            snprintf(hex, sizeof hex, "0x%x", (uint32_t)a.next());
            n = snprintf(tmp, sizeof tmp, (spec + 's').c_str(), hex);
            break;
        }
        case 'n': {
            const uint32_t p = (uint32_t)a.next();
            if (p) vm_write32(p, (uint32_t)out.size());
            continue;
        }
        default:
            out += c;
            continue;
        }
        if (n > 0) out.append(tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
    }
    return out;
}

/* Store into a guest buffer of `size` bytes the way liblv2 does: a string that
 * fits is written with its NUL; one that does not gets its first size - 1
 * characters and no NUL; size 0 writes nothing. Returns the full length. */
static uint32_t lv2_store(uint32_t buf, uint32_t size, const std::string& s)
{
    if (size) {
        if (s.size() < size) {
            memcpy(vm_base + buf, s.data(), s.size());
            vm_base[buf + s.size()] = 0;
        } else {
            memcpy(vm_base + buf, s.data(), size - 1);
        }
    }
    return (uint32_t)s.size();
}

static void lv2_print(const std::string& s)
{
    fwrite(s.data(), 1, s.size(), stderr);   /* where sys_tty_write goes */
    fflush(stderr);
}

static void hle_sys_printf(ppu_context* ctx)
{
    lv2_varargs a{ctx, 1, 0};
    const std::string s = lv2_format((uint32_t)ctx->gpr[3], a);
    lv2_print(s);
    ctx->gpr[3] = (uint32_t)s.size();
}

static void hle_sys_vprintf(ppu_context* ctx)
{
    lv2_varargs a{ctx, 0, (uint32_t)ctx->gpr[4]};
    const std::string s = lv2_format((uint32_t)ctx->gpr[3], a);
    lv2_print(s);
    ctx->gpr[3] = (uint32_t)s.size();
}

static void hle_sys_sprintf(ppu_context* ctx)
{
    lv2_varargs a{ctx, 2, 0};
    const std::string s = lv2_format((uint32_t)ctx->gpr[4], a);
    ctx->gpr[3] = lv2_store((uint32_t)ctx->gpr[3], (uint32_t)s.size() + 1, s);
}

static void hle_sys_vsprintf(ppu_context* ctx)
{
    lv2_varargs a{ctx, 0, (uint32_t)ctx->gpr[5]};
    const std::string s = lv2_format((uint32_t)ctx->gpr[4], a);
    ctx->gpr[3] = lv2_store((uint32_t)ctx->gpr[3], (uint32_t)s.size() + 1, s);
}

static void hle_sys_snprintf(ppu_context* ctx)
{
    lv2_varargs a{ctx, 3, 0};
    const std::string s = lv2_format((uint32_t)ctx->gpr[5], a);
    ctx->gpr[3] = lv2_store((uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4], s);
}

static void hle_sys_vsnprintf(ppu_context* ctx)
{
    lv2_varargs a{ctx, 0, (uint32_t)ctx->gpr[6]};
    const std::string s = lv2_format((uint32_t)ctx->gpr[5], a);
    ctx->gpr[3] = lv2_store((uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4], s);
}

/* SPU printf: _sys_spu_printf_initialize registers four guest callbacks
 * (attach group, detach group, attach thread, detach thread); each
 * attach/detach call invokes the matching one with the group or thread id and
 * returns what it returns. Before initialize, or after finalize: ESTAT. */
static std::atomic<uint32_t> s_spu_printf_cb[4];

static void hle_sys_spu_printf_initialize(ppu_context* ctx)
{
    for (int i = 0; i < 4; i++) s_spu_printf_cb[i] = (uint32_t)ctx->gpr[3 + i];
    ctx->gpr[3] = 0;
}

static void hle_sys_spu_printf_finalize(ppu_context* ctx)
{
    for (int i = 0; i < 4; i++) s_spu_printf_cb[i] = 0;
    ctx->gpr[3] = 0;
}

static void lv2_spu_printf_call(ppu_context* ctx, int which)
{
    const uint32_t cb = s_spu_printf_cb[which];
    if (!cb) { ctx->gpr[3] = LV2_ESTAT; return; }
    ctx->gpr[3] = (uint32_t)ppu_guest_call(cb, (uint32_t)ctx->gpr[3], 0, 0, 0, 0, 0, 0, 0);
}

static void hle_sys_spu_printf_attach_group(ppu_context* ctx)  { lv2_spu_printf_call(ctx, 0); }
static void hle_sys_spu_printf_detach_group(ppu_context* ctx)  { lv2_spu_printf_call(ctx, 1); }
static void hle_sys_spu_printf_attach_thread(ppu_context* ctx) { lv2_spu_printf_call(ctx, 2); }
static void hle_sys_spu_printf_detach_thread(ppu_context* ctx) { lv2_spu_printf_call(ctx, 3); }

/* ---- sys_spinlock_* (sysPrxForUser) -------------------------------------
 *
 * A sys_spinlock_t is one 32-bit word in guest memory and nothing else -- the
 * real firmware spins on it with lwarx/stwcx. The guest only ever touches that
 * word through these four calls, so a host test-and-set on the same address is
 * exactly equivalent and stays correct across the title's own threads.
 *
 * Stored big-endian, so a guest that peeks at the word still reads 1.
 *
 * ABI: initialize / lock / unlock return void; trylock returns CELL_OK or
 * EBUSY. Found via Virtua Fighter 5, which locks with trylock in a retry loop
 * and was the first title to import the family -- the three NIDs were unnamed
 * in the database until compute_nid() was brute-forced over the sysPrxForUser
 * export list.
 */
#define SPINLOCK_BE1  0x01000000u          /* be32(1) */

static inline volatile long* spin_word(uint32_t ea)
{
    return (volatile long*)(vm_base + ea);
}

/* Test-and-set, not compare-and-swap: writing "locked" over an already-locked
 * word is idempotent, so a plain atomic exchange is enough -- and _Interlocked-
 * Exchange is the one RMW the POSIX shim in win32_compat.h already provides. */
static inline int spin_try(uint32_t ea)
{
    return _InterlockedExchange(spin_word(ea), (long)SPINLOCK_BE1) == 0;
}

static void sys_spinlock_initialize(ppu_context* ctx)
{
    uint32_t ea = (uint32_t)ctx->gpr[3];
    if (ea) _InterlockedExchange(spin_word(ea), 0);
}

/* Who currently holds each locked word, so a spin that never ends can name the
 * holder instead of just burning a core in silence.
 *
 * A spinlock has no timeout and no failure return, so a lock that is never
 * released is invisible: the thread sits in Sleep(0) at 100% of a core, a
 * sampling profiler attributes every sample to ntdll, and NOTHING says which
 * address or which holder. Guitar Hero III deadlocks exactly this way during
 * its heap bring-up and presented as "all threads idle in ntdll" while pinning
 * a core -- two facts that only make sense together once the lock is named.
 *
 * Small fixed table, linear scan: contention here is rare by construction (a
 * guest spinlock section is a few instructions), and a miss just means the
 * report says "unknown" rather than being wrong. */
#define SPIN_OWNER_MAX 64
static struct { uint32_t ea; unsigned long tid; } s_spin_owner[SPIN_OWNER_MAX];

static void spin_note_acquire(uint32_t ea)
{
    for (int i = 0; i < SPIN_OWNER_MAX; i++)
        if (s_spin_owner[i].ea == 0 || s_spin_owner[i].ea == ea) {
            s_spin_owner[i].ea = ea; s_spin_owner[i].tid = GetCurrentThreadId(); return;
        }
}
static void spin_note_release(uint32_t ea)
{
    for (int i = 0; i < SPIN_OWNER_MAX; i++)
        if (s_spin_owner[i].ea == ea) { s_spin_owner[i].ea = 0; s_spin_owner[i].tid = 0; return; }
}
static unsigned long spin_owner_of(uint32_t ea)
{
    for (int i = 0; i < SPIN_OWNER_MAX; i++)
        if (s_spin_owner[i].ea == ea) return s_spin_owner[i].tid;
    return 0;
}

static void sys_spinlock_lock(ppu_context* ctx)
{
    uint32_t ea = (uint32_t)ctx->gpr[3];
    if (!ea) return;
    /* ponytail: spin briefly, then hand the core back. On real hardware the
     * holder runs to completion in a few instructions; here it is a host thread
     * the OS can deschedule mid-section, so busy-waiting a whole quantum is the
     * wrong trade. Swap in a futex if a title ever shows real contention here. */
    for (unsigned n = 0; !spin_try(ea); n++) {
        if (n < 64) YieldProcessor();
        else        Sleep(0);
        /* Report once per stuck lock, well past any legitimate section. */
        if (n == 200000) {
            unsigned long owner = spin_owner_of(ea);
            unsigned long me    = GetCurrentThreadId();
            static int reported = 0;
            if (__atomic_fetch_add(&reported, 1, __ATOMIC_RELAXED) < 8)
                fprintf(stderr,
                        "[spinlock] STUCK on 0x%08X after 200k spins: guest lr=0x%08X, "
                        "held by tid %lu, this is tid %lu%s\n",
                        ea, (uint32_t)ctx->lr, owner, me,
                        (owner && owner == me)
                          ? "  <== SELF-DEADLOCK: this thread already holds it"
                          : (owner ? "" : "  (holder unknown -- released without us seeing it?)"));
        }
    }
    spin_note_acquire(ea);
}

static void sys_spinlock_trylock(ppu_context* ctx)
{
    uint32_t ea = (uint32_t)ctx->gpr[3];
    int got = (ea && spin_try(ea));
    if (got) spin_note_acquire(ea);
    ctx->gpr[3] = got
                ? 0                                            /* CELL_OK */
                : (uint64_t)(int64_t)(int32_t)0x8001000Au;     /* EBUSY   */
}

static void sys_spinlock_unlock(ppu_context* ctx)
{
    uint32_t ea = (uint32_t)ctx->gpr[3];
    if (!ea) return;
    spin_note_release(ea);
    _InterlockedExchange(spin_word(ea), 0);
}

extern "C" void ppu_sysprx_register(void)
{
    ps3_hle_register_ctx(0x15BAE46Bu, "_cellGcmInitBody", hle_cellGcmInitBody);
    ps3_hle_register_ctx(0xEBE5F72Fu, "sys_spu_image_import",          hle_sys_spu_image_import);
    ps3_hle_register_ctx(0xE0DA8EFDu, "sys_spu_image_close",           hle_sys_spu_image_close);
    ps3_hle_register_ctx(0x9F04F7AFu, "_sys_printf",                   hle_sys_printf);
    ps3_hle_register_ctx(0xFA7F693Du, "_sys_vprintf",                  hle_sys_vprintf);
    ps3_hle_register_ctx(0xA1F9EAFEu, "_sys_sprintf",                  hle_sys_sprintf);
    ps3_hle_register_ctx(0x791B9219u, "_sys_vsprintf",                 hle_sys_vsprintf);
    ps3_hle_register_ctx(0x06574237u, "_sys_snprintf",                 hle_sys_snprintf);
    ps3_hle_register_ctx(0x0618936Bu, "_sys_vsnprintf",                hle_sys_vsnprintf);
    ps3_hle_register_ctx(0x45FE2FCEu, "_sys_spu_printf_initialize",    hle_sys_spu_printf_initialize);
    ps3_hle_register_ctx(0xDD3B27ACu, "_sys_spu_printf_finalize",      hle_sys_spu_printf_finalize);
    ps3_hle_register_ctx(0xDD0C1E09u, "_sys_spu_printf_attach_group",  hle_sys_spu_printf_attach_group);
    ps3_hle_register_ctx(0x5FDFB2FEu, "_sys_spu_printf_detach_group",  hle_sys_spu_printf_detach_group);
    ps3_hle_register_ctx(0x1AE10B92u, "_sys_spu_printf_attach_thread", hle_sys_spu_printf_attach_thread);
    ps3_hle_register_ctx(0xB3BBCF2Au, "_sys_spu_printf_detach_thread", hle_sys_spu_printf_detach_thread);

    /* PS3_NET_ONLINE: real host sockets (libs/network/sysNet.c). Registered
     * first because the first registration of a NID wins, so these shadow the
     * offline model below; without the variable nothing changes. */
    if (getenv("PS3_NET_ONLINE")) ps3_net_host_register(gcm_guest_alloc);
    /* sceNpScore ranking calls take up to 15 arguments: the ones past r10
     * come off the stack, so they need the full context. */
    np_score_register_ctx();
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

    /* Spinlocks. One guest word each, no kernel object; see the block above. */
    ps3_hle_register_ctx(ps3_compute_nid("sys_spinlock_initialize"), "sys_spinlock_initialize", sys_spinlock_initialize);
    ps3_hle_register_ctx(ps3_compute_nid("sys_spinlock_lock"),       "sys_spinlock_lock",       sys_spinlock_lock);
    ps3_hle_register_ctx(ps3_compute_nid("sys_spinlock_trylock"),    "sys_spinlock_trylock",    sys_spinlock_trylock);
    ps3_hle_register_ctx(ps3_compute_nid("sys_spinlock_unlock"),     "sys_spinlock_unlock",     sys_spinlock_unlock);

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
    /* 9 arguments: maxContention arrives on the stack (see cellSpurs.c). */
    ps3_hle_register_ctx(ps3_compute_nid("_cellSpursWorkloadAttributeInitialize"),
                         "_cellSpursWorkloadAttributeInitialize", hle_spurs_wkattr_init);
    ps3_hle_register_ctx(ps3_compute_nid("sys_mmapper_allocate_memory"), "sys_mmapper_allocate_memory", sys_mmapper_allocate_memory);
    ps3_hle_register_ctx(ps3_compute_nid("sys_mmapper_allocate_memory_from_container"), "sys_mmapper_allocate_memory_from_container", sys_mmapper_allocate_memory_from_container);
    ps3_hle_register_ctx(ps3_compute_nid("sys_mmapper_map_memory"),     "sys_mmapper_map_memory",     crt_ok);
    ps3_hle_register_ctx(ps3_compute_nid("sys_mmapper_unmap_memory"),   "sys_mmapper_unmap_memory",   crt_ok);
    ps3_hle_register_ctx(ps3_compute_nid("sys_mmapper_free_memory"),    "sys_mmapper_free_memory",    crt_ok);
}
