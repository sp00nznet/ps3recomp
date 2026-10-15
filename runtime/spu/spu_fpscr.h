/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * SPU floating-point status and control register (FPSCR) state.
 *
 * Derived from RPCS3's SPU_FPSCR / FPSCR_EX definitions in
 * rpcs3/Emu/Cell/SPUThread.h at upstream revision
 * 4d5def5880217135554ed434cd5055cceebe9cc2 (GPL-2.0).
 *
 * Word order differs from RPCS3: words[0] is the architectural LEFT-most
 * 32-bit word and words[3] the RIGHT-most (RPCS3's v128._u32[] indices are
 * reversed). Logical single-precision slice 0 is the LEFT word; logical
 * double-precision slice 0 is the LEFT 64 bits (RPCS3 slice 1).
 *
 * This module only stores and updates architectural flags and rounding
 * fields. Arithmetic emulation must call it explicitly; it never changes the
 * host floating-point environment.
 */
#ifndef SPU_FPSCR_H
#define SPU_FPSCR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t words[4];
} spu_fpscr;

/* Exception flag bits. */
enum {
    /* Single-precision exceptions (per 32-bit slice, words[slice]). */
    SPU_FPSCR_SOVF = 1 << 2,     /* Overflow */
    SPU_FPSCR_SUNF = 1 << 1,     /* Underflow */
    SPU_FPSCR_SDIFF = 1 << 0,    /* Different (could be IEEE non-compliant) */
    /* Double-precision exceptions (per 64-bit slice, words[1 + slice]). */
    SPU_FPSCR_DOVF = 1 << 13,    /* Overflow */
    SPU_FPSCR_DUNF = 1 << 12,    /* Underflow */
    SPU_FPSCR_DINX = 1 << 11,    /* Inexact */
    SPU_FPSCR_DINV = 1 << 10,    /* Invalid operation */
    SPU_FPSCR_DNAN = 1 << 9,     /* NaN */
    SPU_FPSCR_DDENORM = 1 << 8   /* Denormal */
};

/* Double-precision rounding modes. */
enum {
    SPU_FPSCR_RN_NEAR = 0,
    SPU_FPSCR_RN_ZERO = 1,
    SPU_FPSCR_RN_PINF = 2,
    SPU_FPSCR_RN_MINF = 3
};

/* Clears all fields. state must be a valid pointer. */
void spu_fpscr_reset(spu_fpscr *state);

/*
 * Replaces the whole register (FSCRWR), clearing flags not present in words
 * and discarding reserved bits. state and words must be valid pointers;
 * words may alias state->words.
 */
void spu_fpscr_write(spu_fpscr *state, const uint32_t words[4]);

/*
 * Copies the exact current state (FSCRRD). state and words must be valid
 * pointers; words may alias state->words.
 */
void spu_fpscr_read(const spu_fpscr *state, uint32_t words[4]);

/*
 * Returns the rounding mode (0..3) of double slice 0 (LEFT) or 1 (RIGHT),
 * or -1 for a null state or out-of-range slice.
 */
int spu_fpscr_get_rounding(const spu_fpscr *state, unsigned slice);

/*
 * The mutators below return 0 on success, or -1 without modifying state for
 * a null state, out-of-range slice/mode, or unknown flag bits. Flags are
 * sticky: they are ORed into the existing value.
 */
int spu_fpscr_set_rounding(spu_fpscr *state, unsigned slice, unsigned mode);

/* slice 0..3 (0 = LEFT); flags from SPU_FPSCR_S*. */
int spu_fpscr_single_flags(spu_fpscr *state, unsigned slice, uint32_t flags);

/* slice 0..1 (0 = LEFT); flags from SPU_FPSCR_D*. */
int spu_fpscr_double_flags(spu_fpscr *state, unsigned slice, uint32_t flags);

/* Sets the single-precision divide-by-zero flag for slice 0..3 (0 = LEFT). */
int spu_fpscr_divide_by_zero(spu_fpscr *state, unsigned slice);

#ifdef __cplusplus
}
#endif

#endif /* SPU_FPSCR_H */
