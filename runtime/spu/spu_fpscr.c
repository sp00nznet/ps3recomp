/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * SPU FPSCR state implementation.
 *
 * Derived from RPCS3's SPU_FPSCR in rpcs3/Emu/Cell/SPUThread.h at upstream
 * revision 4d5def5880217135554ed434cd5055cceebe9cc2 (GPL-2.0), with word
 * indices reversed so that words[0] is the architectural LEFT-most word.
 */
#include "spu_fpscr.h"

#include <stddef.h>

#define SPU_FPSCR_SINGLE_FLAG_BITS 0x00000007u
#define SPU_FPSCR_DOUBLE_FLAG_BITS 0x00003F00u

/* Writable bits per word, LEFT (words[0]) to RIGHT (words[3]). */
static const uint32_t spu_fpscr_write_masks[4] = {
    0x00000F07u, 0x00003F07u, 0x00003F07u, 0x00000F07u
};

/* Rounding lives in the LEFT word: logical slice 0 at bits 10-11, slice 1 at
 * bits 8-9. */
static unsigned spu_fpscr_rounding_shift(unsigned slice)
{
    return 10u - 2u * slice;
}

void spu_fpscr_reset(spu_fpscr *state)
{
    state->words[0] = 0;
    state->words[1] = 0;
    state->words[2] = 0;
    state->words[3] = 0;
}

void spu_fpscr_write(spu_fpscr *state, const uint32_t words[4])
{
    /* Load everything first so words may alias state->words. */
    const uint32_t w0 = words[0];
    const uint32_t w1 = words[1];
    const uint32_t w2 = words[2];
    const uint32_t w3 = words[3];

    state->words[0] = w0 & spu_fpscr_write_masks[0];
    state->words[1] = w1 & spu_fpscr_write_masks[1];
    state->words[2] = w2 & spu_fpscr_write_masks[2];
    state->words[3] = w3 & spu_fpscr_write_masks[3];
}

void spu_fpscr_read(const spu_fpscr *state, uint32_t words[4])
{
    const uint32_t w0 = state->words[0];
    const uint32_t w1 = state->words[1];
    const uint32_t w2 = state->words[2];
    const uint32_t w3 = state->words[3];

    words[0] = w0;
    words[1] = w1;
    words[2] = w2;
    words[3] = w3;
}

int spu_fpscr_get_rounding(const spu_fpscr *state, unsigned slice)
{
    if (state == NULL || slice > 1u) {
        return -1;
    }
    return (int)((state->words[0] >> spu_fpscr_rounding_shift(slice)) & 3u);
}

int spu_fpscr_set_rounding(spu_fpscr *state, unsigned slice, unsigned mode)
{
    unsigned shift;

    if (state == NULL || slice > 1u || mode > 3u) {
        return -1;
    }
    shift = spu_fpscr_rounding_shift(slice);
    state->words[0] = (state->words[0] & ~(3u << shift)) | ((uint32_t)mode << shift);
    return 0;
}

int spu_fpscr_single_flags(spu_fpscr *state, unsigned slice, uint32_t flags)
{
    if (state == NULL || slice > 3u || (flags & ~SPU_FPSCR_SINGLE_FLAG_BITS) != 0) {
        return -1;
    }
    state->words[slice] |= flags;
    return 0;
}

int spu_fpscr_double_flags(spu_fpscr *state, unsigned slice, uint32_t flags)
{
    if (state == NULL || slice > 1u || (flags & ~SPU_FPSCR_DOUBLE_FLAG_BITS) != 0) {
        return -1;
    }
    state->words[1u + slice] |= flags;
    return 0;
}

int spu_fpscr_divide_by_zero(spu_fpscr *state, unsigned slice)
{
    if (state == NULL || slice > 3u) {
        return -1;
    }
    /* Divide-by-zero flags live in the RIGHT word, LEFT slice highest. */
    state->words[3] |= 1u << (11u - slice);
    return 0;
}
