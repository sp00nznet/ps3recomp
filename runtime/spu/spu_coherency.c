/*
 * ps3recomp - PPU <-> SPU lock-line coherence
 *
 * See spu_coherency.h for what this is for. The state here is deliberately
 * plain: a bit per 128-byte line, a small table of the SPU contexts that have
 * ever reserved one, and the lock-line spinlock. Everything that WRITES it runs
 * with the lock-line lock held (the SPU's GETLLAR, the PPU's coherent store),
 * so the read-modify-write on a shared bitmap byte is serialized. Only
 * spu_coh_is_reserved reads without the lock, which is the entire point: it is
 * on every PPU store. The one other unlocked path is a GETLLAR that re-reserves
 * a marked line (spu_coh_reserve_fast); its ordering argument is there.
 */

#include <stdlib.h>
#include "spu_coherency.h"
#include "spu_coh_bitmap.h"

#include <stdint.h>
#include <stdio.h>
#include "ps3emu/log_first.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * The lock-line lock
 *
 * Moved here from spu_channels.c, where it was a file-static and so could
 * serialize SPU against SPU but not SPU against PPU -- which left the PPU free
 * to write a line in the middle of a PUTLLC's compare-and-commit. Same
 * exclusion semantics, now shared. MSVC uses a bounded polite spin loop;
 * POSIX spins briefly and then parks on a mutex (see below).
 * ===========================================================================*/
#if defined(_MSC_VER)
#include <intrin.h>
#include <windows.h>   /* SwitchToThread */
static volatile long g_lockline = 0;
/* Test-and-test-and-set: spin on a plain read (no cache-line ping-pong) with a
 * pause, and yield after a long wait. The bare exchange loop let GH3's four
 * idle job-policy pollers saturate the line and delay every other SPU atomic
 * -- FMOD's mixer task among them, 25-40 ms at a time. */
/* After ~1 ms of spinning (six rounds of 4096 pauses) the waiter sleeps instead.
 * SwitchToThread only hands the core to a thread queued on this same processor,
 * and Sleep(0) only to one of equal or higher priority -- neither reaches a
 * lower-priority holder that is waiting for a core. With every core busy, waiters
 * above the holder's priority could keep it off the CPU for tens of milliseconds.
 * A normal wait is far under a microsecond and never gets here.
 * SPU_LOCKLINE_SLEEP=0 keeps the plain spin. */
void spu_lockline_lock(void)
{
    static int s_sleep = -1;
    unsigned spins = 0, rounds = 0;
    while (_InterlockedExchange(&g_lockline, 1)) {
        if (s_sleep < 0) { const char* e = getenv("SPU_LOCKLINE_SLEEP"); s_sleep = !(e && e[0] == '0'); }
        do {
            _mm_pause();
            if (++spins >= 4096) {
                spins = 0;
                if (rounds++ < 6 || !s_sleep) SwitchToThread(); else Sleep(1);
            }
        } while (g_lockline);
    }
}
void spu_lockline_unlock(void) { _InterlockedExchange(&g_lockline, 0); }
#elif defined(_WIN32)
/* Non-MSVC Windows keeps the plain spin, without a pthread dependency. */
#include <stdatomic.h>
static atomic_flag g_lockline = ATOMIC_FLAG_INIT;
void spu_lockline_lock(void)   { while (atomic_flag_test_and_set_explicit(&g_lockline, memory_order_acquire)) { } }
void spu_lockline_unlock(void) { atomic_flag_clear_explicit(&g_lockline, memory_order_release); }
#else
#include <pthread.h>
#include <time.h>
/* Every SPU reservation and every PPU coherent store takes this one lock, so
 * it is acquired very often and held very briefly (a line copy and a short
 * list walk). But its owner is an ordinary host thread: with more guest
 * threads than cores it can be preempted while holding the lock, and a bare
 * test-and-set loop then keeps every waiter spinning on a full core -- the
 * cores the owner needs to run again -- for the rest of its time slice.
 *
 * So: retry for up to 20 us, with exponentially spaced attempts so the
 * waiters do not keep pulling the lock's cache line away from its owner, and
 * then park on the mutex. A running owner releases well inside the spin; a
 * preempted one no longer costs the waiters their cores. The mutex keeps the
 * same non-recursive exclusion and acquire/release publication. */
