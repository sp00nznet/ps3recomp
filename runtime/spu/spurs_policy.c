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
#if defined(_MSC_VER)
    static __declspec(thread) spu_context* t_ctx;
#else
    static _Thread_local spu_context* t_ctx;
#endif
    if (!t_ctx) {
        t_ctx = (spu_context*)malloc(sizeof(spu_context));
        if (!t_ctx) return -1;
    }
    spu_context* ctx = t_ctx;
    memset(ctx, 0, sizeof(*ctx));
    spu_context_init(ctx, 0);
    ctx->image_id    = image_id;
    ctx->policy_mode = 1;

    /* PM image at its load base. */
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
    ctx->gpr[4]._u32[0] = (uint32_t)wkl_data;          /* lo32: the queue EA  */
    ctx->gpr[4]._u32[1] = (uint32_t)(wkl_data >> 32);  /* hi32: aux           */
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

    spu_run_with_halt(entry, ctx);

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
