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
 * The SpursKernelContext fields this fills are the ones the lifted wwsjob PM
 * verifiably reads (LS 0x100/0x1C0/0x1D0/0x1E0), laid out per the known SPURS
 * kernel contract:
 *   0x1C0 be u64 spurs EA      0x1D4 wklCurrentUniqueId  0x1DC exitToKernelAddr
 *   0x1C8 be u32 spuNum        0x1D8 wklCurrentId        0x1E0 selectWorkloadAddr
 *   0x1CC be u32 dmaTagId      0x1D0 wklCurrentAddr (PM base 0xA00)
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
                          uint64_t wkl_data, uint32_t wid, uint32_t spurs_ea)
{
    if (!entry || !pm_host || !pm_size || pm_size > SPU_LS_SIZE - 0xA00)
        return -1;

    spu_context* ctx = (spu_context*)calloc(1, sizeof(spu_context));
    if (!ctx) return -1;
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
    KBE32(0x1C0, 0);                    /* spurs EA hi32 */
    KBE32(0x1C4, spurs_ea);             /* spurs EA lo32 */
    KBE32(0x1C8, 0);                    /* spuNum */
    KBE32(0x1CC, 8);                    /* dmaTagId (kernel's tag) */
    KBE32(0x1D0, 0xA00);                /* wklCurrentAddr = PM load base */
    KBE32(0x1D4, wid);                  /* wklCurrentUniqueId */
    KBE32(0x1D8, wid);                  /* wklCurrentId */
    KBE32(0x1DC, SPURS_PM_EXIT_TO_KERNEL_LS);
    KBE32(0x1E0, SPURS_PM_SELECT_WORKLOAD_LS);
#undef KBE32

    /* Entry registers per cellSpursModuleEntry. */
    ctx->gpr[0]._u32[0] = SPURS_PM_EXIT_TO_KERNEL_LS;  /* return-to-kernel link */
    ctx->gpr[1]._u32[0] = 0x3FFB0;                     /* stack (PM reloads its own) */
    ctx->gpr[3]._u32[0] = 0x100;                       /* context */
    ctx->gpr[4]._u32[0] = (uint32_t)(wkl_data >> 32);  /* ea (u64, preferred dword) */
    ctx->gpr[4]._u32[1] = (uint32_t)wkl_data;
    ctx->gpr[5]._u32[0] = 0;                           /* poll status */

    g_spurs_pm_polls  = 0;
    g_spurs_pm_exited = 0;

    fprintf(stderr, "[spurs-pm] RUN wid=%u data=0x%llX spurs=0x%08X image=%d entry=0xA00 size=%u\n",
            wid, (unsigned long long)wkl_data, spurs_ea, image_id, pm_size);
    fflush(stderr);

    spu_run_with_halt(entry, ctx);

    fprintf(stderr, "[spurs-pm] END wid=%u status=0x%X pc=0x%05X polls=%u exited=%u\n",
            wid, ctx->status, ctx->pc, g_spurs_pm_polls, g_spurs_pm_exited);
    fflush(stderr);

    int rc = g_spurs_pm_exited ? 0 : -2;
    free(ctx);
    return rc;
}
