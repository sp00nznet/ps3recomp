/*
 * ps3recomp - PPU <-> SPU lock-line coherence
 *
 * See spu_coherency.h for what this is for. The state here is deliberately
 * plain: a bit per 128-byte line, a small table of the SPU contexts that have
 * ever reserved one, and the lock-line spinlock. Everything that WRITES it runs
 * with the lock-line lock held (the SPU's GETLLAR, the PPU's coherent store),
 * so the read-modify-write on a shared bitmap byte is serialized. Only
 * spu_coh_is_reserved reads without the lock, which is the entire point: it is
 * on every PPU store.
 */

#include <stdlib.h>
#include "spu_coherency.h"
#ifndef _WIN32
#include <execinfo.h>
#include <dlfcn.h>
#endif

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * The lock-line lock
 *
 * Moved here from spu_channels.c, where it was a file-static and so could
 * serialize SPU against SPU but not SPU against PPU -- which left the PPU free
 * to write a line in the middle of a PUTLLC's compare-and-commit. Same
 * spinlock, same semantics, now shared. _InterlockedExchange is a clang-cl/MSVC
 * intrinsic (no runtime library symbol needed); elsewhere use the C11
 * equivalent, which lowers to the same LL/SC or lock-xchg on every target.
 * ===========================================================================*/
/* Test-and-test-and-set: spin on a plain read (no cache-line ping-pong) with
 * a pause, and yield after a long wait. The bare exchange loop let GH3's four
 * idle job-policy pollers saturate the line and delay every other SPU atomic
 * -- FMOD's mixer task among them, 25-40 ms at a time -- and on POSIX, where
 * it lasted longest, it was 10% of all busy CPU in inFamous (T-0015).
 *
 * Striped since T-0015: one global lock for every atomic in the process
 * serialized SPUs and PPU threads working on unrelated lines. Each stripe
 * sits on its own 128-byte host line (Apple silicon's line size), so two
 * stripes never share one. */
