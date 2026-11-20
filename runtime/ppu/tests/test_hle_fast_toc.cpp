/*
 * Fast HLE handler TOC regression.
 *
 * A firmware import is a cross-module call: the caller executes
 *
 *     bl   <import stub>
 *     ld   r2, 0x28(r1)
 *
 * expecting the import stub to have spilled the caller's TOC into the reserved
 * ABI slot at [r1+0x28]. Fast handlers used to return before _TocGuard, so the
 * slot kept whatever happened to be there (often 0) and every later
 * TOC-relative global read was garbage.
 *
 * This test checks both fast-path exits:
 *   - the fast handler handles the call (returns 1)
 *   - the fast handler declines and the full dispatcher runs (returns 0)
 *
 * Build (also wired as the hle_fast_toc CTest):
 *   see the test_hle_fast_toc target in CMakeLists.txt.
 */
#define PPU_NO_INLINE_LOADS
#define PPU_NO_INLINE_STORES
#include "ppu_recomp.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern "C" {
void ps3_hle_register(uint32_t nid, const char* name, void* handler);
void ps3_hle_register_fast(uint32_t nid, int (*fn)(ppu_context*));
void ps3_hle_call(uint32_t nid, ppu_context* ctx);
}

static int fails;

#define CHECK(cond, msg) do { \
    if (cond) printf("[PASS] %s\n", msg); \
    else { printf("[FAIL] %s\n", msg); fails++; } \
} while (0)

static int fast_calls;
static int fallback_fast_calls;
static int full_calls;

static int fast_handler(ppu_context* ctx)
{
    fast_calls++;
    ctx->gpr[3] = 0x5151;
    return 1;
}

static int declining_fast_handler(ppu_context* ctx)
{
    (void)ctx;
    fallback_fast_calls++;
    return 0;   /* take the full dispatcher */
}

static uint64_t full_handler(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7)
{
    (void)a0; (void)a1; (void)a2; (void)a3;
    (void)a4; (void)a5; (void)a6; (void)a7;
    full_calls++;
    return 0x1234;
}

int main(void)
{
    const uint32_t NID_FAST     = 0x10000001u;
    const uint32_t NID_FALLBACK = 0x10000002u;
    const uint32_t SP           = 0x1000u;
    const uint64_t TOC          = 0x00E18380ull;
    const uint64_t STALE        = 0xDEADBEEFull;

    ps3_hle_register_fast(NID_FAST, fast_handler);
    ps3_hle_register(NID_FAST, "fast-test", (void*)full_handler);

    ps3_hle_register_fast(NID_FALLBACK, declining_fast_handler);
    ps3_hle_register(NID_FALLBACK, "fallback-test", (void*)full_handler);

    ppu_context ctx;

    /* Fast handler handles the call: r2 and the ABI spill slot must be intact. */
    memset(&ctx, 0, sizeof(ctx));
    ctx.gpr[1] = SP;
    ctx.gpr[2] = TOC;
    ctx.gpr[3] = 0xAAAA;
    vm_write64(SP + 0x28, STALE);

    ps3_hle_call(NID_FAST, &ctx);

    CHECK(fast_calls == 1, "fast handler ran");
    CHECK(full_calls == 0, "full handler did not run when the fast form handled it");
    CHECK((uint32_t)ctx.gpr[3] == 0x5151u, "fast handler's r3 result survived");
    CHECK(ctx.gpr[2] == TOC, "fast path restores r2");
    CHECK(vm_read64(SP + 0x28) == TOC, "fast path spills the caller TOC to [r1+0x28]");

    /* Fast handler declines: the full dispatcher runs and the spill still holds. */
    memset(&ctx, 0, sizeof(ctx));
    ctx.gpr[1] = SP;
    ctx.gpr[2] = TOC;
    ctx.gpr[3] = 0xBBBB;
    vm_write64(SP + 0x28, STALE);

    ps3_hle_call(NID_FALLBACK, &ctx);

    CHECK(fallback_fast_calls == 1, "declining fast handler ran first");
    CHECK(full_calls == 1, "full handler ran after the fast form declined");
    CHECK((uint32_t)ctx.gpr[3] == 0x1234u, "full handler's r3 result survived");
    CHECK(ctx.gpr[2] == TOC, "fallback path restores r2");
    CHECK(vm_read64(SP + 0x28) == TOC, "fallback path spills the caller TOC to [r1+0x28]");

    printf("Results: %s (%d failure(s))\n", fails ? "FAIL" : "OK", fails);
    return fails ? 1 : 0;
}
