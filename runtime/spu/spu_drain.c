/* SPU_DRAIN trampoline model -- runtime state + stub hooks (faithful-adopt).
 *
 * Milestone 1 lands the execution-model plumbing; the per-transfer hooks
 * (lockstep gate, flight recorder, SPURS task-launch) are inert stubs here and
 * gain real bodies in their own milestones. Keeping them as real out-of-line
 * functions (not macros) means the SPU_DRAIN/SPU_RET call sites are already in
 * place -- later milestones only replace the bodies. */
#include "spu_context.h"

/* Pending cross-function transfer target for this host thread's SPU context. */
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*) = 0;

/* Round-robin lockstep token gate (milestone: spu_lockstep). No-op until then. */
void yz_lockstep_tick(spu_context* ctx) { (void)ctx; }

/* SPURS task-launch interception at a trampoline hop (milestone: SPURS kernel).
 * No-op until then. */
void spu_task_launch_check(spu_context* ctx, void* fn) { (void)ctx; (void)fn; }

/* Restore the active image after a lifted call bracket. The brsl/bisl emission
 * saves image_id in a call-site local and hands it back here so an image
 * adopted inside the callee cannot leak into the caller's continuation. A
 * persistent LS-0xA00 workload module (module_img_a00) is re-applied since the
 * plain restore would undo it. */
void spu_img_restore(spu_context* ctx, int32_t saved_img)
{
    ctx->image_id = ctx->module_img_a00 ? ctx->module_img_a00 : saved_img;
}

/* Wake a host thread blocked in a channel wait (milestone: channel-stall).
 * No-op until blocking channel reads exist. */
void spu_ch_wake(spu_context* ctx) { (void)ctx; }
