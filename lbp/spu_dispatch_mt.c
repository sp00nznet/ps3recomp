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

typedef void (*spu_dispatch_fn)(spu_context*);
spu_dispatch_fn spu_lookup(uint32_t addr, int image_id);  /* spu_channels.c (exported) */
void spu_indirect_branch(spu_context* ctx);               /* full resolver in the lib */

#define TASKSET_PM_SYSCALL_ADDR 0xA70u   /* mirrors YDKJ_TASKSET_PM_SYSCALL_ADDR */

void spu_indirect_branch_mt(spu_context* ctx)
{
    uint32_t pc = ctx->pc & SPU_LS_MASK;
    if (!ctx->policy_mode && pc != TASKSET_PM_SYSCALL_ADDR && ctx->image_id != 23) {
        spu_dispatch_fn fn = spu_lookup(pc, ctx->image_id);
        if (fn) {
            ctx->pc = pc;
            __attribute__((musttail)) return fn(ctx);
        }
    }
    /* slow / special path: full resolver (diagnostics, intercepts, miss log) */
    __attribute__((musttail)) return spu_indirect_branch(ctx);
}