static pthread_mutex_t g_lockline = PTHREAD_MUTEX_INITIALIZER;
static inline uint64_t spu_lockline_now_ns(void)
{
#if defined(__APPLE__)
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
#endif
}
static inline void spu_lockline_relax(void)
{
#if defined(__aarch64__)
    __asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
}
void spu_lockline_lock(void)
{
    if (!pthread_mutex_trylock(&g_lockline)) return;
    const uint64_t give_up = spu_lockline_now_ns() + 20000u;
    for (unsigned backoff = 16;; backoff = backoff < 1024u ? backoff * 2u : backoff) {
        for (unsigned i = 0; i < backoff; ++i) spu_lockline_relax();
        if (!pthread_mutex_trylock(&g_lockline)) return;
        if (spu_lockline_now_ns() >= give_up) break;
    }
    if (pthread_mutex_lock(&g_lockline)) abort();
}
void spu_lockline_unlock(void) { if (pthread_mutex_unlock(&g_lockline)) abort(); }
#endif

/* ===========================================================================
 * The reserved-line bitmap
 *
 * One bit per 128-byte line over the whole 32-bit guest address space: 4 MiB of
 * zero-initialized BSS, of which only the pages around addresses an SPU has
 * actually reserved are ever touched. The reference implementation scopes its
 * bitmap to the one window a single title's SPURS structures live in; this
 * runtime hands out guest memory from several disjoint regions (the ELF image,
 * the sys_memory window at 0x40000000, the raw-SPU local-store windows), and a
 * line outside a hardcoded window would be silently non-coherent, which is the
 * failure this whole file exists to remove.
 * ===========================================================================*/
#define SPU_COH_LINES       (1u << (32 - SPU_COH_LINE_SHIFT))   /* 2^25 lines */
#define SPU_COH_BITMAP_SZ   (SPU_COH_LINES / 8)                 /* 4 MiB */

static unsigned char s_coh_bitmap[SPU_COH_BITMAP_SZ];

/* g_spu_coh_bits (spu_coh_bitmap.h) stays NULL until the first GETLLAR
 * anywhere. That keeps the PPU fast path off the bitmap entirely for a title
 * that runs no SPU code, and keeps every existing test that stores through
 * vm_write* paying one compare against a hot global. Written under the lock;
 * read unlocked, where a stale NULL costs at most one missed event on the very
 * first reservation of the run. */

/* Every live virtual context may retain a reservation between scheduling
 * quanta; physical SPU count is not a bound on this set. All access is under
 * the lock-line lock. Entries are removed before their context's lifetime ends.
 *
 * A contiguous array, not a linked list: every notifier walks all of it with
 * the line held, and a list's scattered nodes were a cache miss per holder on
 * top of the one on each holder's resv_valid, which the walk now prefetches a
 * few entries ahead. */
static spu_context** s_coh_ctxs;
static unsigned s_coh_n, s_coh_cap;
#define SPU_COH_PREFETCH 4
static inline void spu_coh_prefetch_holder(unsigned i)
{
    if (i < s_coh_n) __builtin_prefetch(&s_coh_ctxs[i]->resv_valid);
}
static spu_coh_watch* s_coh_watches;

unsigned long g_spu_lr_raise = 0;

