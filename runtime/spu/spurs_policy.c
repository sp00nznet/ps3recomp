/* spurs_policy.c — run a SPURS workload's policy module on a lifted SPU.
 *
 * The real SPURS kernel dispatches a workload by loading its policy module
 * (a raw SPU binary) at LS 0xA00 and entering it with the ABI of
 * cellSpursModuleEntry(uintptr_t context, uint64_t ea):
 *
 *   r0 = return-to-kernel address     r3 = kernel context LS address (0x100)
 *   r1 = stack (the PM re-loads its   r4 = the workload's u64 data (preferred
 *        own initial stack)                doubleword) — e.g. a joblist EA
 *                                     r5 = poll status
 *
 * The SpursKernelContext fields this fills, per the layout RPCS3 documents
 * (Emu/Cell/Modules/cellSpurs.h, struct SpursKernelContext) and the lifted
 * wwsjob PM verifiably reads. NOTE both pointers are 64-BIT:
 *   0x1C0 be u64 spurs EA           0x1D8 be u32 wklCurrentUniqueId
 *   0x1C8 be u32 spuNum             0x1DC be u32 wklCurrentId
 *   0x1CC be u32 dmaTagId           0x1E0 be u32 exitToKernelAddr
 *   0x1D0 be u64 wklCurrentAddr     0x1E4 be u32 selectWorkloadAddr
 *        (PM base 0xA00, in the LOW word at 0x1D4)
 *
 * exitToKernel/selectWorkload point at two reserved LS addresses below the PM
 * base; spu_indirect_branch intercepts them (ctx->policy_mode) and HLEs the
 * kernel side: select = "no contention, keep running", exit = end of run.
 */
#include "spu_context.h"
#include "spu_workload.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int spu_run_with_halt(void (*)(spu_context*), spu_context*);

/* Counters the intercepts in spu_channels.c maintain for the CURRENT run
 * (single writer per run; reads are diagnostic). */
volatile unsigned g_spurs_pm_polls = 0;   /* selectWorkload calls this run */
volatile unsigned g_spurs_pm_exited = 0;  /* set when exitToKernel was taken */
volatile unsigned g_wws_batch_gets = 0;   /* loadCommands (LS 0xC00) fetch count, spu_dma.h */

