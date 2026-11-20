/* Encoded ROTHMI must logically shift each of eight halfwords independently.
 * Literal full-vector results catch missing dispatch, word-sized/arithmetic
 * shifts, halfword order mistakes, mask-width errors and alias clobbering. */
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
static void run(uint32_t opcode, const uint32_t input[4], const uint32_t expected[4], int alias) {
    unsigned ra = (opcode >> 7) & 127, rt = alias ? ra : opcode & 127;
    memset(&ctx,0,sizeof(ctx)); spu_context_init(&ctx,0); ctx.image_id=-1;
    memcpy(ctx.gpr[ra]._u32,input,16);
    const uint32_t sentinel[4]={0x11223344,0x55667788,0x99AABBCC,0xDDEEFF00};
    memcpy(ctx.gpr[100]._u32,sentinel,16);
    be32(ctx.ls+0x100,(opcode & ~127u) | rt); be32(ctx.ls+0x104,0);
    spu_interp_run(&ctx,0x100);
    if(ctx.status!=SPU_STATUS_STOPPED_BY_STOP || ctx.pc!=0x108) {
        fprintf(stderr,"ROTHMI %08X alias=%d stopped at %X status=%d\n",opcode,alias,ctx.pc,ctx.status);
        abort();
    }
    if(memcmp(ctx.gpr[rt]._u32,expected,16)) {
        fprintf(stderr,"ROTHMI %08X alias=%d got %08X:%08X:%08X:%08X expected %08X:%08X:%08X:%08X\n",
                opcode,alias,ctx.gpr[rt]._u32[0],ctx.gpr[rt]._u32[1],ctx.gpr[rt]._u32[2],ctx.gpr[rt]._u32[3],
                expected[0],expected[1],expected[2],expected[3]);
        abort();
    }
    if(!alias) assert(!memcmp(ctx.gpr[ra]._u32,input,16));
    assert(!memcmp(ctx.gpr[100]._u32,sentinel,16));
}
int main(void) {
    /* Halfwords with sign bits, all ones, low bits and eight distinct patterns. */
    const uint32_t input[4]={0x8001FFFF,0x7FFF4000,0xF00F00FF,0x1234ABCD};
    static const struct { int imm; uint32_t expected[4]; } cases[]={
        {0,{0x8001FFFF,0x7FFF4000,0xF00F00FF,0x1234ABCD}},
        {-1,{0x40007FFF,0x3FFF2000,0x7807007F,0x091A55E6}},
        {-7,{0x010001FF,0x00FF0080,0x01E00001,0x00240157}},
        {-15,{0x00010001,0,0x00010000,0x00000001}},
        {-16,{0,0,0,0}},
        {-17,{0,0,0,0}},
        {-31,{0,0,0,0}},
        {-32,{0x8001FFFF,0x7FFF4000,0xF00F00FF,0x1234ABCD}},
        {-33,{0x40007FFF,0x3FFF2000,0x7807007F,0x091A55E6}},
        {-63,{0,0,0,0}},
        {-64,{0x8001FFFF,0x7FFF4000,0xF00F00FF,0x1234ABCD}},
        {1,{0,0,0,0}},
        {16,{0,0,0,0}},
        {17,{0x00010001,0,0x00010000,0x00000001}},
        {31,{0x40007FFF,0x3FFF2000,0x7807007F,0x091A55E6}},
        {32,{0x8001FFFF,0x7FFF4000,0xF00F00FF,0x1234ABCD}},
        {63,{0x40007FFF,0x3FFF2000,0x7807007F,0x091A55E6}},
    };
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        uint32_t opcode=0x0FA00000u|(((uint32_t)cases[i].imm&127u)<<14)|(8u<<7)|7u;
        run(opcode,input,cases[i].expected,0);
        run(opcode,input,cases[i].expected,1);
    }
    puts("PASS: 34 encoded ROTHMI vectors, eight halfwords, mask boundaries, and alias");
    return 0;
}
