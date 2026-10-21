/* spu_rotqby (pshufb path) must match the byte-loop reference for every
 * shift, including the ignored high bits of the preferred word. Build with
 * -msse4.1 so the SIMD version is the one under test. */
#include "../spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
#if !defined(__SSE4_1__)
    puts("test_spu_rotqby: built without SSE4.1, nothing to compare");
    return 0;
#else
    srand(24680);
    for (int it = 0; it < 200000; it++) {
        u128 a, b;
        for (int i = 0; i < 16; i++) {
            a._u8[i] = (uint8_t)rand(); b._u8[i] = (uint8_t)rand();
        }
        if (it < 4096) b._u32[0] = (uint32_t)it * 0x01010101u;  /* every low nibble, odd high bits */
        u128 x = spu_rotqby(a, b), y = spu_rotqby_ref(a, b);
        if (memcmp(&x, &y, 16)) {
            printf("test_spu_rotqby: MISMATCH at iteration %d (shift %u)\n", it, b._u32[0] & 0x0F);
            return 1;
        }
    }
    puts("test_spu_rotqby: ok (200000 random cases, all 16 shifts)");
    return 0;
#endif
}
