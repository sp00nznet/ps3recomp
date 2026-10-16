/*
 * Conservative NV40 vertex-program position dataflow for optional native AO.
 *
 * rsx_ao_position_dataflow_rows() extends rsx_ao_position_program_rows() to
 * programs that compute o[0] lanes as DP4/DPH(P, c[base+n]) into temps and
 * later forward them to o[0] through MOV or zero-neutral MUL/ADD/MAD. It
 * returns 1 and stores the constant rows for output x, y, z, w only when all
 * four lanes provably carry projections of exactly the same point value by
 * contiguous rows; every unknown or ambiguous case returns 0 and leaves rows
 * untouched.
 *
 * Proof boundaries:
 *  - Straight-line analysis only; flow control or unknown opcodes reject.
 *  - Each temp lane is UNKNOWN, ZERO or a projection token. Tokens are
 *    immutable records of (opcode, point kind/index/swizzle, point lane
 *    generations at evaluation, constant row); copies survive later writes.
 *  - ZERO comes only from an unmodified direct constant whose current bits are
 *    exactly +0/-0, and from NV40 zero-annihilating MUL/MAD. No numeric
 *    interpretation or algebraic guessing is performed.
 *  - Predicated or saturated results are UNKNOWN. Predicated writes bump
 *    generations but never establish definite initialisation.
 */
#ifndef RSX_AO_POSITION_DATAFLOW_H
#define RSX_AO_POSITION_DATAFLOW_H

#include "rsx_ao_position_program.h"

#define RSX_AO_PD_UNKNOWN 0u
#define RSX_AO_PD_ZERO 1u
#define RSX_AO_PD_TOKEN0 2u
#define RSX_AO_PD_OP_MOV 1u
#define RSX_AO_PD_OP_MUL 2u
#define RSX_AO_PD_OP_ADD 3u
#define RSX_AO_PD_OP_MAD 4u

typedef struct rsx_ao_pd_token {
    uint32_t gen[4]; /* point lane generations (temp point only, else 0) */
    uint16_t row;
    uint16_t swizzle;
    uint8_t op, kind, index;
} rsx_ao_pd_token;

typedef struct rsx_ao_pd_state {
    uint32_t gen[256];
    uint8_t init[64];
    uint16_t val[256]; /* UNKNOWN, ZERO or TOKEN0 + token index */
    uint32_t counter;
    unsigned tokens;
    rsx_ao_pd_token token[RSX_AO_PP_MAX_INSTRUCTIONS];
} rsx_ao_pd_state;

/* Value of one source operand as read by destination component c. */
static inline unsigned rsx_ao_pd_source(const rsx_ao_pd_state *s, uint32_t src, unsigned abs_flag,
                                        uint32_t d1, uint32_t d3,
                                        const float constants[512][4], unsigned c)
{
    unsigned comp = (unsigned)((src >> (14u - 2u * c)) & 3u);
    int modified = ((src >> 16) & 1u) || abs_flag;

    if ((src & 3u) == 1u) {
        unsigned v = s->val[((src >> 2) & 63u) * 4u + comp];
        /* Negation/abs keep zero but destroy projection provenance. */
        return modified && v != RSX_AO_PD_ZERO ? RSX_AO_PD_UNKNOWN : v;
    }
    if ((src & 3u) == 3u && !modified && !((d3 >> 1) & 1u)) {
        unsigned index = (unsigned)((d1 >> 12) & 1023u);
        uint32_t bits;
        if (index >= RSX_AO_PP_CONSTANTS)
            return RSX_AO_PD_UNKNOWN;
        memcpy(&bits, &constants[index][comp], sizeof(bits));
        if (!(bits & 0x7fffffffu)) /* exactly +0 or -0 */
            return RSX_AO_PD_ZERO;
    }
    return RSX_AO_PD_UNKNOWN;
}

static inline void rsx_ao_pd_store(rsx_ao_pd_state *s, unsigned reg, unsigned mask,
                                   int unconditional, const unsigned res[4])
{
    unsigned c;
    rsx_ao_pp_write(s->gen, s->init, &s->counter, reg, mask, unconditional);
    for (c = 0; c < 4u; ++c)
        if ((mask >> c) & 1u)
            s->val[reg * 4u + c] = (uint16_t)res[c];
}

