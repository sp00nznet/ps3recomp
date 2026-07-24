/*
 * SPU lockstep gate (faithful-adopt, from canersaka's fork) -- a single global
 * run token passed round-robin among registered lifted-SPU host threads so only
 * one SPU executes lifted code at a time, in registration order, quantum-based.
 * Env-gated (YZ_SPU_LOCKSTEP), default OFF: unset => every call is one
 * predicted-not-taken branch and behavior is byte-identical to no gate.
 *
 * Tick site is SPU_DRAIN (spu_context.h) -- the trampoline re-entry, the real
 * per-transfer hot loop. A blocking channel wait (spu_ch_wait, spu_channels.c)
 * brackets with block_begin/block_end so a waiter releases the token before its
 * OS-level wait (a peer SPU that must post the awaited data is never blocked on
 * this ctx). Each ctx is pinned to one host thread for its lifetime; this gates
 * (blocks non-holders) rather than migrating.
 */
#ifndef SPU_LOCKSTEP_H
#define SPU_LOCKSTEP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct spu_context;

/* -1 unarmed, 0 off, 1 on. */
extern volatile int g_yz_lockstep_on;

/* Arms from env (YZ_SPU_LOCKSTEP, YZ_LOCKSTEP_QUANTUM), idempotent, thread-safe;
 * prints the "[lockstep] ARMED" banner once. */
int yz_lockstep_enabled(void);

/* Register/unregister this ctx's host thread in the round-robin ring.
 * register() BLOCKS until this ctx holds the token (first member gets it
 * immediately; later members wait their turn) -- callers must not run lifted
 * SPU code before it returns. Both no-op when unset. */
void yz_lockstep_register(struct spu_context* ctx);
void yz_lockstep_unregister(struct spu_context* ctx);

/* Per-tick-site call (SPU_DRAIN): counts one quantum unit and hands the token
 * to the next runnable member once the quantum expires, blocking until granted
 * the token again. First line is a single g_yz_lockstep_on==0 check when off. */
void yz_lockstep_tick(struct spu_context* ctx);

/* Bracket a blocking channel wait: release the token before the OS wait, rejoin
 * the rotation on wake. No-ops when unset. */
void yz_lockstep_block_begin(struct spu_context* ctx);
void yz_lockstep_block_end(struct spu_context* ctx);

#ifdef __cplusplus
}
#endif
#endif /* SPU_LOCKSTEP_H */
