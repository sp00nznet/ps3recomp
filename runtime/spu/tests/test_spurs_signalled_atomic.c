/*
 * SPURS taskset `signalled` bitset: the host's read-modify-write of a task's
 * bit against an SPU commit to the same line (runtime/spu/spu_workload.c).
 *
 * A task's signal is one bit in the taskset's signalled bitset (+0x40). The
 * host sets it in spu_taskset_signal_task and clears it when the task consumes
 * it in spu_taskset_wait_signal. SPU tasks set bits in the same word with
 * GETLLAR/PUTLLC, under the lock-line lock: a task signalling another, or the
 * last task to reach a cellSpursBarrier releasing the others. If the host reads
 * the word outside that lock and writes it back inside it, an SPU commit landing
 * between the two is overwritten with the stale word and that task never wakes.
 * Twisted Metal's post-FX barrier lost a wake exactly so and stopped for good.
 *
 * The interleaving is built with the lock rather than raced for, as in
 * test_spu_lockline_peer.c. The line is reserved by a peer SPU, so a host store
 * to it is a coherent store that takes the lock-line lock. The main thread takes
 * the lock, a second thread starts the host operation and is held at the lock,
 * the SPU commit is applied under the lock, and only then is the host operation
 * let through. Afterwards both bits must be right: the host's own change and
 * the SPU's commit.
 *
 * spu_channels.c is compiled in, as in the other SPU fixtures, for the symbols
 * spu_workload.c needs.
 *
 * Build:
 *   cc -std=gnu17 -I include -I runtime/spu -I runtime/platform \
 *      -o /tmp/test_spurs_signalled_atomic \
 *      runtime/spu/tests/test_spurs_signalled_atomic.c \
 *      runtime/spu/spu_drain.c runtime/spu/spu_lockstep.c \
 *      runtime/spu/spu_coherency.c runtime/spu/spu_workload.c \
 *      runtime/spu/spurs_policy.c runtime/spu/spurs_job.c \
 *      runtime/spu/spu_tsp_weak.c runtime/spu/spu_vm_pagemap.c \
 *      runtime/spu/spurs_policy_blob_weak.c runtime/spu/spu_interp.c \
 *      runtime/platform/win32_compat.c -lpthread -lm
 *
 * Exit code: 0 if all passed, 1 if any failed. Final line prints a summary.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint8_t g_guest_mem[1u << 20];
uint8_t* vm_base = g_guest_mem;
uint32_t ppu_vm_size = 0;

#include "../spu_channels.c"

/* The runtime beyond the SPU layer, stubbed as in test_spu_mfc_slots.c. None
 * of it is on the path this test drives. */
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

int  spu_taskset_wait_signal(uint32_t taskset_ea, uint32_t taskId);
void spu_taskset_signal_task(uint32_t taskset_ea, uint32_t taskId);

static int g_pass, g_fail;
static const char* g_current = "(none)";

#define TEST(name) g_current = (name)

#define CHECK_EQ_U32(actual, expected) do {                       \
    uint32_t _a = (uint32_t)(actual), _e = (uint32_t)(expected);  \
    if (_a == _e) { g_pass++; }                                   \
    else {                                                        \
        g_fail++;                                                 \
        fprintf(stderr, "FAIL: %s\n  at %s:%d: %s\n"              \
                        "  actual   0x%08X\n  expected 0x%08X\n", \
                g_current, __FILE__, __LINE__, #actual, _a, _e);  \
    }                                                             \
} while (0)

#define TASKSET   0x00030000u                 /* 128-byte aligned */
#define SIG_WORD  (TASKSET + 0x40u)           /* signalled bitset, tasks 0..31 */
#define BIT(t)    ((1u << 31) >> (t))

static spu_context g_peer;                    /* holds the reservation */

static uint32_t rd_be32(uint32_t ea)
{
    const uint8_t* p = g_guest_mem + ea;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void wr_be32(uint32_t ea, uint32_t v)
{
    uint8_t* p = g_guest_mem + ea;
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void* do_signal_task4(void* arg)  { (void)arg; spu_taskset_signal_task(TASKSET, 4); return NULL; }
static void* do_wait_task3(void* arg)    { (void)arg; spu_taskset_wait_signal(TASKSET, 3); return NULL; }

/* The host operation runs on its own thread while the main thread holds the
 * lock-line lock; the SPU commit (`spu_bit` set in the word) is applied under
 * the lock, then the lock is released and the host operation completes. */
static void interleave(void* (*host_op)(void*), uint32_t spu_bit)
{
    pthread_t t;
    spu_lockline_lock();
    pthread_create(&t, NULL, host_op, NULL);
    sleep_ms(100);                            /* the host op reaches the lock */
    wr_be32(SIG_WORD, rd_be32(SIG_WORD) | spu_bit);   /* the SPU's PUTLLC */
    spu_lockline_unlock();
    pthread_join(t, NULL);
}

int main(void)
{
    /* A peer SPU has the bitset line reserved, as a task in GETLLAR does. */
    spu_lockline_lock();
    spu_coh_reserve(&g_peer, SIG_WORD);
    spu_lockline_unlock();

    TEST("host signal vs SPU commit: neither bit is lost");
    wr_be32(SIG_WORD, 0);
    interleave(do_signal_task4, BIT(3));
    CHECK_EQ_U32(rd_be32(SIG_WORD), BIT(3) | BIT(4));

    TEST("host consume vs SPU commit: the other task's bit survives");
    wr_be32(SIG_WORD, BIT(3));                /* task 3 has a pending signal */
    interleave(do_wait_task3, BIT(4));
    CHECK_EQ_U32(rd_be32(SIG_WORD), BIT(4));

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
