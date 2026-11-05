/* SPU_DIRECT (spu_context.h): a lifted cross-function branch with a known
 * target tail-calls it when the drain loop has nothing to do on that step,
 * and takes the trampoline otherwise. The functions below are written the way
 * the lifter emits them. Built and run in each mode by tools/test_spu_direct.py.
 *
 *   direct      two functions branch to each other 2,000,000 times: only the
 *               run's first branch takes a drain step, and the host stack does
 *               not grow (a plain call per branch would need far more than a
 *               thread's stack)
 *   off         the same with PS3_SPU_DIRECT=0: one drain step per branch
 *   irq         an SPU interrupt raised mid-chain is taken at the next branch
 *   reset-hint  a branch to a registered stack-reset entry takes a drain step
 *   ls0         a branch to LS 0 takes a drain step, which ends the job there
 *   nested      after a nested spu_drain_call returns, drain_ret_pc is the
 *               enclosing drain's return pc again, not 0 */
#include "../spu_helpers.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
extern void spu_begin_image(int);
extern void spu_register_function(uint32_t, void (*)(spu_context*));
extern void spu_register_stack_reset_entry(uint32_t, int);
extern int spu_run_with_halt(void (*)(spu_context*), spu_context*);
extern void spu_halt(spu_context*);

static spu_context ctx;
static uint32_t s_loops = 2000000;
static int s_raise_at = -1;            /* irq: loop count that raises the event */

static void finish(spu_context* c)
{
    c->status = SPU_STATUS_STOPPED_BY_STOP;
    spu_halt(c);
}
static void wrong(spu_context* c)      /* must never run */
{
    c->gpr[21]._u32[0]++;
    finish(c);
}
static void handler(spu_context* c)    /* interrupt handler at 0x300 */
{
    c->gpr[23]._u32[0] = c->gpr[10]._u32[0];
    finish(c);
}

static void func_b(spu_context* c);
static void func_a(spu_context* c)     /* at 0x240 */
{
    c->gpr[10] = spu_ai(c->gpr[10], 1);
    if (c->gpr[10]._u32[0] == (uint32_t)s_raise_at) c->event_status |= 0x20u;
    if (c->gpr[10]._u32[0] >= s_loops) { SPU_DIRECT(c, 0x380, finish); }
    SPU_DIRECT(c, 0x280, func_b);
}
static void func_b(spu_context* c)     /* at 0x280 */
{
    SPU_DIRECT(c, 0x240, func_a);
}
static void to_ls0(spu_context* c)     /* at 0x2C0 */
{
    c->gpr[10] = spu_ai(c->gpr[10], 1);
    SPU_DIRECT(c, 0x0, wrong);
}
static void to_ls0_entry(spu_context* c)
{
    SPU_DIRECT(c, 0x2C0, to_ls0);
}

/* nested: outer -> (drain, return pc 0x204) mid -> (drain, return pc 0x208) leaf */
static void leaf(spu_context* c)
{
    SPU_RET(c);
}
static void mid(spu_context* c)
{
    c->gpr[0] = spu_link(0x208);
    c->host_depth++;
    c->pc = 0x200;
    g_spu_trampoline_fn = leaf;
    spu_drain_call(c, 0x208);
    c->host_depth--;
    c->gpr[22]._u32[0] = c->drain_ret_pc;   /* back in the 0x204 drain */
    c->gpr[0] = spu_link(0x204);
    SPU_RET(c);
}
static void outer(spu_context* c)
{
    c->gpr[0] = spu_link(0x204);
    c->host_depth++;
    c->pc = 0x2C0;
    g_spu_trampoline_fn = mid;
    spu_drain_call(c, 0x204);
    c->host_depth--;
    finish(c);
}

