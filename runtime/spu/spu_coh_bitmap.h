/*
 * ps3recomp - the SPU reserved-line bitmap, for inline store paths
 *
 * Every guest store asks whether an SPU has reserved its 128-byte line
 * (spu_coherency.h). The PPU store paths test the bitmap here, inline, rather
 * than through a call into spu_coherency.c.
 *
 * g_spu_coh_bits is NULL until an SPU reserves its first line, so a title that
 * runs no SPU code pays one load and branch per store. It is one merged
 * definition per program (selectany / weak): a target that links the store
 * paths without spu_coherency.c, such as the PPU loader fixtures, sees NULL
 * and never a reserved line.
 */

#ifndef SPU_COH_BITMAP_H
#define SPU_COH_BITMAP_H

#include <stdint.h>

/* selectany (a COMDAT "any": one definition kept, every object sees the same
 * variable) under MSVC and MinGW. MinGW's weak definition is a PE weak external
 * whose default is a per-object `.weak.<name>.<object>` symbol; linking C++
 * objects that reach it through the .refptr indirection ended with the symbol
 * undefined. */
#if defined(_MSC_VER) || defined(__MINGW32__)
#  define SPU_COH_SHARED __declspec(selectany)
#else
#  define SPU_COH_SHARED __attribute__((weak))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SPU_COH_LINE_SHIFT  7   /* 128-byte lines */

/* One bit per line over the 32-bit guest address space; spu_coherency.c
 * publishes it on the first reservation and only ever sets bits. */
SPU_COH_SHARED const unsigned char* g_spu_coh_bits = 0;

static inline int spu_coh_line_reserved(uint32_t addr)
{
    const unsigned char* bits = g_spu_coh_bits;
    if (!bits) return 0;
    const uint32_t line = addr >> SPU_COH_LINE_SHIFT;
    return (bits[line >> 3] >> (line & 7)) & 1;
}

#ifdef __cplusplus
}
#endif

#endif /* SPU_COH_BITMAP_H */