int spu_run_policy_module(spu_lifted_entry_fn entry, int image_id,
                          const uint8_t* pm_host, uint32_t pm_size,
                          uint64_t wkl_data, uint32_t wid, uint32_t spurs_ea,
                          uint32_t spu_num)
{
    if (!entry || !pm_host || !pm_size || pm_size > SPU_LS_SIZE - 0xA00)
        return -1;

    /* Reuse one context per thread: this runs per policy dispatch (thousands
     * per second with the SPURS workload churn live), and a fresh 260KB
     * calloc/free each time was allocator + page-zeroing churn. The explicit
     * memset preserves the exact fresh-zero semantics calloc provided. */
    /* PERSISTENT context per (workload, virtual SPU): a real policy module
     * stays RESIDENT on its SPU -- its LS carries continuation state between
     * scheduling quanta (the wwsjob PM caches the last-seen queue line at LS
     * 0x14B0 and its staging rings at 0x12xx; a freshly zeroed context every
     * dispatch made it treat every re-entry as first contact and it could
     * never progress its claim/stage pipeline). Image + context init happen
     * once; re-entries keep LS and only refresh the entry registers and the
     * (possibly game-updated) kernel-context fields. */
    enum { SPURS_PM_MAX_WKL = 16 };
    static spu_context* s_pm_ctx[SPURS_PM_MAX_WKL][6];
    if (wid >= SPURS_PM_MAX_WKL || spu_num >= 6) return -1;
    spu_context* ctx = s_pm_ctx[wid][spu_num];
    int first = 0;
    /* SPURS_PM_FRESH=1: treat EVERY dispatch as first contact (zeroed LS, image
     * reloaded). A/B against the persistent default: with persistence, the wwsjob
     * PM's first entry attaches + claims exactly ONE ticket then yields, and every
     * RE-entry takes a stale-state path that does NOTHING (LS 0x12A0/0x12C0/0x12F0
     * job-pipeline slots persist; entry only partially re-inits them) -- LBP's
     * 10-ticket loading queue stalls at lanes {1,2} forever. Fresh contexts make
     * each dispatch run the full attach->claim->yield cycle, so repeated kernel
     * dispatches drain the queue one ticket at a time (the persistent-LS claim
     * that motivated the cache predates the r4 word0/word1 layout fix). */
    static int s_fresh = -1;
    if (s_fresh < 0) s_fresh = getenv("SPURS_PM_FRESH") ? 1 : 0;
    /* Reset the persistent context when the workload's DATA (queue EA)
     * changes: the game re-points a wid at a NEW job queue once the old one
     * drains, and a real SPURS kernel reloads the policy module fresh on a
     * workload switch. Carrying queue A's LS (header cache 0x14B0, busy
     * masks 0x1270/0x1280, records) into queue B made the PM see stale
     * state and claim nothing (observed: LBP's second loading queue sat at
     * lanes {0,...} while the first drained fine). */
    static uint64_t s_pm_data[SPURS_PM_MAX_WKL][6];
    if (!ctx) {
        ctx = (spu_context*)malloc(sizeof(spu_context));
        if (!ctx) return -1;
        s_pm_ctx[wid][spu_num] = ctx;
        first = 1;
        memset(ctx, 0, sizeof(*ctx));
        spu_context_init(ctx, 0);
    } else if (s_fresh || s_pm_data[wid][spu_num] != wkl_data) {
        if (!s_fresh) {
            static int _n = 0;
            if (_n++ < 8)
                fprintf(stderr, "[spurs-pm] wid=%u spu=%u workload data changed "
                        "0x%llX -> 0x%llX: fresh PM context\n", wid, spu_num,
                        (unsigned long long)s_pm_data[wid][spu_num],
                        (unsigned long long)wkl_data);
        }
        first = 1;
        memset(ctx, 0, sizeof(*ctx));
        spu_context_init(ctx, 0);
    }
    s_pm_data[wid][spu_num] = wkl_data;
    ctx->image_id    = image_id;
    ctx->policy_mode = 1;

    /* PM image at its load base (first entry only -- LS persists). */
    if (first)
        memcpy(ctx->ls + 0xA00, pm_host, pm_size);

    /* SpursKernelContext at LS 0x100 (fields big-endian, like all guest data). */
    uint8_t* ls = ctx->ls;
#define KBE32(off, v) do { uint32_t _v = (v); ls[(off)] = (uint8_t)(_v >> 24); \
        ls[(off)+1] = (uint8_t)(_v >> 16); ls[(off)+2] = (uint8_t)(_v >> 8);   \
        ls[(off)+3] = (uint8_t)_v; } while (0)
    /* wklCurrentAddr is a 64-BIT pointer (vm::bcptr<void,u64>), so it occupies
     * 0x1D0..0x1D7 and everything after it sits 4 bytes higher than a u32 would
     * put it. Writing it as a u32 shifted the whole tail of the struct:
     * exitToKernelAddr landed on selectWorkloadAddr's slot and selectWorkloadAddr
     * fell off into moduleId, leaving 0x1E4 zero. The wwsjob PM reads both --
     * `lqa $2,0x1e0; bisl $0,$2` at LS 0x3588 calls exitToKernel, and
     * `lqa $7,0x1e0; rotqbyi $6,$7,4; bisl $0,$6` at LS 0x35A0 calls
     * selectWorkload -- so it called our exit intercept for every poll and
     * branched to 0 for every select. It also tests wklCurrentId (0x1DC) against
     * 1, which only reads sensibly at the correct offset. */
    KBE32(0x1C0, 0);                    /* spurs EA hi32 (u64 ptr)  */
    KBE32(0x1C4, spurs_ea);             /* spurs EA lo32            */
    KBE32(0x1C8, spu_num);              /* spuNum (virtual SPU / lane row) */
    KBE32(0x1CC, 8);                    /* dmaTagId (kernel's tag)  */
    KBE32(0x1D0, 0);                    /* wklCurrentAddr hi32 (u64 ptr) */
    KBE32(0x1D4, 0xA00);                /* wklCurrentAddr lo32 = PM load base */
    KBE32(0x1D8, wid);                  /* wklCurrentUniqueId       */
    KBE32(0x1DC, wid);                  /* wklCurrentId             */
    KBE32(0x1E0, SPURS_PM_EXIT_TO_KERNEL_LS);
    KBE32(0x1E4, SPURS_PM_SELECT_WORKLOAD_LS);
