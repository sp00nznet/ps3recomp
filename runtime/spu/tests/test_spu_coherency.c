/*
 * Unit tests for PPU <-> SPU lock-line coherence (runtime/spu/spu_coherency.c).
 *
 * The mechanism only pays off when three things line up: an SPU marks the line
 * it reserved, a PPU store notices the mark, and the reserving SPU gets
 * SPU_EVENT_LR and loses its reservation. Any one of them silently missing
 * leaves the SPU parked forever, and nothing else in the tree fails, so this
 * asserts all three -- through the REAL PPU store path (ppu_memory.h's
 * vm_write* and ppu_stwcx), not a copy of it, since the whole bug class here is
 * a hook that is not where the stores actually go.
 *
 * The two paths that must NOT fire are tested as carefully as the one that
 * must: a store to a line nobody reserved, and a store to a line other than the
 * reserved one, both have to leave the SPU's event bit alone. A notify that
 * fires too eagerly is a livelock (the SPURS kernel re-enters selection on
 * every unrelated PPU store), not a safe over-approximation.
 *
 * Build:
 *   clang -std=gnu17 -I include -I runtime/spu \
 *         -o /tmp/test_spu_coherency runtime/spu/tests/test_spu_coherency.c
 *
 * Exit code: 0 if all passed, 1 if any failed. Final line prints summary.
 */

/* The unit under test, compiled straight in: it is one small file whose only
 * dependency on the rest of the runtime is spu_ch_wake, stubbed below. */
#include "../spu_coherency.c"

/* The real PPU store and store-conditional path the hook lives in. */
#include "../../ppu/ppu_memory.h"

#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * The runtime pieces a real build supplies, stubbed.
 * -----------------------------------------------------------------------*/
static uint8_t g_guest_mem[1u << 20];
uint8_t* vm_base = g_guest_mem;

/* ppu_memory.h's inline stores reference these. Nothing here watches writes,
 * and PPU-vs-PPU reservation breaking is a different mechanism. */
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi = 0;
int      g_resv_store_active = 0;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t addr, uint64_t val, int width)
{
    (void)addr; (void)val; (void)width;
}

/* spu_channels.c's channel-stall wake. Counted, because "the event bit is set"
 * and "the parked host thread was told" are separate failures: the bit alone
 * wakes the SPU only on its next 10 ms re-poll. */
static int g_wakes;
void spu_ch_wake(spu_context* ctx) { (void)ctx; g_wakes++; }

/* ---------------------------------------------------------------------------
 * Harness
 * -----------------------------------------------------------------------*/
static int g_pass = 0;
static int g_fail = 0;
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

/* Two 128-byte lines well apart, plus an address inside each. */
#define LINE_A   0x00040000u
#define LINE_B   0x00040180u
#define LINE_C   0x00040300u

static spu_context g_spu_a;   /* 256 KB of local store each: not on the stack */
static spu_context g_spu_b;

/* What the SPU's GETLLAR does, minus the local-store copy: take the lock-line
 * lock, mark the line, record the reservation. Mirrors spu_channels.c's
 * MFC_GETLLAR_CMD case. */
static void spu_getllar(spu_context* ctx, uint32_t ea)
{
    ea &= ~127u;
    spu_lockline_lock();
    spu_coh_reserve(ctx, ea);
    ctx->resv_ea = ea;
    ctx->resv_valid = 1;
    spu_lockline_unlock();
}

static void spu_reset(spu_context* ctx, uint32_t spu_id)
{
    spu_context_init(ctx, 0);
    ctx->spu_id = spu_id;
}

/* ===========================================================================
 * The bitmap: what a reservation marks, and what it leaves alone
 * ===========================================================================*/
static void test_bitmap(void)
{
    spu_reset(&g_spu_a, 0);

    TEST("a line nobody reserved is not marked");
    CHECK(spu_coh_is_reserved(LINE_A) == 0);
    CHECK(spu_coh_is_reserved(LINE_A + 64) == 0);

    spu_getllar(&g_spu_a, LINE_A);

    TEST("GETLLAR marks the whole 128-byte line, not just the word");
    CHECK(spu_coh_is_reserved(LINE_A) != 0);
    CHECK(spu_coh_is_reserved(LINE_A + 4) != 0);
    CHECK(spu_coh_is_reserved(LINE_A + 127) != 0);

    TEST("the neighbouring lines stay unmarked");
    CHECK(spu_coh_is_reserved(LINE_A - 1) == 0);
    CHECK(spu_coh_is_reserved(LINE_A + 128) == 0);
    CHECK(spu_coh_is_reserved(LINE_B) == 0);
}

/* ===========================================================================
 * A PPU store into a reserved line
 * ===========================================================================*/
