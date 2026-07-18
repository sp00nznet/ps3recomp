/* SPU bring-up rung 5 brick — lifted-job adapter (host side).
 *
 * Drives a lifted SPU job through spu_run_lifted_job (the lv2<->lifted bridge),
 * passing a task-argument EA (the SPURS task ABI: arg pointer in r3). The job
 * DMA-reads its input from [arg], echoes it to [arg+0x40], and raises completion.
 * Verifies the job ran with the PPU-supplied arg and produced output there.
 *
 * This is the brick that lets the lv2 SPU-thread-group layer run LIFTED SPU code
 * as a task body (register spu_lifted_fallback for the image entry): the missing
 * link between rung 4 (taskset) and full flOw integration.
 */
#include "spu_recomp.h"
#include "spu_helpers.h"
#include "spu_dma.h"
#include "spu_lifted_job.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static uint8_t  g_mem[1u << 20];        /* 1 MB shared "main memory" */
uint8_t*        vm_base = g_mem;
static mfc_engine g_mfc;
static uint32_t g_evt = 0; static int g_evt_wrote = 0;

u128 spu_rdch(spu_context* ctx, uint32_t ch) { (void)ctx; (void)ch; return spu_zero(); }
uint32_t spu_rchcnt(spu_context* ctx, uint32_t ch) { (void)ctx; (void)ch; return 1; }
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value) {
    if (channel == SPU_WrOutIntrMbox) { g_evt = value._u32[0]; g_evt_wrote = 1; return; }
    mfc_channel_write(&g_mfc, ctx, channel, value._u32[0]);
}
void spu_indirect_branch(spu_context* ctx) { (void)ctx; fprintf(stderr, "FAIL: indirect branch\n"); }
void spu_register_function(uint32_t addr, void (*fn)(spu_context*)) { (void)addr; (void)fn; }

extern void spu_func_00000000(spu_context*);   /* the lifted task entry */

static uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static void wbe32(uint8_t* p, uint32_t v) { p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }

/* diagnostics externs referenced from spu_dma.h in the full runtime */
int g_cri_video_dma = 0;
/* stop/halt hooks required by current lifter output */
void spu_stop(spu_context* ctx) { (void)ctx; }
void spu_halt(spu_context* ctx) { (void)ctx; }

int main(void) {
    memset(g_mem, 0, sizeof(g_mem));
    mfc_engine_init(&g_mfc);

    /* The PPU side places a task descriptor / input at some EA and hands its
     * address to the task (the SPURS arg). Here: input 0xABCD1234 at EA 0x800. */
    const uint32_t ARG_EA = 0x800;
    const uint32_t kInput = 0xABCD1234u;
    wbe32(&g_mem[ARG_EA], kInput);

    /* Run the lifted job as a SPURS task: arg in r3, LS = a per-thread local store. */
    static uint8_t local_store[SPU_LS_SIZE];
    memset(local_store, 0, sizeof(local_store));
    int32_t rc = spu_run_lifted_job(spu_func_00000000, local_store, ARG_EA);

    uint32_t result = be32(&g_mem[ARG_EA + 0x40]);   /* job wrote here, relative to its arg */
    int arg_ok   = (result == kInput);               /* job used the PPU-supplied arg EA */
    int event_ok = (g_evt_wrote && g_evt == 0x0ADA); /* completion raised */

    printf("  [ADAPTER] spu_run_lifted_job rc=%d  arg_ea=0x%X\n", rc, ARG_EA);
    printf("  [TASK ARG] job read [arg] + wrote [arg+0x40] = 0x%08X (expected 0x%08X)  %s\n",
           result, kInput, arg_ok ? "OK" : "FAIL");
    printf("  [EVENT  ] completion = 0x%X (wrote=%d)  %s\n", g_evt, g_evt_wrote, event_ok ? "OK" : "FAIL");
    if (arg_ok && event_ok)
        printf("  PASS: lv2<->lifted adapter ran a lifted SPU job with the SPURS task ABI.\n");
    return (arg_ok && event_ok) ? 0 : 1;
}