#undef KBE32

    /* Entry registers per cellSpursModuleEntry. */
    ctx->gpr[0]._u32[0] = SPURS_PM_EXIT_TO_KERNEL_LS;  /* return-to-kernel link */
    ctx->gpr[1]._u32[0] = 0x3FFB0;                     /* stack (PM reloads its own) */
    ctx->gpr[3]._u32[0] = 0x100;                       /* context */
    /* r4 layout is LO-WORD-IN-PREFERRED, not a big-endian u64: verified
     * against the authentic wwsjob PM bytes (entry 0x2BF0 `stqa r4,0x14E0`,
     * attach 0x2720 `brz r4`/`wrch MFC_EAL, r4` -- it treats r4's PREFERRED
     * WORD as the 32-bit queue EA, and `shlqbyi r4,4`'s next word as the aux
     * half). Passing {hi,lo} put 0 in the preferred slot, so the queue attach
     * early-returned on every run: tickets got claimed via the shifted copy
     * but the job-list header never DMA'd in, and the PM wedged forever in
     * its staged-job wait (LBP's post-intro loading freeze). */
    /* ...and the CLAIM path reads the EA from the shlqbyi(r4,4) copy (LS
     * 0x13F0, via func_3038 -> 2418), i.e. r4's SECOND word. Both word 0
     * (attach) and word 1 (claim) must carry the 32-bit EA; the hi half
     * rides in words 2-3. */
    ctx->gpr[4]._u32[0] = (uint32_t)wkl_data;          /* attach reads word0  */
    ctx->gpr[4]._u32[1] = (uint32_t)wkl_data;          /* claim reads word1   */
    ctx->gpr[4]._u32[2] = (uint32_t)(wkl_data >> 32);
    ctx->gpr[4]._u32[3] = (uint32_t)(wkl_data >> 32);
    ctx->gpr[5]._u32[0] = 0;                           /* poll status */

    g_spurs_pm_polls  = 0;
    g_spurs_pm_exited = 0;

    /* RUN/END tracing: thousands of policy dispatches per second made these
     * unconditional fprintf+fflush pairs a real fraction of movie playback.
     * Default off; SPURS_PM_TRACE=1 restores them (first 64 + every 4096th
     * so an enabled trace can't flood either). */
    static int s_trace = -1;
    if (s_trace < 0) s_trace = getenv("SPURS_PM_TRACE") ? 1 : 0;
    static unsigned long s_runs = 0;
    unsigned long runno = ++s_runs;
    int log_this = s_trace && (runno <= 64 || (runno & 0xFFF) == 0);
    if (log_this) {
        fprintf(stderr, "[spurs-pm] RUN#%lu wid=%u data=0x%llX spurs=0x%08X image=%d entry=0xA00 size=%u\n",
                runno, wid, (unsigned long long)wkl_data, spurs_ea, image_id, pm_size);
        fflush(stderr);
    }

    /* SPURS_PM_FLOW=1: capture this run's cross-function transfer trail (drain
     * hook in spu_drain.c) and dump it IF the run performed a PUTLLC (= a queue
     * claim) -- reconstructs the post-claim decision path that ends in a bare
     * exit-to-kernel instead of job staging. Only spu_num 0 is traced (the
     * concurrent lanes would interleave the buffer). */
    extern uint32_t g_pm_flow_buf[]; extern volatile unsigned g_pm_flow_n;
    extern void* volatile g_pm_flow_ctx;
    extern volatile unsigned g_spu_putllc_count;
    extern volatile unsigned g_spu_putllc_sync_hit;
    extern uint32_t g_barrier_sync_watch;
    static int s_flow = -1;
    if (s_flow < 0) s_flow = getenv("SPURS_PM_FLOW") ? 1 : 0;
    unsigned putllc_before = g_spu_putllc_sync_hit;
    (void)g_spu_putllc_count;
    /* Trace ONLY the stalled queue's own runs: wkl_data == the sync EA the PPU
     * barrier/ticket probes armed (ticket-pub arms it at first publish, before
     * the PM's first work pass). Idle re-runs of this wid matter as much as the
     * claim run -- the empty-cursor path returns without any PUTLLC. */
    /* SPURS_PM_FLOW_ALL=1: trace EVERY spu_num-0 run of this workload without
     * requiring the PPU-side barrier probe to have armed g_barrier_sync_watch.
     * Needed to answer "does the job's buffer-binding routine (pm 0x3098, the
     * only writer of logicalToBufferNumArray@0x11B0 + jobState@0x12C0) ever
     * execute?" on a run that never reaches the barrier at all -- the RPCS3
     * guest dump shows a working SPU has 0x11B0 bound and runJobNum=0, ours
     * has 0x11B0 == 0 and runJobNum == -1. */
    static int s_flow_all = -1;
    if (s_flow_all < 0) s_flow_all = getenv("SPURS_PM_FLOW_ALL") ? 1 : 0;
    int tracing = (s_flow && spu_num == 0 && g_pm_flow_ctx == 0 &&
                   (s_flow_all ||
                    (g_barrier_sync_watch &&
                     (uint32_t)wkl_data == g_barrier_sync_watch)));
    static volatile unsigned long long s_flow_arm_ms;
    extern unsigned long long ps3_ms_now(void);
    if (tracing) {
        g_pm_flow_n = 0; g_pm_flow_ctx = ctx;
        s_flow_arm_ms = ps3_ms_now();
        { static int _a = 0; if (_a++ < 8) {
            fprintf(stderr, "[pm-flow] ARM wid=%u spu=%u (watch=0x%08X)\n",
                    wid, spu_num, g_barrier_sync_watch); fflush(stderr); } }
        /* Watchdog: the traced run has been observed to HANG the jobmanager
         * kernel thread (the claim run never returns -> no later dispatch can
         * report). A detached thread dumps the partial trail after 5s; the
         * trail's LAST pc names the looping lifted function. */
        { static int s_wd_started = 0;
          if (!s_wd_started) { s_wd_started = 1;
            extern void spurs_pm_flow_watchdog_start(void);
            spurs_pm_flow_watchdog_start(); } }
    }

    /* SPURS_PM_PIPE=1: trace how the WWS job PIPELINE state evolves across
     * kernel dispatches. Prints the pipeline counters (loadJobState@0x12A0
     * preferred hword, nextLoadJobNum@0x12C0, loadJobNum@0x12D0, runJobNum@
     * 0x12E0, storeJobNum@0x12F0) before and after each PM run, plus the pc
     * spu_run_with_halt halted at. Answers: does re-entry advance the pipeline
     * or reset/stall it? Only wid<=1 (jobmanager) traced, first 4000 runs. */
    static int s_pipe = -1;
    if (s_pipe < 0) s_pipe = getenv("SPURS_PM_PIPE") ? 1 : 0;
    int pipe_this = s_pipe && wid <= 1 && spu_num == 0;
    uint32_t st_before = 0;
    if (pipe_this) {
        const uint8_t* L = ctx->ls;
        #define HW(o) ((L[(o)+2]<<8)|L[(o)+3])   /* preferred hword (bytes 2-3) */
        st_before = HW(0x12A0);
        (void)st_before;
        #undef HW
    }

    /* SPURS_PM_CLAIMLOG=1: per-run claim-vs-stage summary for BOTH lanes.
     * The loading freeze's signature is a dispatch that CLAIMS a ticket
     * (sync-PUTLLC hit) but never STAGES its job (no loadCommands GET): the
     * claimed ticket is eaten and the batch's barrier can never release
     * (observed: 4-job batch, counter=4, lanes {2,1}). Diffing the sync-PUTLLC
     * and 0xC00-GET counters across each run names that dispatch and shows the
     * LS pipeline state it left behind. */
    static int s_claimlog = -1;
    if (s_claimlog < 0) s_claimlog = getenv("SPURS_PM_CLAIMLOG") ? 1 : 0;
    unsigned batch_before = g_wws_batch_gets;
    unsigned claim_before = g_spu_putllc_sync_hit;

    spu_run_with_halt(entry, ctx);

    if (s_claimlog) {
        unsigned claims = g_spu_putllc_sync_hit - claim_before;
        unsigned stages = g_wws_batch_gets - batch_before;
        if (claims || stages) {
            static unsigned s_cl = 0;
            if (s_cl++ < 64) {
                const uint8_t* L = ctx->ls;
                #define W(o)  ((L[(o)]<<24)|(L[(o)+1]<<16)|(L[(o)+2]<<8)|L[(o)+3])
                fprintf(stderr, "[pm-claim] wid=%u spu=%u first=%d claims=%u stages=%u%s halt_pc=0x%05X "
                        "nextLoad=0x%X loadJob=0x%X runJob=0x%X storeJob=0x%X q13F0=%08X q14E0=%08X\n",
                        wid, spu_num, first, claims, stages,
                        (claims && !stages) ? " <-- CLAIM EATEN" : "",
                        ctx->pc & SPU_LS_MASK,
                        W(0x12C0), W(0x12D0), W(0x12E0), W(0x12F0),
                        W(0x13F0), W(0x14E0));
                fflush(stderr);
                #undef W
            }
        }
    }

    if (pipe_this) {
        static unsigned long s_pn = 0;
        if (s_pn++ < 4000) {
            const uint8_t* L = ctx->ls;
            #define HW(o) ((L[(o)+2]<<8)|L[(o)+3])
            #define W(o)  ((L[(o)]<<24)|(L[(o)+1]<<16)|(L[(o)+2]<<8)|L[(o)+3])
            fprintf(stderr, "[pm-pipe] wid=%u run#%lu first=%d halt_pc=0x%05X "
                    "loadState=%u->%u nextLoad=0x%X loadJob=0x%X runJob=0x%X storeJob=0x%X "
                    "lastStore=0x%X\n",
                    wid, s_pn, first, ctx->pc & SPU_LS_MASK,
                    st_before, HW(0x12A0), W(0x12C0), W(0x12D0), W(0x12E0),
                    W(0x12F0), W(0x1300));
            fflush(stderr);
            #undef HW
            #undef W
        }
    }

    if (tracing) {
        g_pm_flow_ctx = 0;
        static int s_dumps = 0;
        /* Only dump once the PPU is actually spinning on a job barrier
         * (g_barrier_sync_watch armed by the barrier probe): boot-time idle
         * runs also PUTLLC (queue-check protocol) and would burn the cap. */
        /* Only dump runs that actually had a JOB STAGED. A boot-time idle run
         * has empty loadCommands(0xC00)/bufferSetArray(0xDF0) and its trail says
         * nothing about the Load->Run gate -- with SPURS_PM_FLOW_ALL tracing
         * every run, the dump budget was being spent entirely on idle runs. */
        int has_job = 0;
        for (uint32_t _i = 0xDF0; _i < 0xE10 && !has_job; _i++)
            if (ctx->ls[_i]) has_job = 1;
        for (uint32_t _i = 0xC00; _i < 0xC60 && !has_job; _i++)
            if (ctx->ls[_i]) has_job = 1;
        if (has_job && s_dumps < 6) {
            s_dumps++;
            unsigned n = g_pm_flow_n; if (n > 8192) n = 8192;
            fprintf(stderr, "[pm-flow] dump#%d wid=%u %u transfers (sync-PUTLLC delta %u):",
                    s_dumps, wid, n, g_spu_putllc_sync_hit - putllc_before);
            uint32_t last = 0xFFFFFFFFu; unsigned rep = 0;
            for (unsigned i = 0; i < n; i++) {
                uint32_t pc = g_pm_flow_buf[i] & SPU_LS_MASK;
                if (pc == last) { rep++; continue; }
                if (rep) { fprintf(stderr, "*%u", rep + 1); rep = 0; }
                fprintf(stderr, "%s%05X", (i % 16) ? " " : "\n  ", pc);
                last = pc;
            }
            if (rep) fprintf(stderr, "*%u", rep + 1);
            fprintf(stderr, "\n");
            /* Pipeline LS state right after this traced run: same regions the
             * hung-run watchdog dumps -- shows what the 5 type-1 passes built
             * (records/ring) and why the type-2 loads never dispatch. */
            { static const struct { uint32_t a, len; const char* tag; } R[] = {
                  { 0xC00, 0x60, "cmds" }, { 0xDF0, 0x40, "rec" },
                  { 0xEB0, 0x60, "ring" }, { 0x11B0, 0x20, "mark" },
                  { 0x1270, 0x20, "busy" }, { 0x12A0, 0x10, "st12A0" },
                  { 0x12C0, 0x40, "state" } };
              for (unsigned r = 0; r < sizeof R / sizeof R[0]; r++) {
                  fprintf(stderr, "[pm-ls] %-6s 0x%04X:", R[r].tag, R[r].a);
                  for (uint32_t o = 0; o < R[r].len; o += 4) {
                      if (o && (o & 15) == 0) fprintf(stderr, " |");
                      fprintf(stderr, " %02X%02X%02X%02X",
                              ctx->ls[R[r].a+o], ctx->ls[R[r].a+o+1],
                              ctx->ls[R[r].a+o+2], ctx->ls[R[r].a+o+3]);
                  }
                  fprintf(stderr, "\n");
              } }
            fflush(stderr);
        }
    }

    if (log_this) {
        fprintf(stderr, "[spurs-pm] END wid=%u status=0x%X pc=0x%05X polls=%u exited=%u\n",
                wid, ctx->status, ctx->pc, g_spurs_pm_polls, g_spurs_pm_exited);
        fflush(stderr);
    }

    return g_spurs_pm_exited ? 0 : -2;
}