int main(int argc, char** argv)
{
    assert(argc == 2);
    const char* mode = argv[1];
    spu_context_init(&ctx, 0);
    ctx.image_id = 77;
    ctx.policy_mode = 0;
    ctx.status = SPU_STATUS_RUNNING;
    spu_begin_image(77);
    spu_register_function(0x240, func_a);
    spu_register_function(0x280, func_b);
    spu_register_function(0x300, handler);
    spu_register_function(0x380, finish);
    void (*entry)(spu_context*) = func_a;
    if (!strcmp(mode, "irq")) {
        s_loops = 100000; s_raise_at = 1000;
        ctx.ls[0] = 0x30; ctx.ls[1] = 0x00; ctx.ls[2] = 0x60; ctx.ls[3] = 0x00;   /* bra 0x300 */
        ctx.int_enable = 1;
        ctx.event_mask = 0x20u;
    } else if (!strcmp(mode, "reset-hint")) {
        s_loops = 100000;
        spu_register_stack_reset_entry(0x280, 77);
    } else if (!strcmp(mode, "ls0")) {
        entry = to_ls0_entry;
    } else if (!strcmp(mode, "nested")) {
        entry = outer;
    } else if (strcmp(mode, "direct") && strcmp(mode, "off")) {
        fprintf(stderr, "unknown mode\n");
        return 2;
    }
    const int halted = spu_run_with_halt(entry, &ctx);
    printf("%s: halted=%d status=%X loops=%u steps=%u wrong=%u irq-at=%u ret-pc=0x%X tailcalls=%d\n",
           mode, halted, ctx.status, ctx.gpr[10]._u32[0], ctx.steps, ctx.gpr[21]._u32[0],
           ctx.gpr[23]._u32[0], ctx.gpr[22]._u32[0],
#ifdef SPU_DIRECT_TAILCALLS
           1
#else
           0
#endif
           );
    fflush(stdout);
    assert(halted == 1 && ctx.gpr[21]._u32[0] == 0);
#ifdef SPU_DIRECT_TAILCALLS
    const int direct = g_spu_direct > 0;
#else
    const int direct = 0;
#endif
    if (!strcmp(mode, "direct") || !strcmp(mode, "off")) {
        assert(ctx.gpr[10]._u32[0] == s_loops);
        if (!strcmp(mode, "off")) assert(g_spu_direct == 0);
        /* The run's first transfer is always a step (spu_context.h). */
        if (direct) assert(ctx.steps == 1);
        else        assert(ctx.steps == 2 * s_loops - 1);
    } else if (!strcmp(mode, "irq")) {
        assert(ctx.gpr[23]._u32[0] == (uint32_t)s_raise_at);   /* the very next branch */
        if (direct) assert(ctx.steps == 2);
    } else if (!strcmp(mode, "reset-hint")) {
        assert(ctx.gpr[10]._u32[0] == s_loops);
        if (direct) assert(ctx.steps == s_loops - 1);   /* each a -> b; b -> a stays direct */
    } else if (!strcmp(mode, "ls0")) {
        assert(ctx.gpr[10]._u32[0] == 1 && ctx.steps == 2);
    } else if (!strcmp(mode, "nested")) {
        assert(ctx.gpr[22]._u32[0] == 0x204);
    }
    puts("PASS");
    return 0;
}

/* Link-only boundaries outside this CPU-only fixture. Fail if any is reached. */
void spu_thread_publish_ctx(uint32_t tid, void* c) { (void)tid; (void)c; abort(); }
void ppu_resv_break_store(uint64_t ea) { (void)ea; abort(); }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; abort(); }
unsigned long long ps3_ms_now(void) { abort(); }
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi;
int g_resv_store_active;
uint32_t g_barrier_sync_watch;
uint32_t g_ydkj_real_spurs_ea, g_ydkj_real_taskset_ea, g_ydkj_real_taskid;
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; abort(); }
void ydkj_wake_all_event_flags(void) { abort(); }
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d) {
    (void)c; (void)a; (void)b; (void)d; abort();
}
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
