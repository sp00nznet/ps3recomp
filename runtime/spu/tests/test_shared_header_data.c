/*
 * Header-defined shared data is one variable per program.
 *
 * spu_coh_bitmap.h (g_spu_coh_bits) and ps3_ftrace.h (g_ps3_ft) define their
 * state in the header so that targets which link no extra object still see it.
 * Every translation unit that includes a header gets a definition; the linker
 * has to fold them into one. This test stores through one C object and reads
 * through a C++ object (test_shared_header_data_b.cpp), the way the PPU
 * loader and the SPU coherency code reach g_spu_coh_bits, and compares the
 * addresses both objects see.
 *
 * Build (any toolchain; MinGW-w64 as well):
 *   cc -std=gnu17 -I include -I runtime/spu -c runtime/spu/tests/test_shared_header_data.c -o a.o
 *   c++ -std=gnu++17 -I include -I runtime/spu -c runtime/spu/tests/test_shared_header_data_b.cpp -o b.o
 *   c++ a.o b.o -o test_shared_header_data
 */
#include "spu_coh_bitmap.h"
#include "ps3emu/ps3_ftrace.h"
#include <stdio.h>

int  shared_b_line_reserved(uint32_t addr);
const void* shared_b_coh_bits(void);
const void* shared_b_ft(void);

static unsigned char s_bitmap[1u << 21];   /* one bit per 128-byte line, 4 GiB */

int main(void)
{
    int fail = 0;
    const int before = shared_b_line_reserved(0x1000);

    s_bitmap[(0x1000u >> 7) >> 3] |= (unsigned char)(1u << ((0x1000u >> 7) & 7));
    g_spu_coh_bits = s_bitmap;   /* what spu_coherency.c does on the first reservation */

    const int after_here  = spu_coh_line_reserved(0x1000);
    const int after_there = shared_b_line_reserved(0x1000);
    const int other_line  = shared_b_line_reserved(0x2000);

    if (before != 0)          { printf("[FAIL] reserved before the store: %d\n", before); fail++; }
    else                      printf("[PASS] not reserved before the store\n");
    if (after_here != 1)      { printf("[FAIL] store not seen in the storing object\n"); fail++; }
    else                      printf("[PASS] store seen in the storing object\n");
    if (after_there != 1)     { printf("[FAIL] store not seen in the other object\n"); fail++; }
    else                      printf("[PASS] store seen in the other object\n");
    if (other_line != 0)      { printf("[FAIL] unreserved line reads as reserved\n"); fail++; }
    else                      printf("[PASS] unreserved line stays clear\n");
    if (shared_b_coh_bits() != (const void*)&g_spu_coh_bits)
                              { printf("[FAIL] g_spu_coh_bits has two addresses\n"); fail++; }
    else                      printf("[PASS] g_spu_coh_bits is one variable\n");
    if (shared_b_ft() != (const void*)&g_ps3_ft)
                              { printf("[FAIL] g_ps3_ft has two addresses\n"); fail++; }
    else                      printf("[PASS] g_ps3_ft is one variable\n");

    printf("\nResults: %d failed\n", fail);
    return fail ? 1 : 0;
}