/* ---------------------------------------------------------------------------
 * Real taskset-policy probe (Option 2, A/B vs spurs_pm_build_context).
 *
 * Runs the REAL lifted SPURS taskset policy (tsp_spu_func_00000A00, extracted
 * from libsre vaddr 0x23680) far enough to build the SpursTasksetContext at LS
 * 0x2700, then copies that region out for comparison against the C reimpl. The
 * policy's raw bytes (its data tables at LS 0x27B0-0x2840, read via lqr) come
 * from the embedded array so there is no dependency on libsre's guest load
 * address. The policy will eventually `bi` to the selected task's entry (an
 * address in the task image's LS range, unregistered under image id 100) and
 * halt as branch-to-0 -- by then LS 0x2700 holds the authentic context.
 *
 * image_id 100 is reserved for the policy's own indirect-branch table (the
 * lifted policy registers its 130 funcs there via tsp_spu_recomp_register()
 * under spu_begin_image(100)).  Returns the SPU status at halt.
 * ------------------------------------------------------------------------- */
extern const unsigned char g_taskset_policy_bytes[];
extern const unsigned g_taskset_policy_size;
extern void tsp_spu_func_00000A00(spu_context*);

/* pm-flow hang watchdog: dump the traced run's partial transfer trail + live
 * ctx state 5s after arm if the run hasn't returned (g_pm_flow_ctx still set).
 * Reads of the running ctx are racy by design -- diagnostic only. */