static void test_ppu_store_raises_lr(void)
{
    spu_reset(&g_spu_a, 0);
    g_wakes = 0;
    unsigned long raised_before = g_spu_lr_raise;

    spu_getllar(&g_spu_a, LINE_A);

    TEST("a PPU store into the reserved line raises SPU_EVENT_LR");
    vm_write32(LINE_A + 8, 0xDEADBEEFu);
    CHECK_EQ_U32(g_spu_a.event_status & SPU_EVENT_LR, SPU_EVENT_LR);

    TEST("...and drops the reservation, so the SPU's PUTLLC must fail");
    CHECK(g_spu_a.resv_valid == 0);

    TEST("...and wakes the host thread parked on the event channel");
    CHECK(g_wakes == 1);
    CHECK(g_spu_lr_raise == raised_before + 1);

    TEST("...and the store itself still lands, big-endian");
    CHECK_EQ_U32(vm_read32(LINE_A + 8), 0xDEADBEEFu);

    TEST("the line stays marked after the notify (reservations recur)");
    CHECK(spu_coh_is_reserved(LINE_A) != 0);

    TEST("a second store with no live reservation raises nothing more");
    vm_write32(LINE_A + 8, 0x11111111u);
    CHECK(g_wakes == 1);
    CHECK(g_spu_lr_raise == raised_before + 1);

    TEST("the SPU acks the event and the bit clears");
    g_spu_a.event_status &= ~SPU_EVENT_LR;
    CHECK_EQ_U32(g_spu_a.event_status, 0);

    TEST("re-reserving and storing again raises it a second time");
    spu_getllar(&g_spu_a, LINE_A);
    vm_write32(LINE_A + 8, 0x22222222u);
    CHECK_EQ_U32(g_spu_a.event_status & SPU_EVENT_LR, SPU_EVENT_LR);
    CHECK(g_wakes == 2);
}

/* ===========================================================================
 * The two cases that must NOT raise
 * ===========================================================================*/
static void test_no_false_raise(void)
{
    spu_reset(&g_spu_a, 0);
    g_wakes = 0;

    spu_getllar(&g_spu_a, LINE_A);

    TEST("a store to a DIFFERENT line leaves the reservation alone");
    vm_write32(LINE_B + 8, 0x33333333u);
    CHECK_EQ_U32(g_spu_a.event_status, 0);
    CHECK(g_spu_a.resv_valid == 1);
    CHECK(g_wakes == 0);

    TEST("...even when that line is itself reserved by another SPU");
    spu_reset(&g_spu_b, 1);
    spu_getllar(&g_spu_b, LINE_B);
    vm_write32(LINE_B + 8, 0x44444444u);
    CHECK_EQ_U32(g_spu_b.event_status & SPU_EVENT_LR, SPU_EVENT_LR);
    CHECK_EQ_U32(g_spu_a.event_status, 0);
    CHECK(g_spu_a.resv_valid == 1);

    TEST("an SPU that holds NO reservation is never notified");
    spu_reset(&g_spu_b, 1);            /* registered, but resv_valid = 0 */
    g_wakes = 0;
    vm_write32(LINE_B + 8, 0x55555555u);
    CHECK_EQ_U32(g_spu_b.event_status, 0);
    CHECK(g_wakes == 0);
}

/* ===========================================================================
 * Every store width, and the store-conditional commit
 * ===========================================================================*/
static void test_all_store_widths(void)
{
    static const char* names[4] = { "vm_write8", "vm_write16",
                                    "vm_write32", "vm_write64" };
    for (int w = 0; w < 4; w++) {
        spu_reset(&g_spu_a, 0);
        spu_getllar(&g_spu_a, LINE_C);

        TEST(names[w]);
        switch (w) {
        case 0: vm_write8 (LINE_C + 16, 0xA5u);                    break;
        case 1: vm_write16(LINE_C + 16, 0xA5A5u);                  break;
        case 2: vm_write32(LINE_C + 16, 0xA5A5A5A5u);              break;
        case 3: vm_write64(LINE_C + 16, 0xA5A5A5A5A5A5A5A5ull);    break;
        }
        CHECK_EQ_U32(g_spu_a.event_status & SPU_EVENT_LR, SPU_EVENT_LR);
        CHECK(g_spu_a.resv_valid == 0);
    }
}

