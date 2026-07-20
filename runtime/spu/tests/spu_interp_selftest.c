/* spu_interp_selftest.c — end-to-end check of the SPU interpreter.
 *
 * Runs a snippet assembled by spu-lv2-as (bytes below) through the interpreter
 * and asserts the architectural results. Proves decode + execute + control flow
 * + local-store store, using the same helpers the lifter emits.
 *
 *   il $2,5; il $3,7; a $4,$2,$3; ai $4,$4,100; rotqbyi $5,$4,0;
 *   sf $6,$2,$3; stqd $4,0($1); stop
 */
#include "../spu_interp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* This snippet issues no channel ops; stub the channel ABI so the test links
 * without the full runtime (spu_channels.c pulls in unrelated globals). */
u128 spu_rdch(spu_context* c, uint32_t ch) { (void)c;(void)ch; u128 z; memset(&z,0,sizeof z); return z; }
void spu_wrch(spu_context* c, uint32_t ch, u128 v) { (void)c;(void)ch;(void)v; }
uint32_t spu_rchcnt(spu_context* c, uint32_t ch) { (void)c;(void)ch; return 1; }

/* .text of the assembled snippet (big-endian words, verbatim). */
static const unsigned char PROG[] = {
    0x40,0x80,0x02,0x82, 0x40,0x80,0x03,0x83, 0x18,0x00,0xc1,0x04,
    0x1c,0x19,0x02,0x04, 0x3f,0x80,0x02,0x05, 0x08,0x00,0xc1,0x06,
    0x24,0x00,0x00,0x84, 0x00,0x00,0x00,0x00,
};

int main(void) {
    static spu_context ctx;
    memset(&ctx, 0, sizeof ctx);
    memcpy(ctx.ls, PROG, sizeof PROG);
    ctx.gpr[1]._u32[0] = 0x100;   /* stack pointer (store target base) */

    spu_interp_run(&ctx, 0);

    #define P(i) (ctx.gpr[i]._u32[0])
    printf("r2=%u r3=%u r4=%u r5=%u r6=%u stop=0x%X\n",
           P(2), P(3), P(4), P(5), P(6), ctx.stop_code);

    assert(P(2) == 5);
    assert(P(3) == 7);
    assert(P(4) == 112);          /* 5+7+100 */
    assert(P(5) == 112);          /* rotqbyi 0 = identity */
    assert(P(6) == 2);            /* sf: b - a = 7 - 5 */
    assert(ctx.status == SPU_STATUS_STOPPED_BY_STOP);

    /* stqd $4,0($1): LS[0x100] holds 112 big-endian in the preferred word. */
    const unsigned char* q = &ctx.ls[0x100];
    unsigned w = ((unsigned)q[0]<<24)|((unsigned)q[1]<<16)|((unsigned)q[2]<<8)|q[3];
    assert(w == 112);

    /* --- backward-branch loop (the SPU counter-loop mechanism) via spu_dispatch --
     * il $2,0; il $3,5; loop: ai $2,$2,1; ceq $4,$2,$3; brz $4,loop; stop */
    static const unsigned char LOOP[] = {
        0x40,0x80,0x00,0x02, 0x40,0x80,0x02,0x83, 0x1c,0x00,0x41,0x02,
        0x78,0x00,0xc1,0x04, 0x20,0x7f,0xff,0x04, 0x00,0x00,0x00,0x00,
    };
    static spu_context lc;
    memset(&lc, 0, sizeof lc);
    memcpy(lc.ls, LOOP, sizeof LOOP);
    spu_dispatch(&lc, 0);                 /* enters via the computed-branch entry point */
    printf("loop r2=%u stop=0x%X\n", lc.gpr[2]._u32[0], lc.stop_code);
    assert(lc.gpr[2]._u32[0] == 5);       /* loop terminated at the bound */
    assert(lc.status == SPU_STATUS_STOPPED_BY_STOP);

    printf("spu_interp_selftest: PASS\n");
    return 0;
}