#ifdef _WIN32
#include <windows.h>
static DWORD WINAPI spurs_pm_flow_watchdog(LPVOID p)
{
    (void)p;
    extern uint32_t g_pm_flow_buf[]; extern volatile unsigned g_pm_flow_n;
    extern void* volatile g_pm_flow_ctx;
    Sleep(5000);
    spu_context* ctx = (spu_context*)g_pm_flow_ctx;
    if (!ctx) { fprintf(stderr, "[pm-flow] watchdog: run returned (no hang)\n"); return 0; }
    unsigned n = g_pm_flow_n; if (n > 8192) n = 8192;
    fprintf(stderr, "[pm-flow] HUNG-RUN PARTIAL: %u transfers, live pc=0x%05X "
            "LS12A0=%02X%02X LS12C0=%02X%02X LS12F0=%02X%02X%02X%02X LS1300=%02X%02X:",
            n, ctx->pc & SPU_LS_MASK,
            ctx->ls[0x12A0], ctx->ls[0x12A1],
            ctx->ls[0x12C0], ctx->ls[0x12C1],
            ctx->ls[0x12F0], ctx->ls[0x12F1], ctx->ls[0x12F2], ctx->ls[0x12F3],
            ctx->ls[0x1300], ctx->ls[0x1301]);
    /* Pipeline-state hex dump: batch commands (0xC00), per-slot records (0xDF0),
     * staging ring (0xEB0), FFFF markers (0x11B0), scheduler vars (0x1270-0x1340),
     * queue caches (0x13B0-0x1520). One shot -- everything the state-2 cycle's
     * stage decision reads, so the divergent condition can be hand-simulated. */
    { static const struct { uint32_t a, len; const char* tag; } R[] = {
          { 0xC00, 0x60, "cmds" }, { 0xDF0, 0x40, "rec" }, { 0xEB0, 0x40, "ring" },
          { 0x11B0, 0x20, "mark" }, { 0x1270, 0x40, "sched" }, { 0x12C0, 0x50, "state" },
          { 0x13B0, 0x30, "qvar" }, { 0x14A0, 0x30, "qcache" }, { 0x1440, 0x40, "batchrec" } };
      for (unsigned r = 0; r < sizeof R / sizeof R[0]; r++) {
          fprintf(stderr, "\n[pm-ls] %-8s 0x%04X:", R[r].tag, R[r].a);
          for (uint32_t o = 0; o < R[r].len; o += 4) {
              if (o && (o & 15) == 0) fprintf(stderr, " |");
              fprintf(stderr, " %02X%02X%02X%02X",
                      ctx->ls[R[r].a+o], ctx->ls[R[r].a+o+1],
                      ctx->ls[R[r].a+o+2], ctx->ls[R[r].a+o+3]);
          }
      }
      fprintf(stderr, "\n"); }
    uint32_t last = 0xFFFFFFFFu; unsigned rep = 0; unsigned col = 0;
    for (unsigned i = 0; i < n; i++) {
        uint32_t pc = g_pm_flow_buf[i] & SPU_LS_MASK;
        if (pc == last) { rep++; continue; }
        if (rep) { fprintf(stderr, "*%u", rep + 1); rep = 0; }
        fprintf(stderr, "%s%05X", (col++ % 16) ? " " : "\n  ", pc);
        last = pc;
    }
    if (rep) fprintf(stderr, "*%u", rep + 1);
    fprintf(stderr, "\n"); fflush(stderr);
    return 0;
}
void spurs_pm_flow_watchdog_start(void)
{
    HANDLE h = CreateThread(NULL, 0, spurs_pm_flow_watchdog, NULL, 0, NULL);
    if (h) CloseHandle(h);
}
#else
void spurs_pm_flow_watchdog_start(void) {}
#endif

