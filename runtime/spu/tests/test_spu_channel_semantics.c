/*
 * SPU channel and atomic-command semantics that guest code relies on
 * (runtime/spu/spu_channels.c):
 *
 *   - a producer's spu_ch_wake is ordered after a reader that has tested its
 *     predicate but not yet gone to sleep, so a blocking read is never left to
 *     the 10 ms re-poll;
 *   - SPU_RdEventStat returns the events that are pending AND enabled;
 *   - SPU_WrSRR0 keeps a word-aligned local-store address;
 *   - GETLLAR / PUTLLC / PUTLLUC move the 128-byte line to and from a 128-byte
 *     aligned local-store address, whatever the low bits of MFC_LSA say.
 *
 * spu_channels.c is compiled in rather than linked, as in
 * test_spu_decrementer.c, whose build line this test shares; tools/
 * test_spu_channel_semantics.py runs it. Pass a handoff count as the first
 * argument to run the blocking-read handoff more times (default 2000) and
 * print how many were left to the re-poll.
 *
 * Exit code: 0 if all passed, 1 if any failed.
 */

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint8_t g_guest_mem[1u << 20];
uint8_t* vm_base = g_guest_mem;
uint32_t ppu_vm_size = 0;

#include "../spu_channels.c"

static int g_pass, g_fail;
#define CHECK(cond) do {                                                    \
    if (cond) g_pass++;                                                     \
    else { g_fail++; fprintf(stderr, "FAIL at %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static spu_context g_spu;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* ---------------------------------------------------------------------------
 * Blocking reads and the wake that ends them
 * -----------------------------------------------------------------------*/
static int g_prod_done;

static void* wake_producer(void* arg)
{
    (void)arg;
    spu_channel_write(&g_spu.ch_in_mbox, 0x5A5A5A5Au);
    spu_ch_wake(&g_spu);
    __atomic_store_n(&g_prod_done, 1, __ATOMIC_SEQ_CST);
    return NULL;
}

/* A reader tests its predicate and sleeps under ch_wait_lock. Holding that
 * lock stands in for a reader that has just found the mailbox empty: the
 * producer's wake has to come after the reader is asleep (and so can see it),
 * which means it cannot complete while the lock is held. */
static void test_wake_is_ordered_after_the_waiter(void)
{
    pthread_t t;
    g_prod_done = 0;
    AcquireSRWLockExclusive((SRWLOCK*)&g_spu.ch_wait_lock);
    pthread_create(&t, NULL, wake_producer, NULL);
    Sleep(100);
    CHECK(!__atomic_load_n(&g_prod_done, __ATOMIC_SEQ_CST));
    ReleaseSRWLockExclusive((SRWLOCK*)&g_spu.ch_wait_lock);
    pthread_join(t, NULL);
    CHECK(__atomic_load_n(&g_prod_done, __ATOMIC_SEQ_CST));
    (void)spu_channel_read(&g_spu.ch_in_mbox);
}

/* Ping-pong a mailbox word through a reader that blocks in rdch, writing at
 * the moment the reader is about to find the mailbox empty. Returns how many
 * handoffs took longer than the 10 ms re-poll could explain. */
static spu_context g_hs_spu;
static int g_hs_ready, g_hs_got, g_hs_total;
static uint64_t g_hs_stamp, g_hs_slowest;
static int g_hs_late;

static void* handoff_reader(void* arg)
{
    (void)arg;
    for (int i = 0; i < g_hs_total; i++) {
        __atomic_store_n(&g_hs_ready, i + 1, __ATOMIC_SEQ_CST);
        const uint32_t v = spu_rdch(&g_hs_spu, SPU_RdInMbox)._u32[0];
        const uint64_t dt = now_ns() - __atomic_load_n(&g_hs_stamp, __ATOMIC_SEQ_CST);
        if (v != 0xC0DE0000u + (uint32_t)i) g_hs_late = -1000000;
        if (dt > 5000000u) g_hs_late++;
        if (dt > g_hs_slowest) g_hs_slowest = dt;
        __atomic_store_n(&g_hs_got, i + 1, __ATOMIC_SEQ_CST);
    }
    return NULL;
}

static void test_blocking_read_handoff(int n)
{
    pthread_t t;
    spu_context_init(&g_hs_spu, 1);
    g_hs_total = n; g_hs_ready = g_hs_got = g_hs_late = 0; g_hs_slowest = 0;
    pthread_create(&t, NULL, handoff_reader, NULL);
    for (int i = 0; i < n; i++) {
        while (__atomic_load_n(&g_hs_ready, __ATOMIC_SEQ_CST) != i + 1) Sleep(0);
        for (volatile int k = 0; k < (i * 7) % 300; k++) { }   /* vary where the write lands */
        __atomic_store_n(&g_hs_stamp, now_ns(), __ATOMIC_SEQ_CST);
        spu_channel_write(&g_hs_spu.ch_in_mbox, 0xC0DE0000u + (uint32_t)i);
        spu_ch_wake(&g_hs_spu);
        while (__atomic_load_n(&g_hs_got, __ATOMIC_SEQ_CST) != i + 1) Sleep(0);
    }
    pthread_join(t, NULL);
    CHECK(g_hs_late >= 0);                       /* every word arrived, in order */
    printf("%d blocking mailbox handoffs: %d slower than 5 ms, slowest %.3f ms\n",
           n, g_hs_late < 0 ? 0 : g_hs_late, (double)g_hs_slowest / 1e6);
}

/* ---------------------------------------------------------------------------
 * Event status, SRR0
 * -----------------------------------------------------------------------*/
static uint32_t rdch(uint32_t ch) { return spu_rdch(&g_spu, ch)._u32[0]; }
static void wrch(uint32_t ch, uint32_t v) { spu_wrch(&g_spu, ch, spu_splat_u32(v)); }

static void test_event_status_is_pending_and_enabled(void)
{
    g_spu.event_status = 0;
    wrch(SPU_WrEventMask, 0x0002u);
    SPU_EVENT_SET(&g_spu, 0x0002u | 0x0010u);          /* one enabled, one not */
    CHECK(rdch(SPU_RdEventStat) == 0x0002u);

    wrch(SPU_WrEventMask, 0x0010u | 0x0002u);
    CHECK(rdch(SPU_RdEventStat) == (0x0002u | 0x0010u));

    wrch(SPU_WrEventMask, 0x0004u);                    /* only a disabled event is pending */
    g_spu.event_status = 0x0002u;
    CHECK(spu_rchcnt(&g_spu, SPU_RdEventStat) == 0);   /* what the count already said */
    wrch(SPU_WrEventMask, 0);                          /* nothing enabled: nothing reads */
    CHECK(rdch(SPU_RdEventStat) == 0);
    g_spu.event_status = 0;
}

static void test_srr0_is_a_word_local_store_address(void)
{
    wrch(SPU_WrSRR0, 0x12345678u);
    CHECK(rdch(SPU_RdSRR0) == (0x12345678u & 0x3FFFCu));
    wrch(SPU_WrSRR0, 0xFFFFFFFFu);
    CHECK(rdch(SPU_RdSRR0) == 0x3FFFCu);
    wrch(SPU_WrSRR0, 0x2040u);
    CHECK(rdch(SPU_RdSRR0) == 0x2040u);
}

/* ---------------------------------------------------------------------------
 * Atomic commands and the local-store address
 * -----------------------------------------------------------------------*/
static void atomic_cmd(spu_context* c, uint32_t ea, uint32_t lsa, uint32_t cmd)
{
    c->mfc_eah = 0; c->mfc_eal = ea; c->mfc_lsa = lsa;
    (void)spu_mfc_atomic(c, cmd);
}

static void test_atomic_commands_use_an_aligned_lsa(void)
{
    static spu_context c;
    const uint32_t ea = 0x40000;
    uint8_t line[128], other[128];
    for (int i = 0; i < 128; i++) { line[i] = (uint8_t)(0x10 + i); other[i] = (uint8_t)(0xA0 ^ i); }
    spu_context_init(&c, 2);
    c.image_id = -1;

    /* GETLLAR with the low seven bits of the LSA set fills the aligned line. */
    memcpy(g_guest_mem + ea, line, 128);
    memset(c.ls, 0xEE, SPU_LS_SIZE);
    atomic_cmd(&c, ea, 0x2000 + 0x4C, MFC_GETLLAR_CMD);
    CHECK(memcmp(c.ls + 0x2000, line, 128) == 0);
    CHECK(c.ls[0x1FFF] == 0xEE && c.ls[0x2080] == 0xEE);
    CHECK(c.resv_valid);

    /* PUTLLC from an unaligned LSA stores the aligned line and succeeds. */
    memcpy(c.ls + 0x2000, other, 128);
    atomic_cmd(&c, ea, 0x2000 + 0x7F, MFC_PUTLLC_CMD);
    CHECK(c.atomic_stat == 0);
    CHECK(memcmp(g_guest_mem + ea, other, 128) == 0);

    /* PUTLLUC likewise. */
    memcpy(c.ls + 0x3000, line, 128);
    atomic_cmd(&c, ea + 0x80, 0x3000 + 0x01, MFC_PUTLLUC_CMD);
    CHECK(memcmp(g_guest_mem + ea + 0x80, line, 128) == 0);

    /* The last 128 bytes of local store: an unaligned LSA used to carry the
     * copy past the end of the store and over the context behind it. */
    {
        const size_t after = offsetof(spu_context, ls_store) + SPU_LS_SIZE;
        uint8_t tail[128], tail_before[128];
        memcpy(tail_before, (uint8_t*)&c + after, sizeof tail_before);
        memcpy(g_guest_mem + ea, line, 128);
        atomic_cmd(&c, ea, SPU_LS_SIZE - 16, MFC_GETLLAR_CMD);
        memcpy(tail, (uint8_t*)&c + after, sizeof tail);
        CHECK(memcmp(tail, tail_before, sizeof tail) == 0);
        CHECK(memcmp(c.ls + SPU_LS_SIZE - 128, line, 128) == 0);
    }
}

int main(int argc, char** argv)
{
    const int handoffs = argc > 1 ? atoi(argv[1]) : 2000;

    spu_context_init(&g_spu, 0);
    g_spu.image_id = -1;
    g_spu_force_ch_block = 1;           /* blocking reads, as for a raw SPU */

    test_wake_is_ordered_after_the_waiter();
    test_blocking_read_handoff(handoffs);
    test_event_status_is_pending_and_enabled();
    test_srr0_is_a_word_local_store_address();
    test_atomic_commands_use_an_aligned_lsa();

    printf("SPU channel semantics: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * The runtime beyond the SPU channel layer, stubbed as in
 * test_spu_decrementer.c. None of it is on the path this test drives.
 * -----------------------------------------------------------------------*/
void ppu_resv_break_store(uint64_t ea)                 { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w)
{
    (void)a; (void)v; (void)w;
}
unsigned long long ps3_ms_now(void)                    { return now_ns() / 1000000u; }
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