static inline int rsx_ao_position_dataflow_rows(const void *program, size_t bytes,
                                                const float constants[512][4], uint16_t rows[4])
{
    static const unsigned unknown[4] = {RSX_AO_PD_UNKNOWN, RSX_AO_PD_UNKNOWN,
                                        RSX_AO_PD_UNKNOWN, RSX_AO_PD_UNKNOWN};
    const unsigned char *p = (const unsigned char *)program;
    rsx_ao_pd_state s;
    const rsx_ao_pd_token *t[4];
    unsigned pos[4] = {0, 0, 0, 0}, written = 0, count, i, c;

    if (!program || !constants || !rows || !bytes || (bytes & 15u) ||
        bytes / 16u > RSX_AO_PP_MAX_INSTRUCTIONS)
        return 0;
    memset(&s, 0, sizeof(s));
    count = (unsigned)(bytes / 16u);

    for (i = 0; i < count; ++i, p += 16) {
        uint32_t d0 = rsx_ao_pp_u32(p), d1 = rsx_ao_pp_u32(p + 4);
        uint32_t d2 = rsx_ao_pp_u32(p + 8), d3 = rsx_ao_pp_u32(p + 12);
        unsigned vec_op = (unsigned)((d1 >> 22) & 31u);
        unsigned sca_op = (unsigned)((d1 >> 27) & 31u);
        uint32_t s0 = ((d2 >> 23) & 511u) | ((d1 & 255u) << 9);
        uint32_t s1 = (d2 >> 6) & 131071u;
        uint32_t s2 = ((d2 & 63u) << 11) | (d3 >> 21);
        unsigned vmask = rsx_ao_pp_mask(d3, 16), smask = rsx_ao_pp_mask(d3, 20);
        unsigned out = (unsigned)((d3 >> 2) & 31u);
        int vec_result = (int)((d0 >> 30) & 1u);
        int uncond = rsx_ao_pp_unconditional(d0);
        int plain = uncond && !((d0 >> 26) & 1u); /* not predicated / saturated */
        unsigned res[4] = {RSX_AO_PD_UNKNOWN, RSX_AO_PD_UNKNOWN, RSX_AO_PD_UNKNOWN,
                           RSX_AO_PD_UNKNOWN};

        if (((d3 & 1u) != 0u) != (i + 1u == count)) /* END exactly on the last */
            return 0;
        if (vec_op == 23u || vec_op == 24u || vec_op > 25u ||
            (sca_op >= 8u && sca_op <= 12u) || sca_op >= 17u)
            return 0;
        if (sca_op && !vec_result && out == 0u && smask) /* scalar position write */
            return 0;

        /* All operands are read here, before any write of this instruction. */
        if (vec_op == RSX_AO_PP_OP_DP4 || vec_op == RSX_AO_PP_OP_DPH) {
            unsigned kind, index, swizzle, row;
            if (rsx_ao_pp_decode_row(d0, d1, d2, d3, &kind, &index, &swizzle, &row)) {
                unsigned need = vec_op == RSX_AO_PP_OP_DP4 ? 15u : 7u;
                if (kind == 2u || (s.init[index] & need) == need) {
                    rsx_ao_pd_token *tok = &s.token[s.tokens];
                    memset(tok, 0, sizeof(*tok));
                    tok->op = (uint8_t)vec_op;
                    tok->kind = (uint8_t)kind;
                    tok->index = (uint8_t)index;
                    tok->swizzle = (uint16_t)swizzle;
                    tok->row = (uint16_t)row;
                    if (kind == 1u)
                        for (c = 0; c < 4u; ++c)
                            if ((need >> c) & 1u)
                                tok->gen[c] = s.gen[index * 4u + c];
                    for (c = 0; c < 4u; ++c)
                        res[c] = RSX_AO_PD_TOKEN0 + s.tokens;
                    ++s.tokens;
                }
            }
        } else if (plain && vec_op >= RSX_AO_PD_OP_MOV && vec_op <= RSX_AO_PD_OP_MAD) {
            for (c = 0; c < 4u; ++c) {
                unsigned a = rsx_ao_pd_source(&s, s0, (d0 >> 21) & 1u, d1, d3, constants, c);
                unsigned b = rsx_ao_pd_source(&s, s1, (d0 >> 22) & 1u, d1, d3, constants, c);
                unsigned e = rsx_ao_pd_source(&s, s2, (d0 >> 23) & 1u, d1, d3, constants, c);
                if (vec_op == RSX_AO_PD_OP_MOV)
                    res[c] = a;
                else if (vec_op == RSX_AO_PD_OP_MUL)
                    res[c] = a == RSX_AO_PD_ZERO || b == RSX_AO_PD_ZERO ? RSX_AO_PD_ZERO
                                                                        : RSX_AO_PD_UNKNOWN;
                else if (vec_op == RSX_AO_PD_OP_ADD)
                    res[c] = a == RSX_AO_PD_ZERO ? e : e == RSX_AO_PD_ZERO ? a : RSX_AO_PD_UNKNOWN;
                else
                    res[c] = a == RSX_AO_PD_ZERO || b == RSX_AO_PD_ZERO ? e : RSX_AO_PD_UNKNOWN;
            }
        }

        if (vec_op && vec_op != RSX_AO_PP_OP_ARL) {
            unsigned reg = (unsigned)((d0 >> 15) & 63u);
            if (reg != 63u)
                rsx_ao_pd_store(&s, reg, vmask, uncond, res);
            if (vec_result && out == 0u && vmask) {
                for (c = 0; c < 4u; ++c) {
                    if (!((vmask >> c) & 1u))
                        continue;
                    if (((written >> c) & 1u) || res[c] < RSX_AO_PD_TOKEN0)
                        return 0;
                    pos[c] = res[c] - RSX_AO_PD_TOKEN0;
                }
                written |= vmask;
            }
        }
        /* Scalar results are conservatively unknown; written after the vector. */
        if (sca_op && vec_result && ((d3 >> 7) & 63u) != 63u)
            rsx_ao_pd_store(&s, (unsigned)((d3 >> 7) & 63u), smask, uncond, unknown);
    }

    if (written != 15u)
        return 0;
    for (c = 0; c < 4u; ++c)
        t[c] = &s.token[pos[c]];
    if (t[0]->row + 3u >= RSX_AO_PP_CONSTANTS)
        return 0;
    for (c = 1; c < 4u; ++c)
        if (t[c]->op != t[0]->op || t[c]->kind != t[0]->kind || t[c]->index != t[0]->index ||
            t[c]->swizzle != t[0]->swizzle ||
            memcmp(t[c]->gen, t[0]->gen, sizeof(t[0]->gen)) != 0 ||
            t[c]->row != t[0]->row + c)
            return 0;
    for (c = 0; c < 4u; ++c)
        rows[c] = t[c]->row;
    return 1;
}

#endif
