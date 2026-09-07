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

#include "spu_coherency.h"

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
static volatile long g_lockline = 0;
void spu_lockline_lock(void)   { while (_InterlockedExchange(&g_lockline, 1)) { } }
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

void spu_coh_reserve(spu_context* ctx, uint32_t ea)
{
    uint32_t line = ea >> SPU_COH_LINE_SHIFT;
    s_coh_bitmap[line >> 3] |= (unsigned char)(1u << (line & 7));
    s_coh_armed = 1;

    if (!ctx) return;
    for (int i = 0; i < SPU_COH_MAX_CTX; i++) {
        if (s_coh_ctxs[i] == ctx) return;
        if (s_coh_ctxs[i] == NULL) { s_coh_ctxs[i] = ctx; return; }
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

int spu_coh_is_reserved(uint32_t addr)
{
    if (!s_coh_armed) return 0;
    uint32_t line = addr >> SPU_COH_LINE_SHIFT;
    return (s_coh_bitmap[line >> 3] >> (line & 7)) & 1u;
}

void spu_coh_notify_write(uint32_t ea)
{
    uint32_t line = ea & ~127u;
    for (int i = 0; i < SPU_COH_MAX_CTX; i++) {
        spu_context* c = s_coh_ctxs[i];
        if (!c) continue;
        if (c->resv_valid && (c->resv_ea & ~127u) == line) {
            c->event_status |= SPU_EVENT_LR;
            c->resv_valid = 0;          /* reservation lost, PUTLLC must fail */
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
