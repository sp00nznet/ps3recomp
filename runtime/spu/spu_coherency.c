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
#if defined(_MSC_VER)
#include <intrin.h>
#include <windows.h>   /* SwitchToThread */
static volatile long g_lockline = 0;
/* Test-and-test-and-set: spin on a plain read (no cache-line ping-pong) with a
 * pause, and yield after a long wait. The bare exchange loop let GH3's four
 * idle job-policy pollers saturate the line and delay every other SPU atomic
 * -- FMOD's mixer task among them, 25-40 ms at a time. */
void spu_lockline_lock(void)
{
    unsigned spins = 0;
    while (_InterlockedExchange(&g_lockline, 1)) {
        do {
            _mm_pause();
            if (++spins >= 4096) { spins = 0; SwitchToThread(); }
        } while (g_lockline);
    }
}
void spu_lockline_unlock(void) { _InterlockedExchange(&g_lockline, 0); }
#else
#include <stdatomic.h>
static atomic_flag g_lockline = ATOMIC_FLAG_INIT;
void spu_lockline_lock(void)   { while (atomic_flag_test_and_set_explicit(&g_lockline, memory_order_acquire)) { } }
void spu_lockline_unlock(void) { atomic_flag_clear_explicit(&g_lockline, memory_order_release); }
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
static struct { uint32_t line; uint32_t valid; uint32_t used; } s_ppu_resv[SPU_COH_MAX_PPU];
static int s_ppu_resv_hi;                         /* slots in use are below this */
#ifdef _WIN32
static __declspec(thread) int t_ppu_slot = -1;
#else
static __thread int t_ppu_slot = -1;
#endif

static void ppu_resv_clear(uint32_t line, int except)
{
    for (int i = 0; i < s_ppu_resv_hi; i++)
        if (i != except && s_ppu_resv[i].valid && s_ppu_resv[i].line == line)
            s_ppu_resv[i].valid = 0;
}

void spu_coh_ppu_reserve(uint32_t ea)
{
    coh_mark_line(ea);
    if (t_ppu_slot < 0) {
        for (int i = 0; i < SPU_COH_MAX_PPU; i++)
            if (!s_ppu_resv[i].used) {
                s_ppu_resv[i].used = 1;
                t_ppu_slot = i;
                if (i >= s_ppu_resv_hi) s_ppu_resv_hi = i + 1;
                break;
            }
        if (t_ppu_slot < 0) {
            static int warned;
            if (!warned++) fprintf(stderr, "[spu-coh] more than %d PPU threads hold reservations\n",
                                   SPU_COH_MAX_PPU);
            return;
        }
    }
    s_ppu_resv[t_ppu_slot].line = ea & ~127u;
    s_ppu_resv[t_ppu_slot].valid = 1;
}

int spu_coh_ppu_holds(uint32_t ea)
{
    return t_ppu_slot >= 0 && s_ppu_resv[t_ppu_slot].valid &&
           s_ppu_resv[t_ppu_slot].line == (ea & ~127u);
}

void spu_coh_ppu_drop(void)
{
    if (t_ppu_slot >= 0) s_ppu_resv[t_ppu_slot].valid = 0;
}

void spu_coh_ppu_thread_exit(void)
{
    if (t_ppu_slot < 0) return;
    spu_lockline_lock();
    s_ppu_resv[t_ppu_slot].valid = 0;
    s_ppu_resv[t_ppu_slot].used = 0;
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
    for (int i = 0; i < SPU_COH_MAX_CTX; i++)
        if (s_coh_ctxs[i] == ctx) return;
    for (int i = 0; i < SPU_COH_MAX_CTX; i++) {
        if (s_coh_ctxs[i] == NULL) {
            s_coh_ctxs[i] = ctx;
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
        if (s_coh_ctxs[i] == ctx) s_coh_ctxs[i] = NULL;
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
        uintptr_t c = (uintptr_t)s_coh_ctxs[i];
        if (c >= lo && c < hi) {
            static int _n = 0;
            if (__atomic_fetch_add(&_n, 1, __ATOMIC_RELAXED) < 16)
                fprintf(stderr, "[spu-coh] leaked ctx %p (img=%d) dropped at thread exit\n",
                        (void*)c, s_coh_ctxs[i]->image_id);
            s_coh_ctxs[i] = NULL;
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
        spu_context* c = s_coh_ctxs[i];
        if (!c || (const void*)c == self) continue;
        if (c->resv_valid && (c->resv_ea & ~127u) == line) {
            spu_ev_raise(c, SPU_EVENT_LR);
            c->resv_valid = 0;
            g_spu_lr_raise++;
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
        spu_context* c = s_coh_ctxs[i];
        if (!c) continue;
        if (c->resv_valid && (c->resv_ea & ~127u) == line) {
            spu_ev_raise(c, SPU_EVENT_LR);
            c->resv_valid = 0;          /* reservation lost, PUTLLC must fail */
            /* SPU_PUTLLC_WHY=1: name the agent that killed it. A PUTLLC that
             * always fails for "no reservation" is useless without knowing who
             * took it away -- a peer SPU, or the PPU committing to the line. */
            { static _Atomic int s_w = -1;
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
