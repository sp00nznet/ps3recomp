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
#include "spu_interp.h"
#include "spu_coherency.h"        /* spu_interp_run — un-lifted SPU images */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

typedef void (*spu_lifted_entry_fn)(spu_context*);
/* Guest RAM base, for decoding a raw SPU thread's argument block. */
extern uint8_t* vm_base;



/* Run an UN-LIFTED SPU image via the interpreter. Same context/LS/ABI bring-up
 * as spu_run_lifted_job (r1 = LS top, LS in/out, raw-thread arg EA in r3), but
 * the engine is the interpreter fetching from live local store — so a title's
 * SPU jobs run without lifting them first. The channel/DMA ABI (spu_wrch/rdch,
 * MFC) is shared, so DMA, mailboxes, and event signalling behave identically.
 * Returns the SPU's stop code. */
/* inmbox_val: if nonzero, pre-loads the SPU inbound mailbox so the job's first
 * `rdch SPU_RdInMbox` returns it -- this is how a persistent-worker sim SPU
 * receives its per-frame work-descriptor EA (delivered by the game's per-frame
 * event-port send). 0 for the initial group_start run. */
static inline int32_t spu_run_interp_job(uint8_t* local_store, uint32_t entry_pc,
                                         uint32_t args_ea, int image_id,
                                         uint32_t spu_id, uint32_t group_id,
                                         uint32_t inmbox_val)
{
    extern uint8_t* vm_base;
    spu_context ctx;
    spu_context_init(&ctx, 0);
    if (inmbox_val) spu_channel_write(&ctx.ch_in_mbox, inmbox_val);
    /* Identify the context as the real thread so a WrOutMbox/WrOutIntrMbox the
     * SPU issues reaches the RIGHT connected event queue: g_spu_out_mbox_hook ->
     * ydkj_spu_out_mbox_deliver looks the thread up by spu_id. Left at 0 (the
     * spu_context_init default), delivery finds no thread and silently drops the
     * event -- which is why the sim SPUs' natural completion signals never woke
     * the waiting PPU. */
    ctx.spu_id       = spu_id;
    ctx.spu_group_id = group_id;
    /* SPU_CTX_LOG=1: one line per job context, with its address. "Three
     * contexts for one dispatch" is otherwise unanswerable -- every other
     * probe sees contexts only once they reserve. */
    { static int s_cl = -1;
      if (s_cl < 0) s_cl = getenv("SPU_CTX_LOG") ? 1 : 0;
      if (s_cl) { fprintf(stderr, "[spu-ctx] new job context %p image=%d entry=0x%05X\n",
                          (void*)&ctx, image_id, entry_pc); fflush(stderr); } }
    ctx.image_id = image_id;
    ctx.gpr[1]._u32[0] = SPU_LS_SIZE - 0x10;       /* SPU stack top, 16B aligned */
    if (local_store) memcpy(ctx.ls, local_store, SPU_LS_SIZE);
    /* Raw-SPU-thread ABI: sys_spu_thread_argument is 4 u64s (arg1..arg4) passed
     * in r3..r6. Each is a 64-bit effective address the SPU treats as the pair
     * {word0 = EA-low, word1 = EA-high}: DMA code writes MFC_EAL from word0 and
     * asserts (dma.h) that word1 (EA-high) is 0 (main memory is 32-bit
     * addressable). So the low 32 bits of the guest u64 go in word0, high in
     * word1 -- NOT the plain big-endian doubleword order (which would put the EA
     * in word1 and trip the assert on any real pointer arg). */
    if (args_ea && vm_base) {
        for (int i = 0; i < 4; i++) {
            const uint8_t* p = vm_base + args_ea + i * 8;
            uint32_t hi = ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
            uint32_t lo = ((uint32_t)p[4]<<24)|((uint32_t)p[5]<<16)|((uint32_t)p[6]<<8)|p[7];
            ctx.gpr[3 + i]._u32[0] = lo;
            ctx.gpr[3 + i]._u32[1] = hi;
        }
    } else {
        ctx.gpr[3]._u32[0] = args_ea;
    }
    spu_interp_run(&ctx, entry_pc);
    /* Completion signal, exactly ONCE per run. Real hardware raises a PPU event
     * only from WrOutIntrMbox; the plain mailbox is PPU-polled. But a sim SPU
     * that finishes by writing only the PLAIN mailbox (Rubber Ducky's
     * rbodycoll/isosurf/hfluid write 0x3F and never touch the intr mbox) then
     * never wakes the PPU, which blocks in sys_event_queue_receive on its
     * connected queue forever. Synthesise the event for that case here rather
     * than forwarding every plain write from spu_wrch: a run writes the mailbox
     * several times, and one event per WRITE oversupplies the queue until it
     * desynchronises from the frame loop and wedges. One per RUN keeps the
     * queue 1:1 with frames. Per-context, so concurrent SPUs can't race.
     * ponytail: end-of-run check, not a channel-level counter -- the context
     * already records which mailbox the run last used. */
    if (!spu_channel_has_data(&ctx.ch_out_intr_mbox) &&
         spu_channel_has_data(&ctx.ch_out_mbox)) {
        extern void (*g_spu_out_mbox_hook)(uint32_t, uint32_t, int, uint32_t);
        if (g_spu_out_mbox_hook)
            g_spu_out_mbox_hook(ctx.spu_group_id, ctx.spu_id, 1, ctx.ch_out_mbox.value);
    }
    if (local_store) memcpy(local_store, ctx.ls, SPU_LS_SIZE);
    /* `ctx` is about to go out of scope: take it out of the reserving set
     * first, or the coherency walk dereferences this stack frame after it is
     * gone. See spu_coh_unregister. */
    spu_coh_unregister(&ctx);
    return (int32_t)ctx.stop_code;
}

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
/* Optional per-run setup for a RAW SPU THREAD (sys_spu_thread_*), as opposed to
 * a SPURS job. Pass NULL for the job case and nothing changes.
 *
 * A raw SPU thread is a persistent worker: it inits, hands the PPU a ready
 * handshake, then idles on its inbound mailbox waiting for commands. Three
 * things it needs that a fire-and-forget job does not:
 *
 *   inmbox_val    the command the PPU just wrote with
 *                 sys_spu_thread_write_spu_mb, pre-loaded so the worker's
 *                 `rdch SPU_RdInMbox` returns it.
 *   park_on_empty park (halt) instead of spinning when the inbox is empty, so
 *                 the run ENDS at the idle point and the host thread is free.
 *   spu_id        without it, a WrOutMbox the worker issues cannot be matched
 *                 back to an lv2 SPU thread, so the completion event is dropped
 *                 and the PPU waits in sys_event_queue_receive forever. The
 *                 interpreter path already sets this; the lifted path did not.
 */
