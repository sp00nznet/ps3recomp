/*
 * ps3recomp - hand-assembled NV40 programs for tests and the host harness
 *
 * The smallest vertex and fragment programs a backend has to run, encoded
 * field by field, so a test can load "a guest shader" without a title. The
 * layouts are the ones the decompilers document (the header comment of
 * rsx_vp_decompiler.c, docs/RSX_FRAGMENT_PROGRAM.md, and the exec_if fields
 * in rsx_fp_decompiler.c), and libs/video/tests/test_shader_msl.c checks the
 * HLSL each program decompiles to says what was meant -- so a wrong bit here
 * fails there, by name, rather than as a blank frame in the host harness.
 *
 * Header-only: the host harness and the unit test both include it and
 * neither links the other.
 */
#ifndef PS3RECOMP_RSX_TEST_PROGRAMS_H
#define PS3RECOMP_RSX_TEST_PROGRAMS_H

#include "ps3emu/ps3types.h"

/* ---- vertex program ----------------------------------------------------------
 * Four little-endian words per instruction, as SET_TRANSFORM_PROGRAM delivers
 * them and rsx_vp_decompile reads them.
 *   D0: dst_tmp [15:20] (0x3F = none), vec_result [30]
 *   D1: src0 high 8 bits [0:7], input_src [8:11], vec_opcode [22:26],
 *       sca_opcode [27:31]
 *   D2: src2 high 6 bits [0:5], src1 [6:22], src0 low 9 bits [23:31]
 *   D3: end [0], dst [2:6], sca_dst_tmp [7:12], vec_writemask [13:16],
 *       sca_writemask [17:20], src2 low 11 bits [21:31]
 * A 17-bit source is reg_type [0:1] (1 = temp, 2 = input), reg [2:7], the
 * swizzle in [8:15] as w/z/y/x two bits each, neg [16].
 * ---------------------------------------------------------------------------*/

#define RSX_TEST_VP_OP_MOV      0x01u
#define RSX_TEST_VP_OP_TXL      0x19u
#define RSX_TEST_VP_SRC_TEMP    1u
#define RSX_TEST_VP_SRC_INPUT   2u
/* Component selectors, 0 = x .. 3 = w, in x,y,z,w lane order. */
#define RSX_TEST_VP_SWZ(x, y, z, w) \
    (((u32)(w) << 8) | ((u32)(z) << 10) | ((u32)(y) << 12) | ((u32)(x) << 14))
#define RSX_TEST_VP_SWZ_IDENT   RSX_TEST_VP_SWZ(0, 1, 2, 3)

static inline void rsx_test_put_le32(u8* p, u32 w)
{
    p[0] = (u8)w; p[1] = (u8)(w >> 8); p[2] = (u8)(w >> 16); p[3] = (u8)(w >> 24);
}

static inline u32 rsx_test_vp_src(u32 type, u32 reg, u32 swz)
{
    return type | ((reg & 0x3Fu) << 2) | swz;
}

/* One instruction at `p`: `vec_op` reading input register v[input] with
 * swizzle `swz` as src0, writing all four lanes of output register o[out],
 * no temp write, scalar unit idle. `end` sets the program's end bit.
 *
 * `tex_unit` is TXL's vertex-texture unit, D2 bits [8:9]. Those bits sit
 * inside the src1 field, which is why they can simply be OR-ed in: src1 here
 * is temp register 0, whose encoding leaves both of them clear, and TXL reads
 * its coordinate from src0 anyway. */
