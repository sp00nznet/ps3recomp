/* ILH must replicate the immediate in every 16-bit lane. Real interpreter
 * bytes plus literal full-vector expectations; no title assets are included. */
#include "../spu_helpers.h"
#include "../spu_interp.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* CPU-only instructions never reach these external channel/diagnostic hooks. */
u128 spu_rdch(spu_context* c, uint32_t ch) { (void)c; (void)ch; abort(); }
void spu_wrch(spu_context* c, uint32_t ch, u128 v) { (void)c; (void)ch; (void)v; abort(); }
uint32_t spu_rchcnt(spu_context* c, uint32_t ch) { (void)c; (void)ch; abort(); }
int g_spu_ls_watch_n, g_spu_ls_probe, g_wws_read_probe, g_wws_code_probe, g_spu_smc_watch;
spu_lifted_fn spu_lifted_lookup(const spu_context* c, uint32_t a) { (void)c; (void)a; abort(); }
void spu_spurs_taskset_syscall(spu_context* c) { (void)c; abort(); }
void spu_ls_watch_slow(uint32_t l, int w, const uint8_t* p, uint32_t pc, uint32_t lr) {
    (void)l; (void)w; (void)p; (void)pc; (void)lr; abort();
}

static void be32(uint8_t* p, uint32_t value) {
    p[0] = value >> 24; p[1] = value >> 16; p[2] = value >> 8; p[3] = value;
}
static spu_context ctx;
static int run(const char* name, uint32_t opcode, uint32_t want, u128 lifted) {
    memset(&ctx, 0, sizeof(ctx));
    spu_context_init(&ctx, 0);
    ctx.image_id = -1; /* actual pure interpretation, no registry shortcut */
    memset(&ctx.gpr[80], 0xA5, sizeof(u128));
    memset(&ctx.gpr[79], 0x5A, sizeof(u128));
    be32(ctx.ls + 0x100, opcode);
    be32(ctx.ls + 0x104, 0); /* stop 0 */
    spu_interp_run(&ctx, 0x100);
    assert(ctx.status == SPU_STATUS_STOPPED_BY_STOP && ctx.pc == 0x108);
    const u128 expected = { ._u32 = {want, want, want, want} };
    /* Check all 16 bytes against literal independently derived expectations,
     * then against the existing helper used by lifted instruction emission. */
    int ok = !memcmp(&ctx.gpr[80], &expected, 16) && !memcmp(&ctx.gpr[80], &lifted, 16);
    for (unsigned i = 0; i < 16; ++i) assert(ctx.gpr[79]._u8[i] == 0x5A);
    printf("%s opcode=%08X got=%08X:%08X:%08X:%08X expected=%08X %s\n", name, opcode,
           ctx.gpr[80]._u32[0], ctx.gpr[80]._u32[1], ctx.gpr[80]._u32[2],
           ctx.gpr[80]._u32[3], want, ok ? "PASS" : "FAIL");
    return !ok;
}
int main(void) {
    static const struct { uint16_t imm; uint32_t ilh, il, ilhu; } cases[] = {
        {0x0000, 0x00000000, 0x00000000, 0x00000000},
        {0x0001, 0x00010001, 0x00000001, 0x00010000},
        {0x7FFF, 0x7FFF7FFF, 0x00007FFF, 0x7FFF0000},
        {0x8000, 0x80008000, 0xFFFF8000, 0x80000000},
        {0xBEEF, 0xBEEFBEEF, 0xFFFFBEEF, 0xBEEF0000},
        {0xFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFF0000},
    };
    /* Exact observed retail instruction: ilh $80,0xBEEF. */
    int failed = run("ILH/native-cookie", 0x41DF77D0u, 0xBEEFBEEFu, spu_ilh(0xBEEF));
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        uint16_t imm = cases[i].imm;
        uint32_t operand = ((uint32_t)imm << 7) | 80u;
        failed += run("ILH", 0x41800000u | operand, cases[i].ilh, spu_ilh(imm));
        failed += run("IL", 0x40800000u | operand, cases[i].il, spu_il((int16_t)imm));
        failed += run("ILHU", 0x41000000u | operand, cases[i].ilhu, spu_ilhu(imm));
    }
    printf("SPU immediate loads: %d failures across 19 checks\n", failed);
    return failed ? 1 : 0;
}
