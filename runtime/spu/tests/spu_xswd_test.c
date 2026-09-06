/* Standalone check of spu_xswd: sign-extend SPU words 1 and 3 to doublewords.
 * SPU word N lives at _u32[N]; a doubleword result is (high,low) = (_u32[2d], _u32[2d+1]). */
#include <stdio.h>
#include <stdint.h>
#include "../spu_helpers.h"

static int fails = 0;
static void check(const char* name, uint32_t w1, uint32_t w3,
                  uint32_t eh0, uint32_t el0, uint32_t eh1, uint32_t el1)
{
    u128 a; for (int i = 0; i < 4; i++) a._u32[i] = 0xDEADBEEF;
    a._u32[1] = w1; a._u32[3] = w3;
    u128 r = spu_xswd(a);
    int ok = r._u32[0]==eh0 && r._u32[1]==el0 && r._u32[2]==eh1 && r._u32[3]==el1;
    printf("  %-28s %08X %08X %08X %08X   %s\n", name,
           r._u32[0], r._u32[1], r._u32[2], r._u32[3], ok ? "ok" : "FAIL");
    if (!ok) { printf("      expected %08X %08X %08X %08X\n", eh0, el0, eh1, el1); fails++; }
}

int main(void)
{
    printf("spu_xswd:\n");
    /* the case that was silently returning zero */
    check("small positive (1, 2)",      1u, 2u,          0u, 1u,          0u, 2u);
    check("larger positive",            0x00001234u, 0x0000ABCDu, 0u, 0x00001234u, 0u, 0x0000ABCDu);
    check("negative sign-extends",      0xFFFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x80000000u);
    check("zero",                       0u, 0u,          0u, 0u,          0u, 0u);
    check("max positive int32",         0x7FFFFFFFu, 0x00000001u, 0u, 0x7FFFFFFFu, 0u, 1u);
    printf(fails ? "FAILED %d\n" : "all passed\n", fails);
    return fails != 0;
}