/* ===========================================================================
 * Live holder counts (PS3RECOMP_SPU_LIVE_HOLDERS=1, default off)
 *
 * Marks are sticky, so once any SPU has reserved a line every later write to
 * it -- PPU stores and store-conditionals, MFC puts, PUTLLC commits -- walks
 * every registered context under the lock-line lock to raise lost-reservation
 * events, although usually no SPU holds that line any more.
 *
 * On, each context counts itself in s_coh_live for the line of its GETLLAR,
 * before it reads the line, and stays counted until its reservation ends on
 * its own thread (PUTLLC either way, PUTLLUC, a GETLLAR of another line,
 * unregister). A reservation that another agent takes away stays counted until
 * then, so a line's count is never below the number of contexts holding it. A
 * notify that finds the count at zero does everything else as before -- the
 * PPU reservation break, the watch wakes -- and skips only the holder walk.
 * The lock-line lock is still taken by every such write: host code relies on
 * it to make read-modify-writes of these lines atomic against guest stores.
 *
 * Why skipping is safe. A GETLLAR on the lock-free path (spu_coh_reserve_fast)
 * raises the count, then issues the seq_cst fence that already separates
 * publishing its reservation from copying the line. A notify runs after the
 * write and spu_coh_writer_fence(), and only then reads the count. That is the
 * same store-buffering shape as the reservation itself: of the two seq_cst
 * fences one comes first, so either the notify's read sees the increment (and
 * walks), or the GETLLAR's copy already holds the write. A count only drops on
 * the owner's thread after its reservation was consumed, and each decrement
 * follows that context's own increment in the counter's modification order,
 * so a notify that reads the counter after an increment never sees it at zero
 * while that reservation is live. The locked GETLLAR (spu_coh_reserve) counts
 * under the lock-line lock that every notifier holds, so it needs nothing
 * more. Lines share counters (a hashed table), which only keeps the walk.
 * ===========================================================================*/
#define SPU_COH_LIVE_SLOTS 65536u
static uint32_t s_coh_live[SPU_COH_LIVE_SLOTS];
static int s_coh_live_on = -1;
unsigned long g_spu_coh_live_skips;

int spu_coh_live_enabled(void)
{
    int on = __atomic_load_n(&s_coh_live_on, __ATOMIC_RELAXED);
    if (__builtin_expect(on < 0, 0)) {
        const char* e = getenv("PS3RECOMP_SPU_LIVE_HOLDERS");
        on = e && e[0] == '1' && !e[1];
        __atomic_store_n(&s_coh_live_on, on, __ATOMIC_RELAXED);
        if (on) { fprintf(stderr, "[spu-coh] live reservation holder counts on\n"); fflush(stderr); }
    }
    return on;
}

static inline uint32_t* spu_coh_live_slot(uint32_t ea)
{
    return &s_coh_live[(ea >> SPU_COH_LINE_SHIFT) & (SPU_COH_LIVE_SLOTS - 1u)];
}

void spu_coh_live_end(spu_context* c)
{
    if (!c || !c->coh_live_counted) return;
    c->coh_live_counted = 0;
    __atomic_fetch_sub(spu_coh_live_slot(c->coh_live_line), 1u, __ATOMIC_RELEASE);
}

/* Owner thread, at a GETLLAR, before the line is read. */
static void spu_coh_live_begin(spu_context* c, uint32_t ea)
{
    if (!c || spu_coh_live_enabled() <= 0) return;
    const uint32_t line = ea & ~127u;
    if (c->coh_live_counted && c->coh_live_line == line) return;
    spu_coh_live_end(c);
    __atomic_fetch_add(spu_coh_live_slot(line), 1u, __ATOMIC_SEQ_CST);
    c->coh_live_line = line;
    c->coh_live_counted = 1;
}

/* After the writer's fence: may this notify skip the holder walk? */
static inline int spu_coh_live_idle(uint32_t line)
{
    if (spu_coh_live_enabled() <= 0) return 0;
    if (__atomic_load_n(spu_coh_live_slot(line), __ATOMIC_RELAXED) != 0) return 0;
    __atomic_fetch_add(&g_spu_coh_live_skips, 1ul, __ATOMIC_RELAXED);
    return 1;
}

