/*
 * A SPURS job's first stack frame must not overlap its cache buffer
 * (runtime/spu/spurs_job.c).
 *
 * spu_run_spurs_job lays the job's memory out as binary, I/O, output,
 * scratch + stack, then the cache-hinted read-only inputs. The stack top is
 * therefore the first byte of cacheBuffer[0]. Under the SPU ABI a function
 * saves its link register in its CALLER's frame, at 16($sp), so the job's
 * entry runs "stqd $lr, 16($sp)" first thing. With $sp at stack_top - 16 that
 * quadword is the first 16 bytes of the cache buffer.
 *
 * Twisted Metal's Edge geometry jobs keep their viewport (x, y, width,
 * height) there. After the save it read x = 0x3FF00 (the return address the
 * runner puts in $lr) and width = height = 0, so the screen-space scale was
 * zero, every triangle of a skinned mesh had zero area, and the job culled
 * them all: the character in the main menu was never drawn.
 *
 * The job here is a C entry function that does what a lifted job's prologue
 * does, then checks that its cache buffer still holds what the cache DMA list
 * pointed at, and that it can push one more frame without leaving its stack.
 *
 * Build:
 *   clang -std=gnu17 -I include -I runtime/spu -I runtime/platform \
 *         -o /tmp/test_spurs_job_stack \
 *         runtime/spu/tests/test_spurs_job_stack.c \
 *         runtime/spu/spu_drain.c runtime/spu/spu_lockstep.c \
 *         runtime/spu/spu_coherency.c runtime/spu/spu_workload.c \
 *         runtime/spu/spurs_policy.c runtime/spu/spurs_job.c \
 *         runtime/spu/spu_tsp_weak.c runtime/spu/spu_vm_pagemap.c \
 *         runtime/spu/spurs_policy_blob_weak.c runtime/spu/spu_interp.c \
 *         runtime/platform/win32_compat.c -lpthread -lm
 *
 * Exit code: 0 if all passed, 1 if any failed. Final line prints a summary.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Guest memory: the runner copies the binary, descriptor and cache inputs
 * out of it. */
static uint8_t g_guest_mem[1u << 20];
uint8_t* vm_base = g_guest_mem;
uint32_t ppu_vm_size = 0;

/* spu_run_with_halt and the channel layer the runner needs. */
#include "../spu_channels.c"
#include "spu_workload.h"   /* spu_run_spurs_job */

static int g_pass, g_fail;
static const char* g_current = "(none)";

#define TEST(name) g_current = (name)

#define CHECK(cond) do {                                          \
    if (cond) { g_pass++; }                                       \
    else {                                                        \
        g_fail++;                                                 \
        fprintf(stderr, "FAIL: %s\n  at %s:%d: %s\n",             \
                g_current, __FILE__, __LINE__, #cond);            \
    }                                                             \
} while (0)

#define JOB_EA     0x00010000u   /* CellSpursJob256 */
#define BIN_EA     0x00020000u   /* job binary (never executed here) */
#define CACHE_EA   0x00030000u   /* cache-hinted input */
#define BIN_SIZE   0x400u
#define CACHE_SIZE 0x80u

