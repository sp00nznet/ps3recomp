/*
 * ps3recomp - NID -> HLE end-to-end integration test (real library).
 *
 * Proves the full bridge with a *real* HLE function (not a mock): the generated
 * NID table (gen_hle_nids.py, computed NIDs) registers cellGcmSys's functions;
 * we compute a function's NID with the same firmware algorithm (nid.h) and
 * dispatch through ps3_hle_call, confirming the real cellGcmSys handler runs
 * and returns via the PPC ABI (r3).
 *
 * Build: see the command in test_loader; links ppu_hle.cpp + the generated
 * ppu_hle_nids.cpp + cellGcmSys.c + rsx_commands.c.
 */
#include "ppu_recomp.h"
#include "ps3emu/nid.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern "C" {
void     ps3_hle_call(uint32_t nid, ppu_context* ctx);
uint32_t ps3_hle_count(void);
void     ppu_hle_init(void);
}

/* Host-provided symbols the HLE libs need. */
extern "C" uint8_t* vm_base = nullptr;
typedef void (*ps3_guest_caller_fn)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t);
extern "C" ps3_guest_caller_fn g_ps3_guest_caller = nullptr;

static int fails = 0;
#define CHECK(c, m) do { if (c) printf("[PASS] %s\n", m); else { printf("[FAIL] %s\n", m); fails++; } } while (0)

int main()
{
    ppu_hle_init();                       /* registers cellGcmSys (computed NIDs) */
    uint32_t n = ps3_hle_count();
    printf("registered HLE NIDs: %u\n", n);
    CHECK(n >= 26, "cellGcmSys HLE registered");

    /* cellGcmGetMaxIoMapSize() takes no args and, with no IO mappings yet,
     * returns 256 MiB (0x10000000) -- a deterministic, non-zero value that
     * proves the *real* handler ran (an unresolved NID would return 0). */
    uint32_t nid = ps3_compute_nid("cellGcmGetMaxIoMapSize");
    printf("NID(cellGcmGetMaxIoMapSize) = 0x%08X\n", nid);

    ppu_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    ps3_hle_call(nid, &ctx);
    printf("ps3_hle_call -> r3 = 0x%016llX\n", (unsigned long long)ctx.gpr[3]);
    CHECK(ctx.gpr[3] == 0x10000000ull,
          "real cellGcmGetMaxIoMapSize ran via NID dispatch (r3 = 256 MiB)");

    /* A second, distinct function to confirm dispatch isn't a fluke. */
    uint32_t nid2 = ps3_compute_nid("cellGcmGetCurrentField");
    memset(&ctx, 0, sizeof(ctx));
    ctx.gpr[3] = 0xDEAD;
    ps3_hle_call(nid2, &ctx);
    CHECK(ctx.gpr[3] == 0, "cellGcmGetCurrentField dispatched (returns 0)");

    printf("\nResults: %s (%d failure(s))\n", fails ? "FAIL" : "OK", fails);
    return fails ? 1 : 0;
}