void spu_coh_reserve(spu_context* ctx, uint32_t ea)
{
    /* Read before writing. The kernel reserves the same few lines over and
     * over, and a store of the same value still takes the cache line away
     * from every other core: each PPU store then missed on the pointer and on
     * the bitmap byte. */
    uint32_t line = ea >> SPU_COH_LINE_SHIFT;
    const unsigned char bit = (unsigned char)(1u << (line & 7));
    if (!(s_coh_bitmap[line >> 3] & bit))
        s_coh_bitmap[line >> 3] |= bit;
    if (!g_spu_coh_bits)
        g_spu_coh_bits = s_coh_bitmap;

    if (!ctx) return;
    spu_coh_live_begin(ctx, ea);   /* counted before the caller reads the line */
    for (unsigned i = 0; i < s_coh_n; i++)
        if (s_coh_ctxs[i] == ctx) { __atomic_store_n(&ctx->coh_listed, 1, __ATOMIC_RELAXED); return; }
    if (s_coh_n == s_coh_cap) {
        unsigned cap = s_coh_cap ? s_coh_cap * 2u : 64u;
        spu_context** grown = (spu_context**)realloc(s_coh_ctxs, cap * sizeof(*grown));
        if (!grown) {
            fprintf(stderr, "[spu-coh] cannot register reservation holder %p\n", (void*)ctx);
            fflush(stderr);
            abort(); /* A missed holder loses invalidation events, including ABA. */
        }
        s_coh_ctxs = grown;
        s_coh_cap = cap;
    }
    s_coh_ctxs[s_coh_n++] = ctx;
    __atomic_store_n(&ctx->coh_listed, 1, __ATOMIC_RELAXED);
}

void spu_coh_watch_add(spu_coh_watch* watch, uint32_t ea,
                       void (*wake)(void*), void* opaque)
{
    /* Arming the existing bitmap makes PPU stores/CAS and MFC writes use
     * the same serialized commit/notify path as SPU reservations. */
    spu_coh_reserve(NULL, ea);
    watch->line = ea & ~127u;
    watch->wake = wake;
    watch->opaque = opaque;
    watch->next = s_coh_watches;
    s_coh_watches = watch;
}

void spu_coh_watch_remove(spu_coh_watch* watch)
{
    for (spu_coh_watch** link = &s_coh_watches; *link; link = &(*link)->next) {
        if (*link != watch) continue;
        *link = watch->next;
        watch->next = NULL;
        return;
    }
}

static void spu_coh_wake_watches(uint32_t line)
{
    for (spu_coh_watch* w = s_coh_watches; w; w = w->next)
        if (w->line == line) w->wake(w->opaque);
}

/* Drop a context from the reserving set.
 *
 * There was no way to do this, and the contexts that most need it are
 * TRANSIENT: spu_run_lifted_job_abi runs each job on a STACK-LOCAL
 * spu_context, which registered itself here on its first reservation and then
 * returned. The entry outlived the object, so spu_coh_notify_write walked a
 * dangling pointer -- reading resv_valid, writing event_status and calling
 * spu_ch_wake on reclaimed stack. Undefined behaviour, and observable: Guitar
 * Hero III showed THREE live-looking contexts for one dispatch, stealing each
 * other`s reservations, with every PUTLLC failing "no reservation". */
void spu_coh_unregister(spu_context* ctx)
{
    if (!ctx) return;
    /* Under the lock-line lock, which every walker of s_coh_ctxs holds: a
     * notifier that loaded this pointer just before removal would
     * otherwise go on dereferencing it after the job returned and its stack
     * (or its whole async thread) was gone. GH3 at -O2 crashed that way in
     * spu_coh_notify_write, reading a freed thread stack. */
    spu_lockline_lock();
    for (unsigned i = 0; i < s_coh_n; i++) {
        if (s_coh_ctxs[i] != ctx) continue;
        s_coh_ctxs[i] = s_coh_ctxs[--s_coh_n];   /* order does not matter */
        break;
    }
    __atomic_store_n(&ctx->coh_listed, 0, __ATOMIC_RELAXED);
    spu_coh_live_end(ctx);
    spu_lockline_unlock();
}

/* Drop every registered context that lies in [lo, hi) -- a host thread's
 * stack, called as that thread exits. A stack-local context still registered
 * then has leaked past its unregister; the next walker would read an unmapped
 * stack (GH3 at the end of a Havok task: spu_coh_notify_write faulting).
 * ponytail: safety net that also names the leak; find the path that skips
 * spu_coh_unregister if it keeps firing. */