static void put32(uint32_t ea, uint32_t v)
{
    g_guest_mem[ea] = (uint8_t)(v >> 24); g_guest_mem[ea + 1] = (uint8_t)(v >> 16);
    g_guest_mem[ea + 2] = (uint8_t)(v >> 8); g_guest_mem[ea + 3] = (uint8_t)v;
}
static void put16(uint32_t ea, uint16_t v)
{
    g_guest_mem[ea] = (uint8_t)(v >> 8); g_guest_mem[ea + 1] = (uint8_t)v;
}
static uint32_t ls_get32(const spu_context* ctx, uint32_t lsa)
{
    const uint8_t* p = ctx->ls + (lsa & SPU_LS_MASK);
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* A job with no I/O, the default stack and one 128-byte cache input. */
static void build_job(uint16_t stack_qw, uint16_t scratch_qw)
{
    memset(g_guest_mem, 0, sizeof g_guest_mem);
    put32(JOB_EA + 0x00, 0);                    /* eaBinary, high word */
    put32(JOB_EA + 0x04, BIN_EA);
    put16(JOB_EA + 0x08, BIN_SIZE >> 4);        /* sizeBinary, quadwords */
    put16(JOB_EA + 0x0A, 0);                    /* no input DMA list */
    put16(JOB_EA + 0x1C, stack_qw);             /* sizeStack, quadwords */
    put16(JOB_EA + 0x1E, scratch_qw);           /* sizeScratch, quadwords */
    put32(JOB_EA + 0x24, 8);                    /* one cache DMA entry */
    put32(JOB_EA + 0x30, CACHE_SIZE);           /* entry: size ... */
    put32(JOB_EA + 0x34, CACHE_EA);             /* ... and address */
    for (uint32_t i = 0; i < CACHE_SIZE; i += 4)
        put32(CACHE_EA + i, 0xC0DE0000u | i);
}

/* What the entry saw. */
static int      g_ran, g_cache_intact, g_frames_in_stack;
static uint32_t g_cache_ls, g_sp;

/* The prologue every SPU function starts with: save $lr in the caller's
 * frame, then push a frame of its own and store the back chain. */
static void job_entry(spu_context* ctx)
{
    const uint32_t sp = ctx->gpr[1]._u32[0];
    g_ran = 1;
    g_sp = sp;
    memcpy(ctx->ls + ((sp + 16u) & SPU_LS_MASK), &ctx->gpr[0], 16);   /* stqd $lr,16($sp) */
    const uint32_t nsp = sp - 0x40u;                                   /* stqu $sp,-64($sp) */
    memcpy(ctx->ls + (nsp & SPU_LS_MASK), &ctx->gpr[1], 16);

    /* r3 = CellSpursJobContext2*; cacheBuffer[0] is at +4. */
    g_cache_ls = ls_get32(ctx, ctx->gpr[3]._u32[0] + 4u);
    g_cache_intact = g_cache_ls != 0;
    for (uint32_t i = 0; g_cache_intact && i < CACHE_SIZE; i += 4)
        if (ls_get32(ctx, g_cache_ls + i) != (0xC0DE0000u | i)) g_cache_intact = 0;
    /* Both frames, and the caller's save area above the first, lie below the
     * cache buffer. */
    g_frames_in_stack = sp + 32u <= g_cache_ls && nsp < sp;
}

static void run_case(const char* name, uint16_t stack_qw, uint16_t scratch_qw)
{
    TEST(name);
    build_job(stack_qw, scratch_qw);
    g_ran = g_cache_intact = g_frames_in_stack = 0;
    spu_run_spurs_job(job_entry, 7, JOB_EA, 256);
    CHECK(g_ran);
    CHECK(g_cache_intact);
    CHECK(g_frames_in_stack);
    if (!g_cache_intact || !g_frames_in_stack)
        fprintf(stderr, "  $sp=0x%05X cacheBuffer[0]=0x%05X\n", g_sp, g_cache_ls);
}

int main(void)
{
    run_case("default stack (sizeStack 0)", 0, 0);
    run_case("explicit stack, no scratch", 0x100, 0);
    run_case("stack and scratch", 0x80, 0x40);

    printf("spurs job stack: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * The runtime beyond the job runner and the SPU channel layer, stubbed as in
 * test_spu_taskset_wake.c. None of it is on the path this test drives.
 * -----------------------------------------------------------------------*/
void ppu_resv_break_store(uint64_t ea)                 { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w)
{
    (void)a; (void)v; (void)w;
}
unsigned long long ps3_ms_now(void)                    { return 0; }
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi = 0;
int      g_resv_store_active = 0;
uint32_t g_barrier_sync_watch = 0;

void (*g_spurs_kernel_hook)(uint32_t) = 0;
uint32_t g_ydkj_spurs_ctx_ea = 0;
uint32_t g_ydkj_real_spurs_ea = 0;
uint32_t g_ydkj_real_taskset_ea = 0;
uint32_t g_ydkj_real_taskid = 0;
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; }
void ydkj_wake_all_event_flags(void)                   { }
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d)
{
    (void)c; (void)a; (void)b; (void)d;
}

uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
void spu_thread_publish_ctx(uint32_t tid, void* ctx) { (void)tid; (void)ctx; }
