/* SPU lifter torture-KAT harness. See gen_test_torture.py for the KAT list --
 * each targets a bug class the LBP Bink bring-up hit in the wild (preferred-
 * halfword branches, ila-continuations, computed-branch stack growth).
 *
 * Run with clang (GCC=clang ./run_tests.sh): K6's 100k-iteration bi-loop needs
 * guaranteed-flat dispatch (musttail); under gcc the fallback relies on
 * sibling-call optimization and may fail for harness reasons, not lifter bugs.
 */
#include "spu_recomp.h"
#include "spu_helpers.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "test_torture_expected.h"

uint8_t* vm_base = 0;

/* ---- marker collection (SPU_WrOutMbox) ---- */
static uint32_t g_marks[64];
static int g_nmarks = 0;
static int g_stopped = 0;

u128 spu_rdch(spu_context* ctx, uint32_t ch) { (void)ctx; (void)ch; return spu_zero(); }
uint32_t spu_rchcnt(spu_context* ctx, uint32_t ch) { (void)ctx; (void)ch; return 1; }
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value) {
    (void)ctx;
    if (channel == SPU_WrOutMbox && g_nmarks < 64)
        g_marks[g_nmarks++] = value._u32[0];
}
void spu_stop(spu_context* ctx) { (void)ctx; g_stopped = 1; }
void spu_halt(spu_context* ctx) { (void)ctx; g_stopped = 1; }

/* ---- minimal real dispatch: registry + (musttail) resolver ---- */
typedef void (*fnp)(spu_context*);
static struct { uint32_t addr; fnp fn; } g_reg[4096];
static int g_nreg = 0;
void spu_register_function(uint32_t addr, fnp fn) {
    if (g_nreg < 4096) { g_reg[g_nreg].addr = addr; g_reg[g_nreg].fn = fn; g_nreg++; }
}
void spu_indirect_branch(spu_context* ctx) {
    uint32_t pc = ctx->pc & SPU_LS_MASK;
    for (int i = 0; i < g_nreg; i++) {
        if (g_reg[i].addr == pc) {
#if defined(__clang__)
            __attribute__((musttail)) return g_reg[i].fn(ctx);
#else
            g_reg[i].fn(ctx);
            return;
#endif
        }
    }
    fprintf(stderr, "FAIL: unresolved indirect branch to 0x%05X "
            "(missing lifted function -- discovery bug?)\n", pc);
    exit(3);
}

void spu_recomp_register(void);
void spu_func_00000000(spu_context* ctx);

/* diagnostics externs referenced from spu_dma.h in the full runtime */
int g_cri_video_dma = 0;

int main(void) {
    spu_context* ctx = (spu_context*)calloc(1, sizeof(spu_context));
    if (!ctx) { printf("FAIL: oom\n"); return 2; }
    spu_context_init(ctx, 0);
    spu_recomp_register();

    spu_func_00000000(ctx);

    int bad = 0;
    if (!g_stopped) { printf("FAIL: guest never reached stop\n"); bad = 1; }
    if (g_nmarks != TORTURE_EXPECTED_N) {
        printf("FAIL: %d markers, expected %d\n", g_nmarks, TORTURE_EXPECTED_N);
        bad = 1;
    }
    int n = g_nmarks < TORTURE_EXPECTED_N ? g_nmarks : TORTURE_EXPECTED_N;
    for (int i = 0; i < n; i++) {
        if (g_marks[i] != TORTURE_EXPECTED[i]) {
            printf("FAIL: KAT %d -> 0x%08X (expected 0x%08X)\n",
                   i + 1, g_marks[i], TORTURE_EXPECTED[i]);
            bad = 1;
        }
    }
    if (bad) return 1;
    printf("OK: %d/%d torture KATs (halfword branches, gate reduction, "
           "ila-continuation, 100k bi-loop)", g_nmarks, TORTURE_EXPECTED_N);
    free(ctx);
    return 0;
}
