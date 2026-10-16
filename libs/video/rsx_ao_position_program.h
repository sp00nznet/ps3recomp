/*
 * Conservative NV40 vertex-program position-matrix identification.
 *
 * rsx_ao_position_program_rows() recognises the common projection pattern
 *     o[0].x = dot(P, c[base+0]); o[0].y = dot(P, c[base+1]);
 *     o[0].z = dot(P, c[base+2]); o[0].w = dot(P, c[base+3]);
 * where P is one input register or one temp value that is provably the same
 * value at all four position writes. It returns 1 and stores the constant
 * rows for output x, y, z, w only when that proof succeeds; every unknown or
 * ambiguous case returns 0 and leaves rows untouched.
 *
 * Proof boundaries:
 *  - Programs are analysed linearly. Any flow-control scalar opcode or
 *    unknown opcode rejects the whole program, so straight-line order is
 *    execution order.
 *  - Temp values are never interpreted numerically. Each temp component
 *    write receives a fresh generation number; equal generations at every
 *    position write prove the same value was read. Inputs are immutable.
 *  - Predicated writes still bump generations (the value may have changed)
 *    but never establish definite initialisation.
 *  - This identifies projection usage only; it does not prove a given draw
 *    or camera is supported, and it extracts no numeric camera data.
 */
#ifndef RSX_AO_POSITION_PROGRAM_H
#define RSX_AO_POSITION_PROGRAM_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define RSX_AO_PP_MAX_INSTRUCTIONS 512u
#define RSX_AO_PP_CONSTANTS 512u
#define RSX_AO_PP_SWIZZLE_IDENTITY 0x1b00u
#define RSX_AO_PP_OP_DPH 6u
#define RSX_AO_PP_OP_DP4 7u
#define RSX_AO_PP_OP_ARL 13u

static inline uint32_t rsx_ao_pp_u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* Converts a D3 write-mask field (x at the highest bit, then y, z, w) into
 * x=bit0 .. w=bit3 order. */
static inline unsigned rsx_ao_pp_mask(uint32_t d3, unsigned x_bit)
{
    return (unsigned)(((d3 >> x_bit) & 1u) | (((d3 >> (x_bit - 1u)) & 1u) << 1) |
                      (((d3 >> (x_bit - 2u)) & 1u) << 2) |
                      (((d3 >> (x_bit - 3u)) & 1u) << 3));
}

static inline int rsx_ao_pp_unconditional(uint32_t d0)
{
    /* Condition code 7 means "always" even when the test bit is set. */
    return !((d0 >> 13) & 1u) || ((d0 >> 10) & 7u) == 7u;
}

/* Records a temp write: fresh generation for masked components; only
 * unconditional writes make those components definitely initialised. */
static inline void rsx_ao_pp_write(uint32_t *gen, uint8_t *init, uint32_t *counter,
                                   unsigned reg, unsigned mask, int unconditional)
{
    unsigned c;
    if (!mask)
        return;
    ++*counter;
    for (c = 0; c < 4u; ++c)
        if ((mask >> c) & 1u)
            gen[reg * 4u + c] = *counter;
    if (unconditional)
        init[reg] = (uint8_t)(init[reg] | mask);
}

/* Validates one position write's opcode, modifiers and operands. On success
 * reports the point source (kind 1 temp / 2 input, index, relevant swizzle
 * bits) and the constant row. Any deviation fails closed. */
static inline int rsx_ao_pp_decode_row(uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3,
                                       unsigned *kind, unsigned *index, unsigned *swizzle,
                                       unsigned *row)
{
    unsigned vec_op = (unsigned)((d1 >> 22) & 31u);
    uint32_t s0 = ((d2 >> 23) & 511u) | ((d1 & 255u) << 9);
    uint32_t s1 = (d2 >> 6) & 131071u;
    uint32_t point, constant, point_bits;

    if (vec_op != RSX_AO_PP_OP_DP4 && vec_op != RSX_AO_PP_OP_DPH)
        return 0;
    if (!rsx_ao_pp_unconditional(d0) || ((d0 >> 26) & 1u)) /* predicated / saturated */
        return 0;
    if ((d0 >> 21) & 3u) /* src0/src1 absolute */
        return 0;
    if ((d3 >> 1) & 1u) /* indirect constant addressing */
        return 0;
    if (((s0 >> 16) & 1u) || ((s1 >> 16) & 1u)) /* negated */
        return 0;

    /* The point must be input or temp; the constant can never be the point.
     * DPH is asymmetric (src1.w is added), so only DPH(point, constant). */
    if ((s1 & 3u) == 3u && ((s0 & 3u) == 1u || (s0 & 3u) == 2u)) {
        point = s0;
        constant = s1;
    } else if (vec_op == RSX_AO_PP_OP_DP4 && (s0 & 3u) == 3u &&
               ((s1 & 3u) == 1u || (s1 & 3u) == 2u)) {
        point = s1;
        constant = s0;
    } else {
        return 0;
    }

    if ((constant & 0xff00u) != RSX_AO_PP_SWIZZLE_IDENTITY)
        return 0;
    /* DP4 reads point.xyzw; DPH reads point.xyz only (w swizzle ignored). */
    point_bits = vec_op == RSX_AO_PP_OP_DP4 ? 0xff00u : 0xfc00u;
    if ((point & point_bits) != (RSX_AO_PP_SWIZZLE_IDENTITY & point_bits))
        return 0;

    *kind = (unsigned)(point & 3u);
    *index = *kind == 2u ? (unsigned)((d1 >> 8) & 15u) : (unsigned)((point >> 2) & 63u);
    *swizzle = (unsigned)(point & point_bits);
    *row = (unsigned)((d1 >> 12) & 1023u);
    return 1;
}