static inline void rsx_test_vp_vec_out_tex(u8* p, u32 vec_op, u32 input, u32 swz,
                                           u32 tex_unit, u32 out, int end)
{
    const u32 src0 = rsx_test_vp_src(RSX_TEST_VP_SRC_INPUT, 0, swz);
    const u32 src1 = rsx_test_vp_src(RSX_TEST_VP_SRC_TEMP, 0, RSX_TEST_VP_SWZ_IDENT);
    const u32 src2 = src1;
    const u32 d0 = (0x3Fu << 15)             /* dst_tmp: none            */
                 | (1u << 30);               /* vec_result: write o[dst] */
    const u32 d1 = (src0 >> 9)               /* src0 high 8 bits         */
                 | ((input & 0xFu) << 8)     /* input_src                */
                 | ((vec_op & 0x1Fu) << 22)  /* vec_opcode               */
                 | (0u << 27);               /* sca_opcode: NOP          */
    const u32 d2 = (src2 >> 11)              /* src2 high 6 bits         */
                 | ((src1 & 0x1FFFFu) << 6)  /* src1                     */
                 | ((tex_unit & 3u) << 8)    /* TXL texture unit         */
                 | ((src0 & 0x1FFu) << 23);  /* src0 low 9 bits          */
    const u32 d3 = (end ? 1u : 0u)           /* end                      */
                 | ((out & 0x1Fu) << 2)      /* dst (output register)    */
                 | (0x3Fu << 7)              /* sca_dst_tmp: none        */
                 | (0xFu << 13)              /* vec_writemask xyzw       */
                 | (0u << 17)                /* sca_writemask: none      */
                 | ((src2 & 0x7FFu) << 21);  /* src2 low 11 bits         */
    rsx_test_put_le32(p + 0, d0); rsx_test_put_le32(p + 4, d1);
    rsx_test_put_le32(p + 8, d2); rsx_test_put_le32(p + 12, d3);
}

static inline void rsx_test_vp_vec_out(u8* p, u32 vec_op, u32 input, u32 swz,
                                       u32 out, int end)
{
    rsx_test_vp_vec_out_tex(p, vec_op, input, swz, 0, out, end);
}

/* MOV o[out], v[input].swz */
static inline void rsx_test_vp_mov_out(u8* p, u32 input, u32 swz, u32 out, int end)
{
    rsx_test_vp_vec_out(p, RSX_TEST_VP_OP_MOV, input, swz, out, end);
}

/* TXL o[out], v[input].swz on vertex-texture unit `unit` -- a transform
 * program sampling a texture, which is what a title uses for per-instance
 * placement and displacement data. */
static inline void rsx_test_vp_txl_out(u8* p, u32 input, u32 swz, u32 unit,
                                       u32 out, int end)
{
    rsx_test_vp_vec_out_tex(p, RSX_TEST_VP_OP_TXL, input, swz, unit, out, end);
}

/* ---- fragment program --------------------------------------------------------
 * Four words per instruction in guest memory, each stored big-endian with its
 * 16-bit halves swapped (rsx_fp_read_word undoes both).
 *   word 0: end [0], out reg [1:6], write mask [9:12], input_src [13:16]
 *           (COL0 = 1, TC0 = 4), tex unit [17:20], opcode [24:29]
 *   word 1: src0 -- reg_type [0:1] (1 = input), reg [2:7], swizzle x/y/z/w
 *           at [9:16] two bits each, neg [17]; then the execution condition,
 *           exec_if lt/eq/gt at [18:20] (all three = unconditional, none =
 *           never, which is what an all-zero word means) and its swizzle at
 *           [21:28]
 *   words 2, 3: src1, src2
 * ---------------------------------------------------------------------------*/

#define RSX_TEST_FP_OP_MOV      0x01u
#define RSX_TEST_FP_OP_TEX      0x17u
#define RSX_TEST_FP_OPC(x)      ((u32)(x) << 24)
#define RSX_TEST_FP_MASK_XYZW   (0xFu << 9)
#define RSX_TEST_FP_END         1u
/* Destination register. Left out of the helpers below, which all write r0,
 * because that is the first colour export; the MRT program names r2, which
 * is the second one under 32-bit exports. */