void spu_coh_forget_range(uintptr_t lo, uintptr_t hi)
{
    spu_lockline_lock();
    for (unsigned i = 0; i < s_coh_n;) {
        uintptr_t c = (uintptr_t)s_coh_ctxs[i];
        if (c >= lo && c < hi) {
            static int _n = 0;
            if (PS3_LOG_FIRST(_n, 16))
                fprintf(stderr, "[spu-coh] leaked ctx %p (img=%d) dropped at thread exit\n",
                        (void*)c, s_coh_ctxs[i]->image_id);
            s_coh_ctxs[i] = s_coh_ctxs[--s_coh_n];   /* re-check slot i */
        } else {
            i++;
        }
    }
    spu_lockline_unlock();
}

int spu_coh_is_reserved(uint32_t addr)
{
    return spu_coh_line_reserved(addr);
}

/* A DMA PUT is issued BY an SPU, and hardware does not take that SPU's own
 * reservation away for its own MFC write -- the MFC is part of the same SPE.
 * Our notify had no way to say "everyone but me", so an SPU that reserved a
 * line and then DMA'd a buffer overlapping it killed its own reservation and
 * its next PUTLLC could never succeed. Guitar Hero III's job policy module
 * livelocks exactly there: 16 million atomic ops on one line, every PUTLLC
 * failing for "no reservation", 600k of them self-inflicted.
 *
 * The PUTLLC path already guards this case by dropping its own reservation
 * before notifying; this gives the DMA path the same ability. */
/* ppu_loader.cpp, when linked. The loader is compiled per game, so the host,
 * the fixtures and any build without it have no PPU reservations to break.
 * POSIX: a weak no-op definition the loader's overrides (a weak undefined
 * reference is not enough: ld64 only lets those stay unresolved for dylib
 * symbols). link.exe/lld-link: the linker's own default (see ppu_gcm_pump in
 * ppu_loader.cpp). */
#if defined(_MSC_VER)
void ppu_resv_break_line(uint32_t ea);
void ppu_resv_break_line_absent(uint32_t ea) { (void)ea; }
#pragma comment(linker, "/alternatename:ppu_resv_break_line=ppu_resv_break_line_absent")
#else
__attribute__((weak)) void ppu_resv_break_line(uint32_t ea) { (void)ea; }
#endif

/* ===========================================================================
 * GETLLAR without the lock-line lock
 *
 * Every SPURS policy module polls its workload lines with GETLLAR, so taking
 * the one global lock for each poll serialized every SPU on a 128-byte copy.
 * A poll is almost always a context that is already in s_coh_ctxs
 * re-reading a line that is already marked, and for that case the lock buys
 * nothing the reservation itself does not:
 *
 *   reader (spu_coh_reserve_fast + caller)    writer (any notifier caller)
 *     resv_valid = 0, resv_ea = ea,             store the new line data
 *     resv_valid = 1                            spu_coh_writer_fence()
 *     seq_cst fence                             load resv_valid / resv_ea
 *     copy the line                             if they cover the line:
 *                                                 LR, resv_valid = 0
 *
 * This is the store-buffering (Dekker) shape: with a seq_cst fence between the
 * store and the load on both sides, the two fences are ordered, so at least
 * one side sees the other. Either the reader's copy already holds the write
 * (an ordinary GETLLAR after the store), or the writer sees the reservation
 * and ends it -- the reader may then hold a stale or torn copy, but its
 * PUTLLC fails and it has SPU_EVENT_LR, exactly as for a write landing just
 * after a locked GETLLAR. A writer that loads stale resv_valid/resv_ea is one
 * whose fence came first, so the copy already holds its data; at worst it
 * ends the new reservation spuriously. What the reader gives up is a copy
 * atomic against a writer's multi-store critical section, but such a copy is
 * always followed by that writer's notify, so the reservation is ended.
 *
 * Two preconditions keep every writer of the line on that right-hand side:
 *  - the line is already marked. Marks are never cleared, so every store path
 *    that checks spu_coh_is_reserved takes the notifier for this line.
 *  - ctx is already in s_coh_ctxs (coh_listed). The entry was added under the
 *    lock by an earlier GETLLAR of this same thread, so any writer whose
 *    critical section could overlap this read walks past it.
 * Anything else takes the lock and spu_coh_reserve as before.
 *
 * Writers still hold the lock-line lock, so they stay serialized with each
 * other and with PUTLLC's compare-and-commit; only the reader stopped
 * waiting. resv_valid and resv_ea are accessed atomically on both sides of
 * the race; everything else about a reservation is still only touched by the
 * owning thread or under the lock.
 * ===========================================================================*/
