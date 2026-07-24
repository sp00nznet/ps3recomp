/* spu_lifted_job.h — run a lifted SPU job with the SPURS task ABI.
 *
 * The bridge between the lv2 SPU-thread-group layer (runtime/syscalls/lv2_register.c,
 * which runs SPU threads as registered PPU-fallbacks) and the lifted-execution layer
 * (a lifted spu_func from spu_lifter). A SPURS task / SPU thread receives its argument
 * (the job/task descriptor effective address) in r3 and runs against its 256 KB local
 * store; this helper sets that up, runs the lifted entry, and bridges the local store.
 *
 * Wiring into lv2: register `spu_lifted_fallback` for an SPU image's entry point with
 * `user` = the lifted entry fn; lv2's spu_fallback_thread_proc then calls it with
 * (tid, args_ea, args_size, user), and we run the lifted job on that thread's LS.
 */
#ifndef SPU_LIFTED_JOB_H
#define SPU_LIFTED_JOB_H

#include "spu_context.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

typedef void (*spu_lifted_entry_fn)(spu_context*);

/* Run a lifted SPU job. `local_store` (256 KB, may be NULL) is the SPU thread's
 * local store; it is brought into the context, the job's arg EA is placed in r3
 * (the SPURS task ABI), the lifted entry runs, and the LS is written back.
 * The caller links the channel ABI (spu_wrch/spu_rdch/...) that the lifted code
 * uses to reach DMA / mailboxes / events. Returns the job's exit code (0). */
/* spurs_task_abi: when nonzero, set up r3 per the SPURS task kernel ABI instead
 * of the simple-job ABI. A SPURS task entry expects r3 (128-bit) = a pair:
 *   word0 = {high half 0x40 (kernel marker), low half = DMA tag}
 *   word1 = eaContext  (the task's context save area; the entry DMAs it in)
 * The simple-job ABI just puts the single arg EA in word0. (Verified against the
 * lifted SPURS entry: it checks (r3.word0 >> 16) == 0x40 then DMAs 64 bytes from
 * r3.word1, tag = r3.word0 & 0xFFFF — see spu_func_00003E68/00003ED8.) */
/* r3_override (optional, 4 words, big-endian/native lane values): when non-NULL
 * and spurs_task_abi is set, the full 128-bit r3 the SPURS kernel hands the task
 * is supplied by the caller (captured race-free at dispatch time from the game's
 * eaContext+0x10 descriptor: {0x40-marker handle, eaContext, queue/lock EA,
 * ...}). The claim CAS computes its atomic EA from r3.word2/3 & 0xFFFFFF80, so a
 * zero there locks address 0 and the runtime finds "no ready task". */