typedef struct spu_run_opts {
    uint32_t inmbox_val;
    int      park_on_empty;
    uint32_t spu_id;
    uint32_t group_id;
} spu_run_opts;

static inline int32_t spu_run_lifted_job_abi(spu_lifted_entry_fn entry,
                                             uint8_t* local_store,
                                             uint32_t args_ea,
                                             int image_id,
                                             int spurs_task_abi,
                                             const uint32_t* r3_override,
                                             const spu_run_opts* opts)
{
    if (!entry) return -1;
    spu_context ctx;
    spu_context_init(&ctx, 0);
    { static int s_cl = -1;
      if (s_cl < 0) s_cl = getenv("SPU_CTX_LOG") ? 1 : 0;
      if (s_cl) { fprintf(stderr, "[spu-ctx] new job context %p image=%d\n",
                          (void*)&ctx, image_id); fflush(stderr); } }
    ctx.image_id = image_id;     /* select this image's indirect-branch table */
    if (opts) {
        ctx.spu_id              = opts->spu_id;
        ctx.spu_group_id        = opts->group_id;
        ctx.park_on_empty_inmbox = opts->park_on_empty;
        if (opts->inmbox_val) spu_channel_write(&ctx.ch_in_mbox, opts->inmbox_val);
        /* Publish the LIVE context for the duration of the run so the PPU side
         * (sys_spu_thread_write_spu_mb) can poke this worker's mailbox and wake
         * it where it stands, instead of restarting it from its entry.
         *
         * This is what makes a persistent worker actually persistent: blocking
         * inside rdch keeps the host thread's C stack alive, so the SPU's
         * register state survives the wait. Park-and-restart cannot do that --
         * local store persists but registers do not, so a re-run re-executes
         * init and consumes the next command as though it were a startup
         * parameter. */
        extern void spu_thread_publish_ctx(uint32_t tid, void* c);
        if (opts->spu_id) spu_thread_publish_ctx(opts->spu_id, &ctx);
    }
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
            /* Pass the game's own CellSpursTaskArgument through UNCHANGED.
             *
             * Word 1 used to be overwritten with args_ea, the eaContext this
             * runtime's HLE task-attribute handler had recorded. That is the
             * word the task DMAs its 64-byte context from (func_00003ED8 puts
             * it in MFC_EAL), and the substituted address is a PPU STACK
             * temporary: by the third task the frame has already been recycled,
             * so the task DMAs stack junk and decodes garbage.
             *
             * The game's own word 1 is the task's persistent control block --
             * 0x006B4500 / 0x006B4780 / 0x006B4A00 for the three cri decode
             * tasks. Those are load-bearing: the PPU waits on control+0x100
             * (0x006B4600 / 0x006B4880 / 0x006B4B00), which is exactly the flag
             * the task is supposed to set when a work cycle completes. Pointing
             * the task at a different context is why it never sets it.
             *
             * A substituted eaContext remains the fallback below, for a task
             * whose real argument this runtime never captured. */
            ctx.gpr[3]._u32[0] = r3_override[0];   /* 0x40-marker handle       */
            ctx.gpr[3]._u32[1] = r3_override[1];   /* task control block EA    */
            ctx.gpr[3]._u32[2] = r3_override[2];   /* queue/lock EA            */
            ctx.gpr[3]._u32[3] = r3_override[3];
            if (!ctx.gpr[3]._u32[1]) ctx.gpr[3]._u32[1] = args_ea;
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
        /* RAW SPU THREAD (opts set): sys_spu_thread_argument is FOUR u64s
         * (arg1..arg4) passed in r3..r6 -- not the address of that block. Each
         * is a 64-bit EA the SPU reads as {word0 = EA-low, word1 = EA-high},
         * so the guest u64's low half goes in word0 and the high half in word1
         * (plain big-endian doubleword order would put the EA in word1 and trip
         * the EA-high==0 assert in spu_dma.h on any real pointer argument).
         * This is the same decode spu_run_interp_job already does.
         *
         * Handing the raw path `args_ea` itself instead made the worker read a
         * pointer-to-its-arguments where it expected argument one: MultiStream's
         * mixer ran ~12 hops of CRT, touched no channels at all, and returned
         * (branch to LS 0) without ever reaching its service loop. */
        if (opts && args_ea && vm_base) {
            for (int i = 0; i < 4; i++) {
                const uint8_t* p_ = vm_base + args_ea + i * 8;
                uint32_t hi = ((uint32_t)p_[0]<<24)|((uint32_t)p_[1]<<16)|((uint32_t)p_[2]<<8)|p_[3];
                uint32_t lo = ((uint32_t)p_[4]<<24)|((uint32_t)p_[5]<<16)|((uint32_t)p_[6]<<8)|p_[7];
                ctx.gpr[3 + i]._u32[0] = lo;
                ctx.gpr[3 + i]._u32[1] = hi;
            }
        } else {
            ctx.gpr[3]._u32[0] = args_ea;                       /* simple-job arg -> r3 */
        }
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
    if (taskset_ctx && getenv("SPURS_TASKSET_TRACE")) {
        fprintf(stderr, "[taskset] run image=%d r3=%08X %08X %08X %08X "
                "r4=%08X %08X %08X %08X\n", image_id,
                ctx.gpr[3]._u32[0], ctx.gpr[3]._u32[1], ctx.gpr[3]._u32[2], ctx.gpr[3]._u32[3],
                ctx.gpr[4]._u32[0], ctx.gpr[4]._u32[1], ctx.gpr[4]._u32[2], ctx.gpr[4]._u32[3]);
        fflush(stderr);
    }
    int _halted = 0;
    { extern int spu_run_with_halt(void (*)(spu_context*), spu_context*);
      _halted = spu_run_with_halt(entry, &ctx); }               /* run with halt pad   */
    /* Raw-worker run summary (env SPU_WORKER_TRACE). A worker that produces no
     * outbound word leaves the PPU blocked in sys_event_queue_receive forever,
     * and the difference between "parked at its idle poll" and "fell over after
     * three instructions" is invisible without this. */
    if (opts && getenv("SPU_WORKER_TRACE")) {
        fprintf(stderr, "[worker] spu=0x%X halted=%d steps=%llu status=0x%X pc=0x%05X "
                "inmbox(n=%u) outmbox(n=%u v=0x%08X) outintr(n=%u v=0x%08X)\n",
                ctx.spu_id, _halted, (unsigned long long)ctx.steps, (unsigned)ctx.status,
                (unsigned)(ctx.pc & SPU_LS_MASK),
                (unsigned)ctx.ch_in_mbox.count,
                (unsigned)ctx.ch_out_mbox.count, ctx.ch_out_mbox.value,
                (unsigned)ctx.ch_out_intr_mbox.count, ctx.ch_out_intr_mbox.value);
        fflush(stderr);
    }

    /* Raw-thread completion signal. Hardware raises a PPU event only from
     * WrOutIntrMbox; the plain mailbox is PPU-polled. A worker that reports
     * completion on the PLAIN mailbox would otherwise never wake the PPU
     * blocked in sys_event_queue_receive. Same one-per-RUN rule the
     * interpreter path uses -- one event per WRITE oversupplies the queue.
     * Jobs (opts == NULL) keep their old behaviour exactly. */
    if (opts && !spu_channel_has_data(&ctx.ch_out_intr_mbox) &&
        spu_channel_has_data(&ctx.ch_out_mbox)) {
        extern void (*g_spu_out_mbox_hook)(uint32_t, uint32_t, int, uint32_t);
        if (g_spu_out_mbox_hook)
            g_spu_out_mbox_hook(ctx.spu_group_id, ctx.spu_id, 1, ctx.ch_out_mbox.value);
    }
    if (opts && opts->spu_id) {
        extern void spu_thread_publish_ctx(uint32_t tid, void* c);
        spu_thread_publish_ctx(opts->spu_id, 0);   /* run over: ctx is a stack local */
    }
    /* ctx is a stack local that may have taken a lock-line reservation; left
     * in the reserver set it is walked by the next PPU store to that line.
     * GH3's Havok task returning is what first hit it. */
    spu_coh_unregister(&ctx);
    if (local_store) memcpy(local_store, ctx.ls, SPU_LS_SIZE);  /* LS back out */
    return 0;
}

static inline int32_t spu_run_lifted_job_img(spu_lifted_entry_fn entry,
                                             uint8_t* local_store,
                                             uint32_t args_ea,
                                             int image_id)
{
    return spu_run_lifted_job_abi(entry, local_store, args_ea, image_id, 0, 0, 0);
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
