/* spu_selfmod_selftest.c — closes the loop on self-modifying SPUs.
 *
 * Test 1 (interpreter executes freshly-written code): a program stores an
 * instruction word into local store, then branches to it. The interpreter,
 * which fetches every instruction from live LS, runs the synthesized code —
 * exactly the spu_0004 `stqd; sync; bi $reg` trampoline pattern.
 *
 * Test 2 (runtime write-watch -> eviction): a store via spu_ls_write128 into a
 * REGISTERED function's body fires spu_code_write_watch -> spu_invalidate, so
 * the stale compiled entry is evicted and the next lookup misses (-> interpret).
 */
#include "../spu_interp.h"
#include "../spu_fn_registry.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* No channel ops here; stub the ABI so we link without the MFC world. */
u128 spu_rdch(spu_context* c, uint32_t ch) { (void)c;(void)ch; u128 z; memset(&z,0,sizeof z); return z; }
void spu_wrch(spu_context* c, uint32_t ch, u128 v) { (void)c;(void)ch;(void)v; }
uint32_t spu_rchcnt(spu_context* c, uint32_t ch) { (void)c;(void)ch; return 1; }

static void dummy_fn(spu_context* c) { (void)c; }

int main(void) {
    /* ---- Test 1: self-modifying execution through the interpreter ---- */
    static spu_context ctx;
    memset(&ctx, 0, sizeof ctx);
    /* Program at LS 0:  stqd $2,0($3) ; bi $4   (bytes, big-endian) */
    ctx.ls[0]=0x24; ctx.ls[1]=0x00; ctx.ls[2]=0x01; ctx.ls[3]=0x82;  /* stqd $2,0($3) */
    ctx.ls[4]=0x35; ctx.ls[5]=0x00; ctx.ls[6]=0x02; ctx.ls[7]=0x00;  /* bi $4         */
    /* $2 = the code to write: [ il $9,42 ; stop ; stop ; stop ] */
    ctx.gpr[2]._u32[0] = 0x40801509;  /* il $9,42 */
    ctx.gpr[2]._u32[1] = 0x00000000;  /* stop     */
    ctx.gpr[2]._u32[2] = 0x00000000;
    ctx.gpr[2]._u32[3] = 0x00000000;
    ctx.gpr[3]._u32[0] = 0x80;         /* store target LS 0x80 (initially zero) */
    ctx.gpr[4]._u32[0] = 0x80;         /* jump there and execute it            */

    assert(ctx.ls[0x80] == 0);         /* LS 0x80 is empty before the store */
    spu_interp_run(&ctx, 0);
    printf("selfmod: r9=%u (expect 42)  stop=0x%X\n", ctx.gpr[9]._u32[0], ctx.stop_code);
    assert(ctx.gpr[9]._u32[0] == 42);  /* the written instruction ran */
    assert(ctx.status == SPU_STATUS_STOPPED_BY_STOP);

    /* ---- Test 2: runtime store into lifted code -> eviction ---- */
    static spu_context wc;
    memset(&wc, 0, sizeof wc);
    wc.image_id = 1;
    spu_begin_image(1);
    spu_register_function(0x100, dummy_fn);   /* body [0x100,0x200) */
    spu_register_function(0x200, dummy_fn);
    spu_set_code_region(0x100, 0x300);        /* declare .text extent */
    assert(spu_lookup(0x100, 1) == dummy_fn);

    /* A normal SPU store into the middle of fn@0x100's body. This goes through
     * the SAME spu_ls_write128 the lifter emits -> the watch fires. */
    u128 v; memset(&v, 0xAB, sizeof v);
    spu_ls_write128(&wc, 0x140, v);
    assert(spu_lookup(0x100, 1) == NULL);     /* evicted by the write-watch */
    assert(spu_lookup(0x200, 1) == dummy_fn); /* neighbour untouched */
    printf("write-watch: fn@0x100 evicted after store to 0x140, fn@0x200 intact\n");

    /* And the interpreter's fast-path hook now misses on 0x100 -> would interpret. */
    assert(spu_lifted_lookup(&wc, 0x100) == NULL);

    printf("spu_selfmod_selftest: PASS\n");
    return 0;
}