static inline void spu_coh_writer_fence(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

static inline int spu_coh_holds(const spu_context* c, uint32_t line)
{
    return __atomic_load_n(&c->resv_valid, __ATOMIC_RELAXED) &&
           (__atomic_load_n(&c->resv_ea, __ATOMIC_RELAXED) & ~127u) == line;
}

int spu_coh_reserve_fast(spu_context* ctx, uint32_t ea)
{
    if (!ctx || !__atomic_load_n(&ctx->coh_listed, __ATOMIC_RELAXED) ||
        !spu_coh_is_reserved(ea))
        return 0;
    /* Retire any older reservation before retargeting it. The fence is what
     * the argument above rests on, and what orders the live count (raised
     * here, before it) against the caller's copy of the line. */
    spu_coh_live_begin(ctx, ea);
    __atomic_store_n(&ctx->resv_valid, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&ctx->resv_ea, ea, __ATOMIC_RELAXED);
    __atomic_store_n(&ctx->resv_valid, 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return 1;
}

void spu_coh_notify_write_except(uint32_t ea, const void* self)
{
    uint32_t line = ea & ~127u;
    ppu_resv_break_line(ea);
    spu_coh_writer_fence();
    spu_coh_wake_watches(line);
    if (spu_coh_live_idle(line)) return;   /* no SPU can hold this line now */
    for (unsigned i = 0; i < SPU_COH_PREFETCH; i++) spu_coh_prefetch_holder(i);
    for (unsigned i = 0; i < s_coh_n; i++) {
        spu_coh_prefetch_holder(i + SPU_COH_PREFETCH);
        spu_context* c = s_coh_ctxs[i];
        if (!c || (const void*)c == self) continue;
        if (spu_coh_holds(c, line)) {
            SPU_EVENT_SET(c, SPU_EVENT_LR);
            __atomic_store_n(&c->resv_valid, 0, __ATOMIC_RELAXED);
            g_spu_lr_raise++;
            spu_ch_wake(c);
        }
    }
}

void spu_coh_notify_write(uint32_t ea)
{
    uint32_t line = ea & ~127u;
    ppu_resv_break_line(ea);
    spu_coh_writer_fence();
    spu_coh_wake_watches(line);
    if (spu_coh_live_idle(line)) return;   /* no SPU can hold this line now */
    for (unsigned i = 0; i < SPU_COH_PREFETCH; i++) spu_coh_prefetch_holder(i);
    for (unsigned i = 0; i < s_coh_n; i++) {
        spu_coh_prefetch_holder(i + SPU_COH_PREFETCH);
        spu_context* c = s_coh_ctxs[i];
        if (!c) continue;
        if (spu_coh_holds(c, line)) {
            SPU_EVENT_SET(c, SPU_EVENT_LR);
            __atomic_store_n(&c->resv_valid, 0, __ATOMIC_RELAXED);   /* reservation lost, PUTLLC must fail */
            /* SPU_PUTLLC_WHY=1: name the agent that killed it. A PUTLLC that
             * always fails for "no reservation" is useless without knowing who
             * took it away -- a peer SPU, or the PPU committing to the line. */
            { static int s_w = -1;
              if (s_w < 0) s_w = getenv("SPU_PUTLLC_WHY") ? 1 : 0;
              if (s_w) { static unsigned long long n;
                  if ((++n % 200000) == 1)
                      fprintf(stderr, "[resv-lost] %llu: line 0x%08X taken from img=%d ctx=%p\n",
                              n, line, c->image_id, (void*)c); } }
            g_spu_lr_raise++;
            /* The event_status store has to be visible before the wake. The
             * waiter re-polls its predicate on a timeout as well, so a
             * straggling store costs latency and not the wakeup itself. */
            spu_ch_wake(c);
        }
    }
}

#ifdef __cplusplus
}
#endif
