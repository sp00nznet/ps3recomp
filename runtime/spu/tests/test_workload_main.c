/* SPU workload-dispatch test — host (PPU-side) harness.
 *
 * Validates runtime/spu/spu_workload.c, the cellSpurs dispatch brick:
 *   - cross-language FINGERPRINT agreement (C FNV-1a == the Python extractor's),
 *   - the SPU ELF -> local-store LOADER (a marker baked into the image is read
 *     back by the SPU job, proving the LS was loaded and not just zeroed),
 *   - the SPURS task ABI (the job's arg EA propagates into r3),
 *   - a registry MISS for an unregistered image returns 0 (caller can fall back).
 *
 * The SPU job (gen_test_workload.py) forwards both r3 and the LS marker to main
 * memory via DMA PUT; we register its lifted entry under the image fingerprint,
 * dispatch the raw image, and check the results.
 */
#include "spu_recomp.h"
#include "spu_helpers.h"
#include "spu_dma.h"
#include "spu_workload.h"
#include "test_workload_image.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* SPU "main memory" view (DMA dereferences vm_base + ea). */
static uint8_t g_mem[1024];
uint8_t* vm_base = g_mem;
static mfc_engine g_mfc;

u128 spu_rdch(spu_context* ctx, uint32_t ch) {
    (void)ctx;
    u128 r = spu_zero();
    if (ch == 24) r._u32[0] = 0xFFFFFFFFu;   /* MFC_RdTagStat: DMA synchronous -> done */
    return r;
}
uint32_t spu_rchcnt(spu_context* ctx, uint32_t ch) { (void)ctx; (void)ch; return 1; }
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value) {
    mfc_channel_write(&g_mfc, ctx, channel, value._u32[0]);
}
void spu_indirect_branch(spu_context* ctx) {
    (void)ctx; fprintf(stderr, "FAIL: unexpected indirect branch\n");
}
void spu_register_function(uint32_t addr, void (*fn)(spu_context*)) { (void)addr; (void)fn; }

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

/* diagnostics externs referenced from spu_dma.h in the full runtime */
int g_cri_video_dma = 0;
/* stop/halt hooks required by current lifter output */
void spu_stop(spu_context* ctx) { (void)ctx; }
void spu_halt(spu_context* ctx) { (void)ctx; }

int main(void) {
    mfc_engine_init(&g_mfc);
    int ok = 1;

    /* 1) Fingerprint: C FNV-1a must match the Python extractor's value. */
    uint64_t fp = spu_workload_fingerprint(kWorkloadElf, kWorkloadElfSize);
    int fp_ok = (fp == kWorkloadFP);
    printf("  [FINGERPRINT] C=0x%016llX  py=0x%016llX  %s\n",
           (unsigned long long)fp, (unsigned long long)kWorkloadFP,
           fp_ok ? "OK" : "FAIL");
    ok &= fp_ok;

    /* 2) MISS before registration: dispatch must report no handler. */
    memset(g_mem, 0, sizeof(g_mem));
    int miss = spu_workload_dispatch(kWorkloadElf, kWorkloadElfSize, 0);
    int miss_ok = (miss == 0);
    printf("  [MISS       ] unregistered image -> dispatch=%d            %s\n",
           miss, miss_ok ? "OK" : "FAIL");
    ok &= miss_ok;

    /* 3) Register the lifted entry under the image fingerprint, then dispatch
     *    the raw image with a task arg. The job forwards r3 + the LS marker. */
    spu_workload_register(fp, spu_func_00000000, "test_workload");
    printf("  [REGISTER   ] count=%u\n", spu_workload_count());

    const uint32_t kArg = 0x0000ABCDu;
    memset(g_mem, 0, sizeof(g_mem));
    int hit = spu_workload_dispatch(kWorkloadElf, kWorkloadElfSize, kArg);

    uint32_t got_arg    = be32(&g_mem[0]);    /* LS[0x100] word0 = r3 (task arg)  */
    uint32_t got_marker = be32(&g_mem[16]);   /* LS[0x110] word0 = baked-in marker */
    int hit_ok    = (hit == 1);
    int arg_ok    = (got_arg == kArg);
    int marker_ok = (got_marker == kWorkloadMarker);

    printf("  [DISPATCH   ] hit=%d                                       %s\n",
           hit, hit_ok ? "OK" : "FAIL");
    printf("  [TASK ARG   ] r3 -> main_mem = 0x%08X (expected 0x%08X)  %s\n",
           got_arg, kArg, arg_ok ? "OK" : "FAIL");
    printf("  [LS LOADED  ] marker         = 0x%08X (expected 0x%08X)  %s\n",
           got_marker, kWorkloadMarker, marker_ok ? "OK" : "FAIL");
    ok &= hit_ok & arg_ok & marker_ok;

    if (ok)
        printf("  PASS: fingerprint + ELF->LS load + task-arg ABI + dispatch all correct.\n");
    return ok ? 0 : 1;
}
