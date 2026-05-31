/*
 * ps3recomp - HLE NID dispatch + PPC-ABI adapter test (self-contained).
 *
 * Proves the NID -> handler resolution and the generic PPC integer/pointer ABI
 * marshalling (args r3..r10 -> C args, return -> r3) without linking any HLE
 * library. Generation of the real NID table from annotations is validated
 * separately by compiling gen_hle_nids.py's output.
 *
 * Build: g++ -std=c++17 -I <lift_out> -x c++ test_hle.cpp ../ppu_hle.cpp -o test_hle.exe
 */
#include "ppu_recomp.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern "C" {
void     ps3_hle_register(uint32_t nid, const char* name, void* handler);
void     ps3_hle_call(uint32_t nid, ppu_context* ctx);
uint32_t ps3_hle_count(void);
}

/* A mock "HLE" function: 3 integer args, returns a computed value. */
static uint64_t mock_combine(uint64_t a, uint64_t b, uint64_t c)
{
    return a * 1000 + b * 10 + c;
}

static int fails = 0;
#define CHECK(c, m) do { if (c) printf("[PASS] %s\n", m); else { printf("[FAIL] %s\n", m); fails++; } } while (0)

int main()
{
    ps3_hle_register(0x12345678u, "mock_combine", (void*)mock_combine);
    ps3_hle_register(0xAABBCCDDu, "mock_other",   (void*)mock_combine);
    CHECK(ps3_hle_count() == 2, "two HLE NIDs registered");

    /* Dispatch through the NID with PPC ABI args in r3/r4/r5. */
    ppu_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.gpr[3] = 7; ctx.gpr[4] = 8; ctx.gpr[5] = 9;
    ps3_hle_call(0x12345678u, &ctx);
    printf("ps3_hle_call(mock_combine, r3=7,r4=8,r5=9) -> r3 = %llu\n",
           (unsigned long long)ctx.gpr[3]);
    CHECK(ctx.gpr[3] == 7089, "ABI adapter marshalled r3..r5 and returned via r3");

    /* Unresolved NID must not crash; returns 0. */
    memset(&ctx, 0, sizeof(ctx));
    ctx.gpr[3] = 999;
    ps3_hle_call(0xDEADBEEFu, &ctx);
    CHECK(ctx.gpr[3] == 0, "unresolved NID returns 0 (no crash)");

    printf("\nResults: %s (%d failure(s))\n", fails ? "FAIL" : "OK", fails);
    return fails ? 1 : 0;
}