static inline int32_t spu_run_lifted_job_abi(spu_lifted_entry_fn entry,
                                             uint8_t* local_store,
                                             uint32_t args_ea,
                                             int image_id,
                                             int spurs_task_abi,
                                             const uint32_t* r3_override)
{
    if (!entry) return -1;
    spu_context ctx;
    spu_context_init(&ctx, 0);
    ctx.image_id = image_id;     /* select this image's indirect-branch table */
    /* Initialize the SPU stack pointer to the top of local store (the SPU ABI
     * expects r1 = top-16, 16-byte aligned, with a NULL back-chain). Without
     * this it is 0 from spu_context_init, so the first `r1 -= frame` wraps
     * negative -> garbage stack -> null function pointers -> branch to LS 0. */
    ctx.gpr[1]._u32[0] = SPU_LS_SIZE - 0x10;   /* 0x3FFF0 for a 256KB LS */
    if (local_store) memcpy(ctx.ls, local_store, SPU_LS_SIZE);  /* job's LS in */
    /* A SpursTasksetContext at LS 0x2700 (built by spurs_pm_build_context) writes
     * syscallAddr=0xA70 at STC_SYSCALL_ADDR (0x27C4). That sentinel means "this is
     * a generic SPURS taskset task" (LBP's audio SPEEX/MultiStream tasks) -- flOw
     * jobs never plant it, so keying off it cannot change their behaviour. */
    #define _LB(o) (((uint32_t)ctx.ls[(o)]<<24)|((uint32_t)ctx.ls[(o)+1]<<16)| \
                    ((uint32_t)ctx.ls[(o)+2]<<8)|ctx.ls[(o)+3])
    int taskset_ctx = (_LB(0x27C4) == 0xA70u);
    if (spurs_task_abi) {
        if (r3_override) {
            ctx.gpr[3]._u32[0] = r3_override[0];   /* 0x40-marker handle      */
            ctx.gpr[3]._u32[1] = args_ea;          /* eaContext (DMA'd first) */
            ctx.gpr[3]._u32[2] = r3_override[2];   /* queue/lock EA           */
            ctx.gpr[3]._u32[3] = r3_override[3];
        } else if (image_id == 22) {
            /* cri_mpv leaf (func_00003E68) gate is `rotmai(r3.word0, 112) == 64`
             * = an ARITHMETIC RIGHT-SHIFT BY 16 then ceqi 64, i.e. it wants
             * (r3.word0 >> 16) == 0x40  ->  r3.word0 = 0x0040xxxx (0x40 in bits
             * 16-23, low 16 = DMA tag). If it matches it takes the REAL decode
             * path (func_00003ED8, which DMAs the video job); else it halts
             * (func_00003ED0). NOTE: r3.word0 = 0x40 (my earlier misread) shifts
             * to 0 and FAILS the gate -> no decode, no DMA. Use 0x00400000. */
            ctx.gpr[3]._u32[0] = 0x00400000u;   /* >>16 == 64 -> cri real decode path */
            ctx.gpr[3]._u32[1] = args_ea;       /* eaContext -> r3.word1 */
        } else if (taskset_ctx) {
            /* Generic SPURS task (RPCS3 spursTasksetStartTask): r3 = the task's
             * 16-byte CellSpursTaskArgument (TaskInfo.args, DMA'd to LS 0x2780 by
             * spurs_pm_build_context), passed by value -- NOT the cri 0x40 marker. */
            ctx.gpr[3]._u32[0] = _LB(0x2780);
            ctx.gpr[3]._u32[1] = _LB(0x2784);
            ctx.gpr[3]._u32[2] = _LB(0x2788);
            ctx.gpr[3]._u32[3] = _LB(0x278C);
        } else {
            ctx.gpr[3]._u32[0] = 0x00400000u;   /* 0x40 marker (>>16==64), DMA tag 0 */
            ctx.gpr[3]._u32[1] = args_ea;       /* eaContext -> r3.word1 */
        }
    } else {
        ctx.gpr[3]._u32[0] = args_ea;                           /* simple-job arg -> r3 */
    }
    /* The r3_override path already carries the game's 0x0040xxxx marker (which
     * passes the (r3.word0>>16)==0x40 gate), so do NOT clobber it. The earlier
     * forced r3.word0=0x40 here was a misread of the gate (see above) and made
     * the cri task halt at func_00003ED0 instead of decoding -- removed. */
    /* SPURS leaf ABI: the real PM (spursTasksetStartTask) sets r4 = {taskset->args (d0),
     * taskset->spurs EA (d1)}, read from the SpursTasksetContext at LS 0x2700+0x60/0x68
     * (planted by spurs_pm_build_context). Without it the leaf reads a garbage SPURS base
     * and DMAs from a bad address / bails at init. Set for image 22 (cri) when the context
     * carries a non-zero spurs ptr. */
    if (spurs_task_abi && (image_id == 22 || taskset_ctx)) {
        uint32_t spurs_lo = _LB(0x2764);
        if (spurs_lo) {
            ctx.gpr[4]._u32[0] = _LB(0x2768);  /* args hi (d0) */
            ctx.gpr[4]._u32[1] = _LB(0x276C);  /* args lo      */
            ctx.gpr[4]._u32[2] = _LB(0x2760);  /* spurs hi (d1)*/
            ctx.gpr[4]._u32[3] = spurs_lo;     /* spurs lo = CellSpurs EA */
        }
    }
    #undef _LB
    if (taskset_ctx && getenv("LBP_TASKSET_TRACE")) {
        fprintf(stderr, "[taskset] run image=%d r3=%08X %08X %08X %08X "
                "r4=%08X %08X %08X %08X\n", image_id,
                ctx.gpr[3]._u32[0], ctx.gpr[3]._u32[1], ctx.gpr[3]._u32[2], ctx.gpr[3]._u32[3],
                ctx.gpr[4]._u32[0], ctx.gpr[4]._u32[1], ctx.gpr[4]._u32[2], ctx.gpr[4]._u32[3]);
        fflush(stderr);
    }
    { extern int spu_run_with_halt(void (*)(spu_context*), spu_context*);
      spu_run_with_halt(entry, &ctx); }                         /* run with halt pad   */
    if (local_store) memcpy(local_store, ctx.ls, SPU_LS_SIZE);  /* LS back out */
    return 0;
}

static inline int32_t spu_run_lifted_job_img(spu_lifted_entry_fn entry,
                                             uint8_t* local_store,
                                             uint32_t args_ea,
                                             int image_id)
{
    return spu_run_lifted_job_abi(entry, local_store, args_ea, image_id, 0, 0);
}

static inline int32_t spu_run_lifted_job(spu_lifted_entry_fn entry,
                                         uint8_t* local_store,
                                         uint32_t args_ea)
{
    return spu_run_lifted_job_img(entry, local_store, args_ea, 0);
}

/* lv2 PPU-fallback wrapper: signature matches spu_ppu_fallback_fn so it can be
 * registered via spu_register_ppu_fallback(entry_point, spu_lifted_fallback, fn).
 * Defined where spu_thread_get_local_store() is available (the lv2 TU); declared
 * here for callers. (uint32_t tid, uint32_t args_ea, uint32_t args_size, void* user) */
int32_t spu_lifted_fallback(uint32_t tid, uint32_t args_ea,
                            uint32_t args_size, void* user);

#endif /* SPU_LIFTED_JOB_H */
