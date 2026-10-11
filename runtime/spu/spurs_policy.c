/* spurs_policy.c -- enter a SPURS policy module on an SPU.
 *
 * The SPURS kernel (libs/spurs/spurs_kernel.c) owns the SPU: its local store,
 * the kernel context at LS 0x100, and which workload's module image sits at
 * LS 0xA00. spu_pm_enter only performs the entry (cellSpursModuleEntry):
 *
 *   r0 = exitToKernel            r3 = kernel context (0x100)
 *   r1 = stack                   r4 = the workload argument, then the module's EA
 *                                r5 = poll status
 *
 * and runs the module -- lifted, or on the interpreter -- until it branches to
 * exitToKernel. Its selectWorkload calls (cellSpursModulePollStatus) are
 * answered by the kernel through g_spurs_kernel_select: intercepted in
 * spu_indirect_branch for lifted code, through a stop for interpreted code.
 */
#include "spu_context.h"
#include "spu_workload.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int spu_run_with_halt(void (*)(spu_context*), spu_context*);

/* Set by the exitToKernel intercept (spu_channels.c) for the module this
 * host thread -- one SPU -- is running. */
SPU_THREAD_LOCAL unsigned g_spurs_pm_exited = 0;
volatile unsigned g_wws_batch_gets = 0;   /* loadCommands (LS 0xC00) fetch count, spu_dma.h */

uint64_t (*g_spurs_kernel_select)(spu_context* ctx, uint32_t is_poll) = 0;

/* Kernel-service stubs for an interpreted module: real SPU code at the
 * addresses the kernel context advertises, each ending its run in a stop the
 * loop below services (lifted modules are intercepted in spu_indirect_branch
 * instead). */
#define PM_STOP_EXIT    0x3E1u
#define PM_STOP_SELECT  0x3E4u

int spu_pm_enter(spu_context* ctx, spu_lifted_entry_fn entry, int image_id,
                 uint64_t arg, uint64_t pm_ea, uint32_t poll_status)
{
    /* Entry registers as libsre's kernel sets them (t_workload reads them
     * back): r0 = exitToKernel, r1 = stack, r3 = kernel context, r4 = the
     * workload argument then the module's EA, r5 = poll status. */
    memset(&ctx->gpr[0], 0, sizeof ctx->gpr[0]);
    ctx->gpr[0]._u32[0] = SPURS_PM_EXIT_TO_KERNEL_LS;
    memset(&ctx->gpr[1], 0, sizeof ctx->gpr[1]);
    ctx->gpr[1]._u32[0] = 0x3FFB0;
    memset(&ctx->gpr[3], 0, sizeof ctx->gpr[3]);
    ctx->gpr[3]._u32[0] = 0x100;
    ctx->gpr[4]._u32[0] = (uint32_t)(arg >> 32);
    ctx->gpr[4]._u32[1] = (uint32_t)arg;
    ctx->gpr[4]._u32[2] = (uint32_t)(pm_ea >> 32);
    ctx->gpr[4]._u32[3] = (uint32_t)pm_ea;
    memset(&ctx->gpr[5], 0, sizeof ctx->gpr[5]);
    ctx->gpr[5]._u32[0] = poll_status;
    ctx->image_id = image_id;
    ctx->policy_mode = 1;
    ctx->status = 0;
    g_spurs_pm_exited = 0;

    if (entry) {
        spu_run_with_halt(entry, ctx);
        return g_spurs_pm_exited ? 0 : -1;
    }
    extern uint32_t spu_interp_run(spu_context*, uint32_t);
    static const uint8_t k_exit[4] = { 0x00, 0x00, 0x03, 0xE1 };      /* stop 0x3E1 */
    static const uint8_t k_sel[4]  = { 0x00, 0x00, 0x03, 0xE4 };      /* stop 0x3E4 */
    memcpy(ctx->ls + SPURS_PM_EXIT_TO_KERNEL_LS, k_exit, 4);
    memcpy(ctx->ls + SPURS_PM_SELECT_WORKLOAD_LS, k_sel, 4);
    ctx->policy_mode = 0;                       /* no lifted intercepts */
    uint32_t pc = 0xA00;
    int rc = -1;
    for (;;) {
        spu_interp_run(ctx, pc);
        if (ctx->status != SPU_STATUS_STOPPED_BY_STOP) break;
        if (ctx->stop_code == PM_STOP_EXIT) { rc = 0; break; }
        if (ctx->stop_code != PM_STOP_SELECT || !g_spurs_kernel_select) break;
        const uint64_t r = g_spurs_kernel_select(ctx, ctx->gpr[3]._u32[0]);
        memset(&ctx->gpr[3], 0, sizeof ctx->gpr[3]);
        ctx->gpr[3]._u32[0] = (uint32_t)(r >> 32);
        ctx->gpr[3]._u32[1] = (uint32_t)r;
        pc = ctx->gpr[0]._u32[0] & SPU_LS_MASK;   /* return to the caller (bisl $0) */
        ctx->status = 0;
    }
    ctx->policy_mode = 1;
    if (rc)
        fprintf(stderr, "[spurs-pm] interpreted module image %d ended without exitToKernel: "
                        "status 0x%X stop 0x%X pc 0x%05X\n",
                image_id, ctx->status, ctx->stop_code, (uint32_t)ctx->pc & SPU_LS_MASK);
    return rc;
}
