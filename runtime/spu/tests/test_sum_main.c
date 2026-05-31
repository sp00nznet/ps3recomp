/*
 * Integration test #1: sum loop.
 *
 * Drives test_sum.elf's lifted output. The SPU program reads N values from
 * the inbound mailbox, sums them, writes the result to the outbound mailbox.
 *
 * We provide our own spu_rdch / spu_wrch (overriding spu_channels.c) so we
 * can serve a queue of inbound values without modelling real PPU/SPU
 * scheduling. The lifter only knows about the externs declared in
 * spu_recomp.h, so linking these definitions instead of spu_channels.c is
 * how we plug in the test harness.
 */

#include "spu_recomp.h"
#include "spu_helpers.h"   /* for spu_zero() */
#include <stdio.h>
#include <stdint.h>

/* Required by spu_dma.h (referenced via spu_helpers.h chain). Unused here. */
uint8_t* vm_base = 0;

/* ---- harness state ---- */
static const uint32_t kInputs[] = { 10, 20, 30, 40 };
static const int      kNumInputs = (int)(sizeof(kInputs) / sizeof(kInputs[0]));
static int            g_in_idx = 0;
static uint32_t       g_out = 0;
static int            g_wrote = 0;

/* ---- channel overrides ---- */
u128 spu_rdch(spu_context* ctx, uint32_t channel) {
    (void)ctx;
    uint32_t v = 0;
    if (channel == SPU_RdInMbox) {
        if (g_in_idx < kNumInputs) v = kInputs[g_in_idx++];
        else { fprintf(stderr, "FAIL: rdch underflow\n"); return spu_zero(); }
    }
    u128 r = spu_zero(); r._u32[0] = v; return r;
}
uint32_t spu_rchcnt(spu_context* ctx, uint32_t channel) {
    (void)ctx; (void)channel; return 1;
}
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value) {
    (void)ctx;
    if (channel == SPU_WrOutMbox) { g_out = value._u32[0]; g_wrote = 1; }
}

/* The lifter emits these as externs; this test has no indirect branches
 * so the bodies are unreachable, but the symbols must exist. */
void spu_indirect_branch(spu_context* ctx) {
    (void)ctx;
    fprintf(stderr, "FAIL: unexpected spu_indirect_branch\n");
}
void spu_register_function(uint32_t addr, void (*fn)(spu_context*)) {
    (void)addr; (void)fn;   /* not used in this test */
}

int main(void) {
    spu_context ctx;
    spu_context_init(&ctx, 0);
    spu_func_00000000(&ctx);

    uint32_t expected = 0;
    for (int i = 0; i < kNumInputs; i++) expected += kInputs[i];

    int ok = 1;
    if (!g_wrote)        { printf("FAIL: no wrch to outbound mailbox\n"); ok = 0; }
    if (g_in_idx != kNumInputs)
        { printf("FAIL: consumed %d/%d inbound values\n", g_in_idx, kNumInputs); ok = 0; }
    if (g_out != expected)
        { printf("FAIL: outbound = %u (expected %u)\n", g_out, expected); ok = 0; }

    if (ok) {
        printf("OK: consumed %d values, sum = %u (matches %u)\n",
               g_in_idx, g_out, expected);
    }
    return ok ? 0 : 1;
}