int spurs_run_taskset_policy_probe(uint32_t taskset_ea, uint32_t taskid,
                                   uint32_t spurs_ea, uint64_t wkl_data,
                                   uint32_t wid, uint8_t* out_2700, uint32_t out_len)
{
    spu_context* ctx = (spu_context*)calloc(1, sizeof(spu_context));
    if (!ctx) return -1;
    spu_context_init(ctx, 0);
    ctx->image_id    = 100;      /* policy's own indirect-branch table */
    ctx->policy_mode = 1;

    /* Policy module image at its load base LS 0xA00 (code + data tables). */
    if (g_taskset_policy_size > SPU_LS_SIZE - 0xA00) { free(ctx); return -1; }
    memcpy(ctx->ls + 0xA00, g_taskset_policy_bytes, g_taskset_policy_size);

    /* SpursKernelContext at LS 0x100 (same layout as spu_run_policy_module). */
    uint8_t* ls = ctx->ls;
#define KBE32(off, v) do { uint32_t _v = (v); ls[(off)] = (uint8_t)(_v >> 24); \
        ls[(off)+1] = (uint8_t)(_v >> 16); ls[(off)+2] = (uint8_t)(_v >> 8);   \
        ls[(off)+3] = (uint8_t)_v; } while (0)
    KBE32(0x1C0, 0);                    KBE32(0x1C4, spurs_ea);
    KBE32(0x1C8, 0);                    KBE32(0x1CC, 8);
    KBE32(0x1D0, 0);                    KBE32(0x1D4, 0xA00);
    KBE32(0x1D8, wid);                  KBE32(0x1DC, wid);
    KBE32(0x1E0, SPURS_PM_EXIT_TO_KERNEL_LS);
    KBE32(0x1E4, SPURS_PM_SELECT_WORKLOAD_LS);
    /* taskset EA also planted at the SpursKernelContext taskset slot the policy
     * reads for its taskset DMA (mirrors the cri-r4 injection at LS 0x27B8). */