#if defined(_MSC_VER)
#include <intrin.h>
#include <windows.h>   /* SwitchToThread */
typedef struct { __declspec(align(128)) volatile long v; volatile long seq; char pad[120]; } ll_stripe;
static ll_stripe g_ll[SPU_LL_STRIPES];
static void ll_take(ll_stripe* s)
{
    unsigned spins = 0;
    while (_InterlockedExchange(&s->v, 1)) {
        do {
            _mm_pause();
            if (++spins >= 4096) { spins = 0; SwitchToThread(); }
        } while (s->v);
    }
    _InterlockedIncrement(&s->seq);                  /* odd: a writer is in */
}
static void ll_drop(ll_stripe* s) { _InterlockedIncrement(&s->seq); _InterlockedExchange(&s->v, 0); }
static uint32_t ll_seq_begin(ll_stripe* s)
{
    long q;
    while ((q = s->seq) & 1) _mm_pause();
    MemoryBarrier();
    return (uint32_t)q;
}
static int ll_seq_valid(ll_stripe* s, uint32_t q) { MemoryBarrier(); return (uint32_t)s->seq == q; }
#else
#include <stdatomic.h>
#include <sched.h>
typedef struct { _Alignas(128) atomic_int v; atomic_uint seq; char pad[120]; } ll_stripe;
static ll_stripe g_ll[SPU_LL_STRIPES];
static inline void ll_pause(void)
{
#if defined(__aarch64__) || defined(__arm64__)
    __asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
}
static void ll_take(ll_stripe* s)
{
    unsigned spins = 0;
    while (atomic_exchange_explicit(&s->v, 1, memory_order_acquire)) {
        do {
            ll_pause();
            if (++spins >= 4096) { spins = 0; sched_yield(); }
        } while (atomic_load_explicit(&s->v, memory_order_relaxed));
    }
    /* Seqlock writer: odd while held, and the increment ordered before any
     * write the holder makes (Boehm's seqlock pattern). */
    const unsigned q = atomic_load_explicit(&s->seq, memory_order_relaxed);
    atomic_store_explicit(&s->seq, q + 1u, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
}
static void ll_drop(ll_stripe* s)
{
    const unsigned q = atomic_load_explicit(&s->seq, memory_order_relaxed);
    atomic_store_explicit(&s->seq, q + 1u, memory_order_release);
    atomic_store_explicit(&s->v, 0, memory_order_release);
}
static uint32_t ll_seq_begin(ll_stripe* s)
{
    unsigned q;
    while ((q = atomic_load_explicit(&s->seq, memory_order_acquire)) & 1u) ll_pause();
    return q;
}
static int ll_seq_valid(ll_stripe* s, uint32_t q)
{
    atomic_thread_fence(memory_order_acquire);
    return atomic_load_explicit(&s->seq, memory_order_relaxed) == q;
}
#endif

/* Which stripe guards a line: neighbouring lines and lines a power-of-two
 * apart (job lists, per-SPU arrays) spread out. */
static inline unsigned ll_stripe_of(uint32_t ea)
{
    const uint32_t line = ea >> 7;
    return (line ^ (line >> 6) ^ (line >> 12) ^ (line >> 18)) & (SPU_LL_STRIPES - 1u);
}

/* SPU_LL_GLOBAL=1: every transaction takes one single lock -- exactly the
 * global lock this replaced (not all 64 stripes, which costs far more), for
 * an A/B in the same binary. */
static int ll_global(void)
{
    static int on = -1;   /* plain int: __atomic builtins below */
    int v = __atomic_load_n(&on, __ATOMIC_RELAXED);
    if (v < 0) { v = getenv("SPU_LL_GLOBAL") ? 1 : 0; __atomic_store_n(&on, v, __ATOMIC_RELAXED); }
    return v;
}

uint64_t spu_lockline_mask_ea(uint32_t ea)
{
    return ll_global() ? ~0ull : 1ull << ll_stripe_of(ea);
}

uint64_t spu_lockline_mask_range(uint32_t ea, uint32_t len)
{
    if (!len) return 0;
    if (ll_global()) return ~0ull;
    const uint32_t first = ea & ~127u, last = (uint32_t)(ea + (len - 1u)) & ~127u;
    if (last - first >= 128u * 4u * SPU_LL_STRIPES) return ~0ull;
    uint64_t m = 0;
    for (uint32_t line = first; ; line += 128u) {
        m |= 1ull << ll_stripe_of(line);
        if (line == last) break;
    }
    return m;
}

static ll_stripe g_ll_single;     /* SPU_LL_GLOBAL: the one lock */
static int ll_global(void);

void spu_lockline_lock_mask(uint64_t mask)
{
    if (mask && ll_global()) { ll_take(&g_ll_single); return; }
    while (mask) {                       /* ascending: the deadlock-free order */
        const int i = __builtin_ctzll(mask);
        ll_take(&g_ll[i]);
        mask &= mask - 1u;
    }
}

void spu_lockline_unlock_mask(uint64_t mask)
{
    if (mask && ll_global()) { ll_drop(&g_ll_single); return; }
    while (mask) {
        const int i = __builtin_ctzll(mask);
        ll_drop(&g_ll[i]);
        mask &= mask - 1u;
    }
}

static ll_stripe* ll_for_read(uint32_t ea)
{
    return ll_global() ? &g_ll_single : &g_ll[ll_stripe_of(ea)];
}
uint32_t spu_lockline_read_begin(uint32_t ea)          { return ll_seq_begin(ll_for_read(ea)); }
int      spu_lockline_read_valid(uint32_t ea, uint32_t q) { return ll_seq_valid(ll_for_read(ea), q); }

void spu_lockline_lock(void)   { spu_lockline_lock_mask(~0ull); }
void spu_lockline_unlock(void) { spu_lockline_unlock_mask(~0ull); }

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
#define SPU_COH_LINE_SHIFT  7                                  /* 128-byte lines */
#define SPU_COH_LINES       (1u << (32 - SPU_COH_LINE_SHIFT))   /* 2^25 lines */
#define SPU_COH_BITMAP_SZ   (SPU_COH_LINES / 8)                 /* 4 MiB */

static unsigned char s_coh_bitmap[SPU_COH_BITMAP_SZ];

/* Zero until the first GETLLAR anywhere. Keeps the PPU fast path off the
 * bitmap entirely for a title that runs no SPU code, and keeps every existing
 * test that stores through vm_write* paying one compare against a hot global.
 * Written once, under the lock; read unlocked, where a stale zero costs at most
 * one missed event on the very first reservation of the run. */
static int s_coh_armed;

/* The SPU contexts that have reserved a line. Sized to match the MFC engine
 * registry in spu_channels.c, which is the ceiling on live SPU contexts. */
#define SPU_COH_MAX_CTX 8
static spu_context* s_coh_ctxs[SPU_COH_MAX_CTX];

unsigned long g_spu_lr_raise = 0;

/* A memory barrier executed on behalf of every thread of the process: when it
 * returns, each thread has passed a full barrier since this call began. The
 * asymmetric half of the store-then-check handshake in VM_WRITE_COH, which
 * keeps the PPU's common store path free of a hardware fence. */
#if defined(_WIN32)
void spu_process_barrier(void) { FlushProcessWriteBuffers(); }
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_state.h>   /* thread_get_register_pointer_values */
void spu_process_barrier(void)
{
    /* Reading a thread's register state forces it to a context-synchronising
     * point. (On ARM64 an mprotect TLB shootdown is broadcast in hardware and
     * does not serialise the other cores, so that trick is not a barrier.) */
    thread_act_array_t th; mach_msg_type_number_t n = 0;
    if (task_threads(mach_task_self(), &th, &n) != KERN_SUCCESS) {
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        return;
    }
    for (mach_msg_type_number_t i = 0; i < n; i++) {
        uintptr_t sp, regs[128]; size_t cnt = 128;
        (void)thread_get_register_pointer_values(th[i], &sp, &cnt, regs);
        mach_port_deallocate(mach_task_self(), th[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)th, n * sizeof(th[0]));
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}
#elif defined(__linux__)
#include <linux/membarrier.h>
#include <sys/syscall.h>
#include <unistd.h>
void spu_process_barrier(void)
{
    static _Atomic int s_ok = -1;
    if (s_ok < 0)
        s_ok = syscall(__NR_membarrier, MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED, 0, 0) == 0;
    if (!s_ok || syscall(__NR_membarrier, MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0, 0) != 0)
        syscall(__NR_membarrier, MEMBARRIER_CMD_GLOBAL, 0, 0);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}
#else
void spu_process_barrier(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
#endif

static void coh_mark_line(uint32_t ea)
{
    uint32_t line = ea >> SPU_COH_LINE_SHIFT;
    /* Atomic, though the writers hold the lock-line lock: spu_coh_is_reserved
     * reads these with no lock on every PPU store. A line's first reservation
     * is the one moment a PPU store can slip past: its writer checked the bit
     * (still 0), and its unlocked store may land after this SPU's snapshot.
     * The PPU side stores first and re-checks (VM_WRITE_COH); a process-wide
     * barrier here, before the caller snapshots the line, orders the two --
     * every PPU thread either has its store visible to the snapshot or sees
     * the bit on its re-check and breaks the reservation. Bits are never
     * cleared, so the barrier runs once per line, ever. */
    const unsigned char bit = (unsigned char)(1u << (line & 7));
    const unsigned char was = __atomic_fetch_or(&s_coh_bitmap[line >> 3], bit, __ATOMIC_SEQ_CST);
    __atomic_store_n(&s_coh_armed, 1, __ATOMIC_SEQ_CST);
    if (!(was & bit)) spu_process_barrier();
}

/* ---------------------------------------------------------------------------
 * PPU reservations (lwarx/ldarx), held to the same rule as an SPU's GETLLAR:
 * the reservation is the whole 128-byte granule, and ANY store to the granule
 * by another agent -- an SPU DMA PUT, PUTLLC or PUTLLUC, another PPU thread's
 * store or store-conditional -- clears it. Comparing values cannot express
 * that (a store of identical bytes still clears a reservation), so a PPU
 * reservation marks its line in the bitmap like an SPU's does, which routes
 * every writer of the line through the notify below, and the notify clears
 * the record. A thread's own plain stores leave its own reservation alone.
 *
 * Records live in this static table, indexed by a per-thread slot claimed on
 * the thread's first lwarx and released at its exit: nothing points into a
 * thread's stack, so a thread that dies without releasing leaks a slot, not
 * a dangling pointer. Every access holds the lock-line lock.
 * -------------------------------------------------------------------------*/
#define SPU_COH_MAX_PPU 256
/* line/valid: written by the owner under the stripe(s) of the line(s) it
 * reserves, cleared by a notifier under the stripe of the line it writes,
 * read by every notifier -- so accessed atomically. used: claimed by CAS. */
/* lv: line (low 32) and valid (bit 32) in one word, cleared by CAS from the
 * exact value a notifier saw -- the same protocol as spu_context.resv_word. */
static struct { uint64_t lv; uint32_t used; uint32_t pad; } s_ppu_resv[SPU_COH_MAX_PPU];
static int s_ppu_resv_hi;                         /* slots in use are below this */
#define PSLOT_LV(i)     __atomic_load_n(&s_ppu_resv[i].lv, __ATOMIC_ACQUIRE)
#define PSLOT_LINE(i)   ((uint32_t)PSLOT_LV(i))
#define PSLOT_VALID(i)  ((int)(PSLOT_LV(i) >> 32))
#ifdef _WIN32
static __declspec(thread) int t_ppu_slot = -1;
#else
static __thread int t_ppu_slot = -1;
#endif

static void ppu_resv_clear(uint32_t line, int except)
{
    const int hi = __atomic_load_n(&s_ppu_resv_hi, __ATOMIC_ACQUIRE);
    for (int i = 0; i < hi; i++) {
        if (i == except) continue;
        uint64_t v = PSLOT_LV(i);
        if ((v >> 32) && (uint32_t)v == line)
            __atomic_compare_exchange_n(&s_ppu_resv[i].lv, &v, v & 0xFFFFFFFFull, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
}

void spu_coh_ppu_reserve(uint32_t ea)
{
    coh_mark_line(ea);
    if (t_ppu_slot < 0) {
        for (int i = 0; i < SPU_COH_MAX_PPU; i++) {
            uint32_t z = 0;
            if (__atomic_compare_exchange_n(&s_ppu_resv[i].used, &z, 1u, 0,
                                            __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
                t_ppu_slot = i;
                int hi = __atomic_load_n(&s_ppu_resv_hi, __ATOMIC_RELAXED);
                while (hi < i + 1 &&
                       !__atomic_compare_exchange_n(&s_ppu_resv_hi, &hi, i + 1, 0,
                                                    __ATOMIC_RELEASE, __ATOMIC_RELAXED)) { }
                break;
            }
        }
        if (t_ppu_slot < 0) {
            static int warned;
            if (!warned++) fprintf(stderr, "[spu-coh] more than %d PPU threads hold reservations\n",
                                   SPU_COH_MAX_PPU);
            return;
        }
    }
    __atomic_store_n(&s_ppu_resv[t_ppu_slot].lv, (uint64_t)(ea & ~127u) | (1ull << 32), __ATOMIC_RELEASE);
}

/* The line this thread holds a PPU reservation on, or ~0u: a new lwarx elsewhere
 * has to hold that line's stripe as well (spu_coherency.h). */
uint32_t spu_coh_ppu_held_line(void)
{
    if (t_ppu_slot < 0 || !PSLOT_VALID(t_ppu_slot)) return ~0u;
    return PSLOT_LINE(t_ppu_slot);
}

int spu_coh_ppu_holds(uint32_t ea)
{
    return t_ppu_slot >= 0 && PSLOT_VALID(t_ppu_slot) &&
           PSLOT_LINE(t_ppu_slot) == (ea & ~127u);
}

void spu_coh_ppu_drop(void)
{
    if (t_ppu_slot >= 0)
        __atomic_store_n(&s_ppu_resv[t_ppu_slot].lv, PSLOT_LV(t_ppu_slot) & 0xFFFFFFFFull, __ATOMIC_RELEASE);
}

void spu_coh_ppu_thread_exit(void)
{
    if (t_ppu_slot < 0) return;
    spu_lockline_lock();
    __atomic_store_n(&s_ppu_resv[t_ppu_slot].lv, 0ull, __ATOMIC_RELEASE);
    __atomic_store_n(&s_ppu_resv[t_ppu_slot].used, 0u, __ATOMIC_RELEASE);
    spu_lockline_unlock();
    t_ppu_slot = -1;
}

void spu_coh_reserve(spu_context* ctx, uint32_t ea)
{
    coh_mark_line(ea);

    if (!ctx) return;
    /* Already a member anywhere? Look at every slot before taking a free one:
     * stopping at the first free slot re-added a context that sat in a later
     * slot (an earlier context's slot had since been freed), and unregister
     * then removed one copy and left the other pointing at a dead stack. */
    /* Only this context's own thread adds it, so the membership check cannot
     * race another add of the same context; the slot is claimed by CAS because
     * other contexts join under other lines' stripes. */
    for (int i = 0; i < SPU_COH_MAX_CTX; i++)
        if (__atomic_load_n(&s_coh_ctxs[i], __ATOMIC_ACQUIRE) == ctx) return;
    for (int i = 0; i < SPU_COH_MAX_CTX; i++) {
        spu_context* none = NULL;
        if (__atomic_compare_exchange_n(&s_coh_ctxs[i], &none, ctx, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            /* SPU_COH_LOG=1: name each context as it joins the reserving set,
             * with the host call chain that reserved, so an entry that outlives
             * its context can be traced to the path that skipped unregister. */
#ifndef _WIN32
            { static _Atomic int s_l = -1; if (s_l < 0) s_l = getenv("SPU_COH_LOG") ? 1 : 0;
              if (s_l) {
                  void* bt[8]; int n = backtrace(bt, 8);
                  fprintf(stderr, "[spu-coh] + ctx %p img=%d spu=0x%X slot %d:", (void*)ctx,
                          ctx->image_id, ctx->spu_id, i);
                  for (int k = 1; k < n; k++) { Dl_info di;
                      if (dladdr(bt[k], &di) && di.dli_sname) fprintf(stderr, " %s", di.dli_sname); }
                  fprintf(stderr, "\n"); } }
#endif
            return;
        }
    }
    /* More live SPU contexts than the registry holds. The ones already in it
     * still get their events; this one would silently never wake, so say so. */
    { static int warned = 0;
      if (!warned) { warned = 1;
          fprintf(stderr, "[spu-coh] more than %d reserving SPU contexts -- "
                          "spu=0x%X will not receive lock-line events\n",
                  SPU_COH_MAX_CTX, ctx->spu_id);
          fflush(stderr); } }
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
     * notifier that loaded this pointer just before the NULL store would
     * otherwise go on dereferencing it after the job returned and its stack
     * (or its whole async thread) was gone. GH3 at -O2 crashed that way in
     * spu_coh_notify_write, reading a freed thread stack. */
    spu_lockline_lock();
    for (int i = 0; i < SPU_COH_MAX_CTX; i++)
        if (__atomic_load_n(&s_coh_ctxs[i], __ATOMIC_RELAXED) == ctx)
            __atomic_store_n(&s_coh_ctxs[i], NULL, __ATOMIC_RELEASE);
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
    for (int i = 0; i < SPU_COH_MAX_CTX; i++) {
        uintptr_t c = (uintptr_t)__atomic_load_n(&s_coh_ctxs[i], __ATOMIC_RELAXED);
        if (c >= lo && c < hi) {
            static int _n = 0;
            if (__atomic_fetch_add(&_n, 1, __ATOMIC_RELAXED) < 16)
                fprintf(stderr, "[spu-coh] leaked ctx %p (img=%d) dropped at thread exit\n",
                        (void*)c, ((spu_context*)c)->image_id);
            __atomic_store_n(&s_coh_ctxs[i], NULL, __ATOMIC_RELEASE);
        }
    }
    spu_lockline_unlock();
}

int spu_coh_is_reserved(uint32_t addr)
{
    if (!__atomic_load_n(&s_coh_armed, __ATOMIC_ACQUIRE)) return 0;
    uint32_t line = addr >> SPU_COH_LINE_SHIFT;
    return (__atomic_load_n(&s_coh_bitmap[line >> 3], __ATOMIC_RELAXED) >> (line & 7)) & 1u;
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
void spu_coh_notify_write_except(uint32_t ea, const void* self)
{
    uint32_t line = ea & ~127u;
    ppu_resv_clear(line, -1);
    for (int i = 0; i < SPU_COH_MAX_CTX; i++) {
        spu_context* c = __atomic_load_n(&s_coh_ctxs[i], __ATOMIC_ACQUIRE);
        if (!c || (const void*)c == self) continue;
        if (spu_resv_break(c, line)) {
            spu_ev_raise(c, SPU_EVENT_LR);
            __atomic_fetch_add(&g_spu_lr_raise, 1ul, __ATOMIC_RELAXED);   /* raised under different stripes */
            spu_ch_wake(c);
        }
    }
}

static void notify_spus(uint32_t line);

void spu_coh_notify_write(uint32_t ea)
{
    ppu_resv_clear(ea & ~127u, -1);
    notify_spus(ea & ~127u);
}

/* A store by the current PPU thread: its own reservation survives it. */
void spu_coh_notify_write_from_ppu(uint32_t ea)
{
    ppu_resv_clear(ea & ~127u, t_ppu_slot);
    notify_spus(ea & ~127u);
}

static void notify_spus(uint32_t line)
{
    for (int i = 0; i < SPU_COH_MAX_CTX; i++) {
        spu_context* c = __atomic_load_n(&s_coh_ctxs[i], __ATOMIC_ACQUIRE);
        if (!c) continue;
        if (spu_resv_break(c, line)) {        /* lost: PUTLLC must fail */
            spu_ev_raise(c, SPU_EVENT_LR);
            /* SPU_PUTLLC_WHY=1: name the agent that killed it. A PUTLLC that
             * always fails for "no reservation" is useless without knowing who
             * took it away -- a peer SPU, or the PPU committing to the line. */
            { static _Atomic int s_w = -1;
              if (s_w < 0) s_w = getenv("SPU_PUTLLC_WHY") ? 1 : 0;
              if (s_w) { static unsigned long long n;
                  if ((__atomic_add_fetch(&n, 1ull, __ATOMIC_RELAXED) % 200000) == 1)
                      fprintf(stderr, "[resv-lost] %llu: line 0x%08X taken from img=%d ctx=%p\n",
                              n, line, c->image_id, (void*)c); } }
            __atomic_fetch_add(&g_spu_lr_raise, 1ul, __ATOMIC_RELAXED);   /* raised under different stripes */
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
