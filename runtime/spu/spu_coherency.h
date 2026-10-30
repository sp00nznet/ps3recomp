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
 * check the bitmap, and on a hit commit the store under that line's per-line
 * lock (spu_linelock_lock), bumping its version word, then notify. That does
 * two things at once: the store can no longer land inside a PUTLLC's
 * compare/commit window (where it would be silently reverted), and the
 * reserving SPU gets its event. Per-line locks replace the old one global
 * lock-line lock, so a PPU store to one taskset no longer freezes every other
 * line's SPU atomics.
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

/* The table lock. One process-wide spinlock protecting the reservation-holder
 * list, the watch list, and (via spu_coh_reserve) the reserved-line bitmap's
 * first mark. spu_coh_notify_write and friends take it internally; it is never
 * held across line data access, so it serializes bookkeeping only, never
 * writers of different lines against each other. */
void spu_lockline_lock(void);
void spu_lockline_unlock(void);

/* Per-line lock + version word, RPCS3-style (vm::reservation_acquire). One
 * 64-bit word per 128-byte line, one word per cache line so two hot lines
 * never ping-pong each other:
 *
 *   bit  6     exclusive transaction lock (compare+commit, coherent stores)
 *   bits 7+    version counter, +128 per committed write (advisory: the
 *              128-byte data compare stays the authority; the version is a
 *              debug/A-B-A token and the hook a future fast path can use)
 *
 * A writer of a reserved line takes THIS lock, never the table lock and never
 * a global lock: two writers of DIFFERENT lines proceed in parallel, which the
 * old global lock-line lock forbade. Indexing aliases every 64 KiB (512 words,
 * 32 KiB of BSS) exactly like RPCS3's g_reservations: a write to a line 64 KiB
 * away bumps a shared word, which can only fail a PUTLLC spuriously (the safe
 * direction) -- SPURS lines live within a few KiB, so the aliasing is cold in
 * practice. Callers must pass the line's EA (aligned or not; it is masked).
 *
 * Range variants lock every distinct line in [ea_lo, ea_hi] IN ASCENDING
 * ADDRESS ORDER; the unlock walks the same set in the same order. Single-line
 * lockers never hold more than one line lock, and every multi-line locker
 * acquires in ascending order, so no deadlock is possible. Bump must be called
 * with the line's lock held, after the data write, before unlock. release() is
 * bump+unlock as ONE atomic add (0x40 carries out of the lock bit into the
 * version field: +0x80 version, lock cleared) -- the hot-path pair; plain
 * unlock() stays for paths that locked but wrote nothing. */
void spu_linelock_lock(uint32_t ea);
void spu_linelock_unlock(uint32_t ea);
void spu_linelock_bump(uint32_t ea);
void spu_linelock_release(uint32_t ea);
void spu_linelock_range(uint32_t ea_lo, uint32_t ea_hi);
void spu_linelock_range_unlock(uint32_t ea_lo, uint32_t ea_hi);

/* Mark `ea`'s 128-byte line as SPU-reserved and remember `ctx` as a possible
 * reservation holder. Called from the SPU's GETLLAR with the lock held.
 *
 * Lines are marked and never unmarked. A line the SPURS kernel reserves is
 * reserved again microseconds later, so unmarking would buy nothing; a stale
 * mark costs one locked PPU store to a line the kernel owns anyway. */
void spu_coh_reserve(spu_context* ctx, uint32_t ea);

/* GETLLAR fast path: publish ctx's reservation of ea WITHOUT the lock-line
 * lock, when ctx is already a registered holder and ea's line already marked.
 * Returns 0 when the caller must take the lock and call spu_coh_reserve
 * instead. On 1 the caller copies the line, also without the lock. */
int spu_coh_reserve_fast(spu_context* ctx, uint32_t ea);

/* Is `addr`'s line marked? The PPU store fast path -- a range-free O(1) bitmap
 * lookup guarded by a global that is zero until the first GETLLAR. */
int spu_coh_is_reserved(uint32_t addr);

/* A store by the PPU landed on `ea`'s line. Raise SPU_EVENT_LR on every SPU
 * whose live reservation covers it, drop that reservation, and wake the host
 * thread in case it is parked in `rdch SPU_RdEventStat`.
 *
 * Self-contained: takes the table lock internally; the caller must NOT hold
 * it (holding the line lock during the data write is fine and expected, but
 * release it first -- the kill walk is ordered after the caller's committed
 * write by the caller's own program order, and the 128-byte data compare is
 * the real authority anyway, so the event may safely lag the write). */
void spu_coh_notify_write(uint32_t ea);
/* Remove a context from the reserving set. MUST be called before a
 * transient (stack-allocated) spu_context goes out of scope. */
void spu_coh_unregister(spu_context* ctx);
void spu_coh_forget_range(uintptr_t lo, uintptr_t hi);  /* thread exit */
/* As above, but leaves `self`'s own reservation alone: an SPU's own MFC
 * write does not cost it its reservation on hardware. */
void spu_coh_notify_write_except(uint32_t ea, const void* self);

/* Scoped host waiters for real writes to a guest 128-byte line. Add, remove,
 * and callbacks all run under the lock-line lock. A watch remains caller-owned
 * until remove returns; callbacks must not re-enter coherence or alter watches.
 * They may wake a host waiter, but must not manufacture guest state. */
typedef struct spu_coh_watch {
    uint32_t line;
    void (*wake)(void* opaque);
    void* opaque;
    struct spu_coh_watch* next;
} spu_coh_watch;
void spu_coh_watch_add(spu_coh_watch* watch, uint32_t ea,
                       void (*wake)(void*), void* opaque);
void spu_coh_watch_remove(spu_coh_watch* watch);

/* Count of LR events delivered. A parked SPURS service that never wakes with
 * this at zero is a coherence miss; nonzero moves the question downstream. */
extern unsigned long g_spu_lr_raise;

#ifdef __cplusplus
}
#endif

#endif /* SPU_COHERENCY_H */
