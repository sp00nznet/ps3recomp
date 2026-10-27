/*
 * SPURS taskset ready/enabled bitsets: the host's updates against a task exit
 * (or an SPU commit) changing the same line (libs/spurs/spurs_taskset.c).
 *
 * cellSpursCreateTask found a free task id by reading the enabled bitset (+0x30)
 * and later set the enabled and ready bits (+0x10) with spurs_bitset_set: a
 * vm_read32 outside the lock-line lock and a vm_write32 inside it. The host's
 * task exit clears the exiting task's enabled bit, and SPU tasks update the same
 * line with GETLLAR/PUTLLC. A clear landing between the read and the write was
 * undone: the exited task's enabled bit came back with no task behind it, and
 * cellSpursJoinTask2 on that id never returned (Killzone 3: Havok's thread-pool
 * destructor at quit to menu).
 *
 * The interleaving is built with the lock, as in test_spurs_signalled_atomic.c:
 * a peer SPU has the line reserved (so a host store takes the lock-line lock),
 * the main thread holds the lock, a second thread starts the host operation and
 * is held at the lock, the exit's clear is applied under the lock, and only then
 * is the host operation let through.
 *
 * Build:
 *   cc -std=gnu17 -I include -I runtime/spu -I runtime/platform \
 *      -o /tmp/test_spurs_taskset_bits_atomic \
 *      runtime/spu/tests/test_spurs_taskset_bits_atomic.c libs/spurs/spurs_taskset.c \
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

/* libs/spurs/spurs_taskset.c (its header's PM prototypes clash with the stubs above). */
void     spurs_taskset_add_task(uint32_t taskset_ea, uint32_t taskId, uint64_t elf_ea, uint64_t context,
                                const uint32_t arg[4], const uint32_t ls_pattern[4]);
uint32_t spurs_taskset_take_slot(uint32_t taskset_ea, uint32_t max_tasks);

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

#define TASKSET     0x00030000u               /* 128-byte aligned */
#define READY_WORD  (TASKSET + 0x10u)         /* ready bitset, tasks 0..31 */
#define EN_WORD     (TASKSET + 0x30u)         /* enabled bitset, tasks 0..31 */
#define BIT(t)      ((1u << 31) >> (t))

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

static uint32_t g_slot;
static void* do_add_task2(void* arg)
{
    (void)arg;
    spurs_taskset_add_task(TASKSET, 2, 0x00040000u, 0, NULL, NULL);
    return NULL;
}
static void* do_take_slot(void* arg)  { (void)arg; g_slot = spurs_taskset_take_slot(TASKSET, 128); return NULL; }

/* The host operation runs on its own thread while the main thread holds the
 * lock-line lock; task 0's exit (its enabled bit cleared) is applied under the
 * lock, then the lock is released and the host operation completes. */
static void interleave(void* (*host_op)(void*))
{
    pthread_t t;
    spu_lockline_lock();
    pthread_create(&t, NULL, host_op, NULL);
    sleep_ms(100);                            /* the host op reaches the lock */
    wr_be32(EN_WORD, rd_be32(EN_WORD) & ~BIT(0));     /* task 0 exits */
    spu_lockline_unlock();
    pthread_join(t, NULL);
}

static void reserve_line(void)
{
    /* A peer SPU has the bitset line reserved, as a task in GETLLAR does. A
     * host write drops every reservation on the line, so take it again. */
    spu_lockline_lock();
    spu_coh_reserve(&g_peer, EN_WORD);
    spu_lockline_unlock();
}

int main(void)
{
    TEST("add_task vs task exit: the exited task's enabled bit stays clear");
    wr_be32(EN_WORD, BIT(0) | BIT(1));
    wr_be32(READY_WORD, 0);
    reserve_line();
    interleave(do_add_task2);
    CHECK_EQ_U32(rd_be32(EN_WORD), BIT(1) | BIT(2));
    CHECK_EQ_U32(rd_be32(READY_WORD), BIT(2));

    TEST("take_slot vs task exit: the freed id is taken, the others kept");
    wr_be32(EN_WORD, BIT(0) | BIT(1));
    reserve_line();
    interleave(do_take_slot);
    CHECK_EQ_U32(g_slot, 0);
    CHECK_EQ_U32(rd_be32(EN_WORD), BIT(0) | BIT(1));

    TEST("take_slot: the lowest clear bit, then none when all are taken");
    memset(g_guest_mem + EN_WORD, 0xFF, 16);
    g_guest_mem[EN_WORD + 5] = 0xEF;          /* task 43 free */
    CHECK_EQ_U32(spurs_taskset_take_slot(TASKSET, 128), 43);
    CHECK_EQ_U32(spurs_taskset_take_slot(TASKSET, 128), 128);

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
