/* Clang-compiled musttail fast path for SPU indirect-branch dispatch.
 *
 * The full resolver (spu_indirect_branch, runtime/spu/spu_channels.c) lives in
 * the MSVC-built ps3recomp_runtime.lib, where __attribute__((musttail)) is
 * unavailable -- its `fn(ctx)` is a plain call, so a guest loop that iterates
 * through an indirect branch (the Bink decoder's per-command dispatch) leaks
 * one ~650-byte resolver frame per iteration and blows the thread stack a few
 * thousand iterations into the first really-decoding movie frame (observed as
 * a silent STATUS_GUARD_PAGE_VIOLATION death).
 *
 * This TU is compiled by clang inside the game build: on a registry hit the
 * dispatch is a GUARANTEED tail call -- zero stack growth. Anything unusual
 * (policy-module service addresses, the taskset PM syscall at LS 0xA70, the
 * image-23 cri hack, lookup miss diagnostics) falls back to the full resolver.
 */
#include "spu_context.h"
#include <stdio.h>
#include <stdlib.h>

typedef void (*spu_dispatch_fn)(spu_context*);
spu_dispatch_fn spu_lookup(uint32_t addr, int image_id);  /* spu_channels.c (exported) */
void spu_indirect_branch(spu_context* ctx);               /* full resolver in the lib */

#define TASKSET_PM_SYSCALL_ADDR 0xA70u   /* mirrors YDKJ_TASKSET_PM_SYSCALL_ADDR */

void spu_indirect_branch_mt(spu_context* ctx)
{
    uint32_t pc = ctx->pc & SPU_LS_MASK;
    /* LBP_SPU_PCWATCH: attribute silent SPU spins (a task that stops logging
     * but never returns). Each SPU job runs on its own host thread, so a
     * thread-local counter + small PC ring is race-free; a trace line prints
     * every ~4M dispatches -- a healthy task finishes long before tripping. */
    { static int s_watch = -1;
      if (s_watch < 0) s_watch = getenv("LBP_SPU_PCWATCH") ? 1 : 0;
      if (s_watch) {
          static _Thread_local unsigned long long n;
          static _Thread_local uint32_t ring[8];
          ring[n & 7] = pc;
          if ((++n & 0x3FFFFFu) == 0)
              fprintf(stderr, "[spu-pcwatch] image=%d %lluM dispatches; recent pc:"
                      " %05X %05X %05X %05X %05X %05X %05X %05X\n",
                      ctx->image_id, n >> 20, ring[0], ring[1], ring[2], ring[3],
                      ring[4], ring[5], ring[6], ring[7]);
      } }
    /* Policy modules MUST use this fast path for ordinary branches too: the
     * lib resolver's plain `fn(ctx)` cannot tail under MSVC, so routing every
     * policy-mode branch there stacked one resolver frame per PM loop
     * iteration -- the moment the jobmanager PM got REAL work (intro skip ->
     * loading jobs) it recursed ~28k deep and stack-overflowed the SPURS
     * kernel thread (C00000FD, bt = spu_indirect_branch repeating). Only the
     * kernel-service addresses (exit-to-kernel / select-workload, which the
     * lib intercepts and RETURNS from without dispatching) and the special
     * HLE addresses still take the slow path. */
    int special = (ctx->policy_mode &&
                   (pc == SPURS_PM_EXIT_TO_KERNEL_LS ||
                    pc == SPURS_PM_SELECT_WORKLOAD_LS))
                  || pc == TASKSET_PM_SYSCALL_ADDR
                  || ctx->image_id == 23;
    if (!special) {
        /* Resident overlay first (mirrors the full resolver): streamed plugin
         * code owns its LS range; base-image entries at the same address are
         * stale bytes and must lose. */
        spu_dispatch_fn fn = ctx->resident_ovl ? spu_lookup(pc, ctx->resident_ovl) : 0;
        if (!fn) fn = spu_lookup(pc, ctx->image_id);
        if (fn) {
            ctx->pc = pc;
            __attribute__((musttail)) return fn(ctx);
        }
    }
    /* slow / special path: full resolver (diagnostics, intercepts, miss log) */
    __attribute__((musttail)) return spu_indirect_branch(ctx);
}