static void test_store_conditional(void)
{
    ppu_context ppu;
    ppu_context_init(&ppu);

    /* A successful stwcx on a reserved line notifies. */
    spu_reset(&g_spu_a, 0);
    spu_getllar(&g_spu_a, LINE_A);
    vm_write32(LINE_A + 32, 0x1000u);          /* seed, drops the reservation */
    spu_getllar(&g_spu_a, LINE_A);             /* reserve again */
    g_wakes = 0;

    TEST("a successful stwcx into a reserved line raises SPU_EVENT_LR");
    CHECK_EQ_U32(ppu_lwarx(&ppu, LINE_A + 32), 0x1000u);
    CHECK(ppu_stwcx(&ppu, LINE_A + 32, 0x1001u) == 1);
    CHECK_EQ_U32(vm_read32(LINE_A + 32), 0x1001u);
    CHECK_EQ_U32(g_spu_a.event_status & SPU_EVENT_LR, SPU_EVENT_LR);
    CHECK(g_spu_a.resv_valid == 0);
    CHECK(g_wakes == 1);

    /* A stwcx that FAILS its compare stored nothing, so there is nothing to
     * report: the SPU's line is untouched and its reservation still good. */
    spu_reset(&g_spu_a, 0);
    spu_getllar(&g_spu_a, LINE_A);
    g_wakes = 0;

    TEST("a failed stwcx raises nothing");
    ppu_lwarx(&ppu, LINE_A + 32);
    vm_write32(LINE_A + 64, 0x9999u);          /* elsewhere in the line */
    g_spu_a.event_status = 0;                  /* ignore that one */
    g_spu_a.resv_valid = 1;
    g_wakes = 0;
    ppu.reserve_value = 0xFFFFFFFFu;           /* now the compare cannot match */
    CHECK(ppu_stwcx(&ppu, LINE_A + 32, 0x2002u) == 0);
    CHECK_EQ_U32(g_spu_a.event_status, 0);
    CHECK(g_spu_a.resv_valid == 1);
    CHECK(g_wakes == 0);

    /* stdcx., the 8-byte commit. */
    spu_reset(&g_spu_a, 0);
    spu_getllar(&g_spu_a, LINE_A);
    vm_write64(LINE_A + 40, 0x0102030405060708ull);
    spu_getllar(&g_spu_a, LINE_A);
    g_wakes = 0;

    TEST("a successful stdcx into a reserved line raises SPU_EVENT_LR");
    CHECK(ppu_ldarx(&ppu, LINE_A + 40) == 0x0102030405060708ull);
    CHECK(ppu_stdcx(&ppu, LINE_A + 40, 0x1112131415161718ull) == 1);
    CHECK(vm_read64(LINE_A + 40) == 0x1112131415161718ull);
    CHECK_EQ_U32(g_spu_a.event_status & SPU_EVENT_LR, SPU_EVENT_LR);
    CHECK(g_wakes == 1);
}

/* ===========================================================================
 * Block stores: a memset or memcpy is a store to every line it touches
 * ===========================================================================*/
static void test_block_stores(void)
{
    const uint32_t LINE_D = 0x00040400u;       /* never reserved by anyone */
    uint8_t buf[512];
    memset(buf, 0x5A, sizeof(buf));

    /* One block from inside LINE_A to inside LINE_B, with a different SPU
     * holding each: both reservations go, both SPUs wake, once each. */
    spu_reset(&g_spu_a, 0);
    spu_reset(&g_spu_b, 1);
    spu_getllar(&g_spu_a, LINE_A);
    spu_getllar(&g_spu_b, LINE_B);
    g_wakes = 0;

    TEST("a memset spanning two reserved lines breaks both reservations");
    vm_memset(LINE_A + 100, 0x77, (LINE_B + 10) - (LINE_A + 100));
    CHECK(vm_read8(LINE_A + 100) == 0x77u);
    CHECK(vm_read8(LINE_B + 9) == 0x77u);
    CHECK_EQ_U32(g_spu_a.event_status & SPU_EVENT_LR, SPU_EVENT_LR);
    CHECK_EQ_U32(g_spu_b.event_status & SPU_EVENT_LR, SPU_EVENT_LR);
    CHECK(g_spu_a.resv_valid == 0);
    CHECK(g_spu_b.resv_valid == 0);
    CHECK(g_wakes == 2);

    TEST("a memcpy into a reserved line breaks it");
    spu_reset(&g_spu_a, 0);
    spu_getllar(&g_spu_a, LINE_C);
    g_wakes = 0;
    vm_memcpy_to(LINE_C + 120, buf, 16);       /* straddles into the next line */
    CHECK(vm_read8(LINE_C + 120) == 0x5Au);
    CHECK(vm_read8(LINE_C + 135) == 0x5Au);
    CHECK_EQ_U32(g_spu_a.event_status & SPU_EVENT_LR, SPU_EVENT_LR);
    CHECK(g_spu_a.resv_valid == 0);
    CHECK(g_wakes == 1);

    TEST("a block that touches no reserved line raises nothing");
    spu_reset(&g_spu_a, 0);
    spu_getllar(&g_spu_a, LINE_C);
    g_wakes = 0;
    vm_memcpy_to(LINE_D, buf, sizeof(buf));
    vm_memset(LINE_D, 0, 300);
    CHECK_EQ_U32(g_spu_a.event_status, 0);
    CHECK(g_spu_a.resv_valid == 1);
    CHECK(g_wakes == 0);

    TEST("an empty block is not a store");
    vm_memset(LINE_C, 0, 0);
    vm_memcpy_to(LINE_C, buf, 0);
    CHECK_EQ_U32(g_spu_a.event_status, 0);
    CHECK(g_spu_a.resv_valid == 1);
    CHECK(g_wakes == 0);
}

/* ===========================================================================
 * main: run all and report
 * ===========================================================================*/
int main(void)
{
    test_bitmap();
    test_ppu_store_raises_lr();
    test_no_false_raise();
    test_all_store_widths();
    test_store_conditional();
    test_block_stores();

    printf("\nSPU lock-line coherence tests: %d passed, %d failed\n",
           g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
