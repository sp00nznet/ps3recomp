/*
 * ps3recomp - PPU <-> SPU lock-line coherence
 *
 * An SPU that issues GETLLAR holds a reservation on a 128-byte line and then
 * spins, waiting for someone else to change it. On real hardware a store by
 * ANY other processor to that line kills the reservation and raises
 * SPU_EVENT_LR on the reserving SPU, which is how a parked SPURS idle service
 * learns that the PPU has posted work. Emulated, the two halves live on
 * different host threads with nothing between them, so the SPU half alone gets
 * PUTLLC right (it compares the line before committing) while the PPU half
 * never tells anyone it wrote. The service sleeps through the signal.
 *
 * This is the missing half. Every line an SPU reserves is marked in a bitmap;
 * the PPU store paths (runtime/ppu/ppu_memory.h and runtime/ppu/ppu_loader.cpp)
 * check the bitmap, and on a hit commit the store under the same lock-line lock
 * the SPU's GETLLAR/PUTLLC transaction takes, then notify. That does two things
 * at once: the store can no longer land inside a PUTLLC's compare/commit window
 * (where it would be silently reverted), and the reserving SPU gets its event.
 *
 * The bitmap check is a shift and a load against a global that stays zero until
 * an SPU actually reserves something, so a title that never runs SPU code pays
 * one predictable branch per store.
 */

#ifndef SPU_COHERENCY_H
#define SPU_COHERENCY_H

#include "spu_context.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SPU external event: lock-line reservation lost. */
#define SPU_EVENT_LR  0x400u

/* The lock-line lock, striped by line. Every lock-line transaction, SPU and
 * PPU alike -- the SPU's GETLLAR snapshot and PUTLLC compare-and-commit, a
 * PPU store or stwcx to a line some SPU has reserved -- holds the stripe of
 * each 128-byte line it reads or writes, so transactions on different lines
 * run in parallel and those on one line serialize exactly as before.
 *
 * spu_lockline_lock/unlock take EVERY stripe: the old global lock, for the
 * rare operation that has to exclude all line traffic (the context registry)
 * and for callers that do not name an address. Several stripes are always
 * taken in ascending order, so no two holders deadlock.
 *
 * An agent moving its reservation from line A to line B holds both stripes:
 * a notifier writing A then never sees the owner half-way through re-pointing
 * it, so a reservation is neither kept past a write nor lost to a spurious
 * one. Reservation fields read under another line's stripe are accessed
 * atomically. */
#define SPU_LL_STRIPES 64
void     spu_lockline_lock(void);
void     spu_lockline_unlock(void);
uint64_t spu_lockline_mask_ea(uint32_t ea);
uint64_t spu_lockline_mask_range(uint32_t ea, uint32_t len);
void     spu_lockline_lock_mask(uint64_t mask);
void     spu_lockline_unlock_mask(uint64_t mask);

/* A lock-free read of `ea`'s line, seqlock style: every stripe holder bumps
 * the stripe's sequence (odd while held). begin waits for an even value and
 * returns it; the read is good if valid() still sees that value. A GETLLAR
 * reads this way, so SPUs polling one line no longer serialize on its lock. */
uint32_t spu_lockline_read_begin(uint32_t ea);
int      spu_lockline_read_valid(uint32_t ea, uint32_t seq);

/* Mark `ea`'s 128-byte line as SPU-reserved and remember `ctx` as a possible
 * reservation holder. Called from the SPU's GETLLAR with the lock held.
 *
 * Lines are marked and never unmarked. A line the SPURS kernel reserves is
 * reserved again microseconds later, so unmarking would buy nothing; a stale
 * mark costs one locked PPU store to a line the kernel owns anyway. */
void spu_coh_reserve(spu_context* ctx, uint32_t ea);

/* The line the calling PPU thread holds an lwarx reservation on, or ~0u. */
uint32_t spu_coh_ppu_held_line(void);

/* Is `addr`'s line marked? The PPU store fast path -- a range-free O(1) bitmap
 * lookup guarded by a global that is zero until the first GETLLAR. */
int spu_coh_is_reserved(uint32_t addr);

/* A store by the PPU landed on `ea`'s line. Raise SPU_EVENT_LR on every SPU
 * whose live reservation covers it, drop that reservation, and wake the host
 * thread in case it is parked in `rdch SPU_RdEventStat`.
 *
 * Expects the lock-line lock held, so that the store this is reporting and the
 * reservation state it clears are seen by an SPU's PUTLLC as one transaction. */
void spu_coh_notify_write(uint32_t ea);
/* Remove a context from the reserving set. MUST be called before a
 * transient (stack-allocated) spu_context goes out of scope. */
void spu_coh_unregister(spu_context* ctx);
void spu_process_barrier(void);
void spu_coh_forget_range(uintptr_t lo, uintptr_t hi);  /* thread exit */
/* As above, but leaves `self`'s own reservation alone: an SPU's own MFC
 * write does not cost it its reservation on hardware. */
void spu_coh_notify_write_except(uint32_t ea, const void* self);
/* A store by the current PPU thread: like spu_coh_notify_write, but leaves
 * that thread's own lwarx reservation alone. */
void spu_coh_notify_write_from_ppu(uint32_t ea);

/* PPU lwarx/ldarx reservations: the whole 128-byte granule, cleared by any
 * other agent's store to it (see spu_coherency.c). reserve/holds/drop expect
 * the lock-line lock held; thread_exit takes it. */
void spu_coh_ppu_reserve(uint32_t ea);
int  spu_coh_ppu_holds(uint32_t ea);
void spu_coh_ppu_drop(void);
void spu_coh_ppu_thread_exit(void);

/* Count of LR events delivered. A parked SPURS service that never wakes with
 * this at zero is a coherence miss; nonzero moves the question downstream. */
extern unsigned long g_spu_lr_raise;

#ifdef __cplusplus
}
#endif

#endif /* SPU_COHERENCY_H */
