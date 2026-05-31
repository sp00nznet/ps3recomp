#include "spu_recomp.h"
#include "spu_helpers.h"
#include <stdio.h>
#include <stdint.h>

uint8_t* vm_base = 0;
static uint32_t g_out = 0;
static int g_wrote = 0;

u128 spu_rdch(spu_context* ctx, uint32_t ch) { (void)ctx; (void)ch; return spu_zero(); }
uint32_t spu_rchcnt(spu_context* ctx, uint32_t ch) { (void)ctx; (void)ch; return 1; }
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value) {
    (void)ctx;
    if (channel == SPU_WrOutMbox) { g_out = value._u32[0]; g_wrote = 1; }
}
void spu_indirect_branch(spu_context* ctx) {
    (void)ctx; fprintf(stderr, "FAIL: unexpected indirect branch\n");
}
void spu_register_function(uint32_t addr, void (*fn)(spu_context*)) { (void)addr; (void)fn; }

int main(void) {
    spu_context ctx;
    spu_context_init(&ctx, 0);
    spu_func_00000000(&ctx);

    const uint32_t kExpected = 0xFF80FF80u;
    if (!g_wrote)            { printf("FAIL: no wrch\n"); return 1; }
    if (g_out != kExpected)  { printf("FAIL: out = 0x%08X (expected 0x%08X)\n", g_out, kExpected); return 1; }
    printf("OK: shufb special-selector word = 0x%08X\n", g_out);
    return 0;
}