static inline int rsx_ao_position_program_rows(const void *program, size_t bytes,
                                               uint16_t rows[4])
{
    const unsigned char *p = (const unsigned char *)program;
    uint32_t gen[64 * 4];
    uint8_t init[64];
    uint32_t src_gen[4] = {0, 0, 0, 0};
    uint32_t counter = 0;
    unsigned row[4] = {0, 0, 0, 0};
    unsigned written = 0, op = 0, kind = 0, index = 0, swizzle = 0;
    size_t count, i;
    unsigned c;

    if (!p || !rows || bytes == 0 || bytes % 16u != 0 ||
        bytes / 16u > RSX_AO_PP_MAX_INSTRUCTIONS)
        return 0;
    count = bytes / 16u;
    memset(gen, 0, sizeof(gen));
    memset(init, 0, sizeof(init));

    for (i = 0; i < count; ++i) {
        const unsigned char *q = p + i * 16u;
        uint32_t d0 = rsx_ao_pp_u32(q), d1 = rsx_ao_pp_u32(q + 4);
        uint32_t d2 = rsx_ao_pp_u32(q + 8), d3 = rsx_ao_pp_u32(q + 12);
        unsigned vec_op = (unsigned)((d1 >> 22) & 31u);
        unsigned sca_op = (unsigned)((d1 >> 27) & 31u);
        unsigned dst_tmp = (unsigned)((d0 >> 15) & 63u);
        unsigned dst_out = (unsigned)((d3 >> 2) & 31u);
        unsigned sca_tmp = (unsigned)((d3 >> 7) & 63u);
        unsigned vec_mask = rsx_ao_pp_mask(d3, 16u);
        unsigned sca_mask = rsx_ao_pp_mask(d3, 20u);
        int vec_result = (int)((d0 >> 30) & 1u);
        int unconditional = rsx_ao_pp_unconditional(d0);

        /* END must be set on exactly the last instruction. */
        if (((d3 & 1u) != 0) != (i + 1u == count))
            return 0;
        /* Flow control (8..12, 17..20) and unknown scalar ops break the
         * straight-line assumption; unknown vector ops are unmodelled. */
        if ((sca_op >= 8u && sca_op <= 12u) || sca_op >= 17u)
            return 0;
        if (vec_op == 23u || vec_op == 24u || vec_op > 25u)
            return 0;
        /* Scalar results go to dstOut when vecResult is clear: never position. */
        if (!vec_result && dst_out == 0u && sca_op != 0u && sca_mask != 0u)
            return 0;

        if (vec_result && vec_op != 0u && dst_out == 0u && vec_mask != 0u) {
            unsigned k, idx, sw, r, need, comp;
            /* ARL and every other non-DP4/DPH op are rejected by decode. */
            if ((vec_mask & (vec_mask - 1u)) != 0u || (written & vec_mask) != 0u)
                return 0;
            if (!rsx_ao_pp_decode_row(d0, d1, d2, d3, &k, &idx, &sw, &r))
                return 0;
            need = vec_op == RSX_AO_PP_OP_DP4 ? 15u : 7u;
            /* Source is inspected before this instruction's own writes. */
            if (k == 1u && (init[idx] & need) != need)
                return 0;
            if (!written) {
                op = vec_op;
                kind = k;
                index = idx;
                swizzle = sw;
                for (c = 0; c < 4u; ++c)
                    src_gen[c] = k == 1u ? gen[idx * 4u + c] : 0u;
            } else {
                if (op != vec_op || kind != k || index != idx || swizzle != sw)
                    return 0;
                /* Any relevant temp mutation since the first row rejects. */
                if (k == 1u)
                    for (c = 0; c < 4u; ++c)
                        if (((need >> c) & 1u) && gen[idx * 4u + c] != src_gen[c])
                            return 0;
            }
            comp = vec_mask == 1u ? 0u : vec_mask == 2u ? 1u : vec_mask == 4u ? 2u : 3u;
            row[comp] = r;
            written |= vec_mask;
        }

        /* Record temp writes (ARL only changes the address register). */
        if (vec_op != 0u && vec_op != RSX_AO_PP_OP_ARL && dst_tmp != 63u)
            rsx_ao_pp_write(gen, init, &counter, dst_tmp, vec_mask, unconditional);
        if (sca_op != 0u && vec_result && sca_tmp != 63u)
            rsx_ao_pp_write(gen, init, &counter, sca_tmp, sca_mask, unconditional);
    }

    /* All four components written once, rows contiguous in output order
     * and inside the 512-entry runtime constant array. */
    if (written != 15u || row[0] > RSX_AO_PP_CONSTANTS - 4u)
        return 0;
    for (c = 1; c < 4u; ++c)
        if (row[c] != row[0] + c)
            return 0;
    for (c = 0; c < 4u; ++c)
        rows[c] = (uint16_t)row[c];
    return 1;
}

#endif /* RSX_AO_POSITION_PROGRAM_H */