#undef KBE32

    ctx->gpr[0]._u32[0] = SPURS_PM_EXIT_TO_KERNEL_LS;
    ctx->gpr[1]._u32[0] = 0x3FFB0;
    ctx->gpr[3]._u32[0] = 0x100;
    ctx->gpr[4]._u32[0] = (uint32_t)(wkl_data >> 32);
    ctx->gpr[4]._u32[1] = (uint32_t)wkl_data;
    ctx->gpr[5]._u32[0] = 0;

    g_spurs_pm_polls = 0; g_spurs_pm_exited = 0;

    fprintf(stderr, "[real-pm] RUN taskset=0x%08X task=%u spurs=0x%08X wkl=0x%llX wid=%u\n",
            taskset_ea, taskid, spurs_ea, (unsigned long long)wkl_data, wid);
    fflush(stderr);

    spu_run_with_halt(tsp_spu_func_00000A00, ctx);

    fprintf(stderr, "[real-pm] END status=0x%X pc=0x%05X polls=%u exited=%u\n",
            ctx->status, ctx->pc, g_spurs_pm_polls, g_spurs_pm_exited);
    fflush(stderr);

    if (out_2700 && out_len) {
        uint32_t n = out_len;
        if ((uint32_t)0x2700 + n > SPU_LS_SIZE) n = SPU_LS_SIZE - 0x2700;
        memcpy(out_2700, ctx->ls + 0x2700, n);
    }
    int st = (int)ctx->status;
    free(ctx);
    return st;
}