#define RSX_TEST_FP_OUTREG(i)   ((u32)(i) << 1)
#define RSX_TEST_FP_INSRC(x)    ((u32)(x) << 13)
#define RSX_TEST_FP_INSRC_COL0  RSX_TEST_FP_INSRC(1)
#define RSX_TEST_FP_INSRC_TC0   RSX_TEST_FP_INSRC(4)
#define RSX_TEST_FP_TEXU(x)     ((u32)(x) << 17)
#define RSX_TEST_FP_SRC_INPUT   1u
#define RSX_TEST_FP_SWZ(x, y, z, w) \
    (((u32)(x) << 9) | ((u32)(y) << 11) | ((u32)(z) << 13) | ((u32)(w) << 15))
#define RSX_TEST_FP_SWZ_IDENT   RSX_TEST_FP_SWZ(0, 1, 2, 3)
#define RSX_TEST_FP_EXEC_ALWAYS \
    ((7u << 18) | (0u << 21) | (1u << 23) | (2u << 25) | (3u << 27))

/* Store host word `h` so that rsx_fp_read_word(p) returns it. */
static inline void rsx_test_fp_put_word(u8* p, u32 h)
{
    const u32 be = (h << 16) | (h >> 16);
    p[0] = (u8)(be >> 24); p[1] = (u8)(be >> 16); p[2] = (u8)(be >> 8); p[3] = (u8)be;
}

/* One unconditional instruction at `p` from its first two words; src1 and
 * src2 are zero. */
static inline void rsx_test_fp_instr(u8* p, u32 w0, u32 w1)
{
    rsx_test_fp_put_word(p + 0,  w0);
    rsx_test_fp_put_word(p + 4,  w1 | RSX_TEST_FP_EXEC_ALWAYS);
    rsx_test_fp_put_word(p + 8,  0);
    rsx_test_fp_put_word(p + 12, 0);
}

/* MOV r0, COL0.swz ; END */
static inline void rsx_test_fp_mov_r0_col0(u8* p, u32 swz)
{
    rsx_test_fp_instr(p, RSX_TEST_FP_OPC(RSX_TEST_FP_OP_MOV) | RSX_TEST_FP_MASK_XYZW |
                         RSX_TEST_FP_INSRC_COL0 | RSX_TEST_FP_END,
                         RSX_TEST_FP_SRC_INPUT | swz);
}

/* TEX r0, TC0, texture unit `unit` ; END */
static inline void rsx_test_fp_tex_r0_tc0(u8* p, u32 unit)
{
    rsx_test_fp_instr(p, RSX_TEST_FP_OPC(RSX_TEST_FP_OP_TEX) | RSX_TEST_FP_MASK_XYZW |
                         RSX_TEST_FP_INSRC_TC0 | RSX_TEST_FP_TEXU(unit) | RSX_TEST_FP_END,
                         RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
}

/* MOV r0, COL0.swz0 ; MOV r2, COL0.swz1 ; END -- 32 bytes, two instructions.
 *
 * The smallest program that writes a second colour target: with 32-bit
 * exports r0 is target A and r2 is target B, so the two swizzles come out on
 * two different surfaces. Anything that drops the second export leaves B
 * holding whatever the clear or the guest bytes put there, which is what the
 * host's --mrt mode and the MSL test both key on. */
static inline void rsx_test_fp_mrt_col0(u8* p, u32 swz0, u32 swz1)
{
    rsx_test_fp_instr(p + 0,
                      RSX_TEST_FP_OPC(RSX_TEST_FP_OP_MOV) | RSX_TEST_FP_MASK_XYZW |
                      RSX_TEST_FP_INSRC_COL0 | RSX_TEST_FP_OUTREG(0),
                      RSX_TEST_FP_SRC_INPUT | swz0);
    rsx_test_fp_instr(p + 16,
                      RSX_TEST_FP_OPC(RSX_TEST_FP_OP_MOV) | RSX_TEST_FP_MASK_XYZW |
                      RSX_TEST_FP_INSRC_COL0 | RSX_TEST_FP_OUTREG(2) | RSX_TEST_FP_END,
                      RSX_TEST_FP_SRC_INPUT | swz1);
}

#endif /* PS3RECOMP_RSX_TEST_PROGRAMS_H */
