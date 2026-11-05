/*
 * ps3recomp - RSX Fragment Program (NV40 ISA) → HLSL decompiler
 *
 * See rsx_fp_decompiler.h and docs/RSX_FRAGMENT_PROGRAM.md.
 *
 * The encoding (4 words / 16 bytes per instruction; CONST sources carry an
 * inline 16-byte constant in the following slot) is taken verbatim from
 * Mesa/nouveau nvfx_shader.h. Bit positions are spelled out as named macros
 * below so this file is self-documenting.
 */

#include <stdlib.h>
#include "rsx_fp_decompiler.h"
#include <stdio.h>
#include <string.h>
#include "ps3emu/log_first.h"

/* ---- DWORD 0 (OPDEST) --------------------------------------------------- */
#define FP_END              (1u << 0)
#define FP_OUT_REG_SHIFT    1
#define FP_OUT_REG_MASK     (63u << 1)
#define FP_OUT_HALF         (1u << 7)
#define FP_COND_WRITE       (1u << 8)
#define FP_OUT_MASK_SHIFT   9          /* X@9 Y@10 Z@11 W@12 */
#define FP_INPUT_SRC_SHIFT  13
#define FP_INPUT_SRC_MASK   (15u << 13)
#define FP_TEX_UNIT_SHIFT   17
#define FP_TEX_UNIT_MASK    (15u << 17)
#define FP_OPCODE_SHIFT     24
#define FP_OPCODE_MASK      (0x3Fu << 24)
#define FP_OUT_NONE         (1u << 30)
#define FP_OUT_SAT          (1u << 31)

/* ---- DWORD 1/2/3 (SRC0/1/2) --------------------------------------------- */
#define FP_REG_TYPE_SHIFT   0
#define FP_REG_TYPE_MASK    (3u << 0)
#define   FP_REG_TYPE_TEMP  0
#define   FP_REG_TYPE_INPUT 1
#define   FP_REG_TYPE_CONST 2
#define FP_SRC_REG_SHIFT    2
#define FP_SRC_REG_MASK     (63u << 2)
#define FP_SRC_HALF         (1u << 8)
#define FP_SRC_SWZ_SHIFT    9          /* X@9 Y@11 Z@13 W@15, 2 bits each */
#define FP_SRC_NEGATE       (1u << 17)
#define FP_SRC0_ABS         (1u << 29) /* in DWORD1 */
#define FP_SRC1_ABS         (1u << 18) /* in DWORD2 */
#define FP_BRANCH           (1u << 31) /* DWORD2 bit31: instruction is a branch */

/* ---- Execution-condition / condition-code fields (SRC0 = DWORD1) ---------
 * The NV40 fragment ISA predicates each instruction on a condition register
 * (CC0/CC1). Layout verified against RPCS3's RSXFragmentProgram.h SRC0 union
 * (exec_if_lt@18, exec_if_eq@19, exec_if_gr@20, cond_swizzle@21..28,
 * cond_mod_reg_index@30, cond_reg_index@31) and OPDEST.set_cond@8. All three
 * exec bits set (0b111) == unconditional (the default); none set == never.
 * Dropped entirely until now: MEASURED present in 4 of this capture's 43
 * fragment programs (scratch/a010_reports/fragment/, cond_detail_out.txt) —
 * genuine SET_CC→exec_if producer/consumer pairs (incl. conditional TEX). */
#define FP_EXEC_IF_LT       (1u << 18)
#define FP_EXEC_IF_EQ       (1u << 19)
#define FP_EXEC_IF_GR       (1u << 20)
#define FP_COND_SWZ_SHIFT   21          /* X@21 Y@23 Z@25 W@27, 2 bits each */
#define FP_COND_MOD_REG     (1u << 30)  /* set_cond WRITE target: cc0/cc1     */
#define FP_COND_REG         (1u << 31)  /* exec_if READ source:   cc0/cc1     */

/* ---- Opcodes ------------------------------------------------------------ */
enum {
    OP_NOP=0x00, OP_MOV=0x01, OP_MUL=0x02, OP_ADD=0x03, OP_MAD=0x04,
    OP_DP3=0x05, OP_DP4=0x06, OP_DST=0x07, OP_MIN=0x08, OP_MAX=0x09,
    OP_SLT=0x0A, OP_SGE=0x0B, OP_SLE=0x0C, OP_SGT=0x0D, OP_SNE=0x0E,
    OP_SEQ=0x0F, OP_FRC=0x10, OP_FLR=0x11, OP_KIL=0x12, OP_PK4B=0x13,
    OP_UP4B=0x14, OP_DDX=0x15, OP_DDY=0x16, OP_TEX=0x17, OP_TXP=0x18,
    OP_TXD=0x19, OP_RCP=0x1A, OP_RSQ=0x1B, OP_EX2=0x1C, OP_LG2=0x1D,
    OP_LIT=0x1E, OP_LRP=0x1F, OP_STR=0x20, OP_SFL=0x21, OP_COS=0x22,
    OP_SIN=0x23, OP_PK2H=0x24, OP_UP2H=0x25, OP_POW=0x26, OP_PK4UB=0x27,
    OP_UP4UB=0x28, OP_PK2US=0x29, OP_UP2US=0x2A, OP_PKG=0x2C, OP_UPG=0x2D, OP_DP2A=0x2E, OP_TXL=0x2F,
    OP_TXB=0x31, OP_TEXBEM=0x33, OP_TXPBEM=0x34, OP_RFL=0x36, OP_DP2=0x38, OP_NRM=0x39, OP_DIV=0x3A,
    OP_DIVSQ=0x3B, OP_LIF=0x3C, OP_FENCT=0x3D, OP_FENCB=0x3E
};

const char* rsx_fp_opcode_name(u32 op)
{
    switch (op) {
    case OP_NOP: return "NOP"; case OP_MOV: return "MOV"; case OP_MUL: return "MUL";
    case OP_ADD: return "ADD"; case OP_MAD: return "MAD"; case OP_DP3: return "DP3";
    case OP_DP4: return "DP4"; case OP_DST: return "DST"; case OP_MIN: return "MIN";
    case OP_MAX: return "MAX"; case OP_SLT: return "SLT"; case OP_SGE: return "SGE";
    case OP_SLE: return "SLE"; case OP_SGT: return "SGT"; case OP_SNE: return "SNE";
    case OP_SEQ: return "SEQ"; case OP_FRC: return "FRC"; case OP_FLR: return "FLR";
    case OP_KIL: return "KIL"; case OP_DDX: return "DDX"; case OP_DDY: return "DDY";
    case OP_TEX: return "TEX"; case OP_TXP: return "TXP"; case OP_TXD: return "TXD";
    case OP_RCP: return "RCP"; case OP_RSQ: return "RSQ"; case OP_EX2: return "EX2";
    case OP_LG2: return "LG2"; case OP_LIT: return "LIT"; case OP_LRP: return "LRP";
    case OP_STR: return "STR"; case OP_SFL: return "SFL"; case OP_COS: return "COS";
    case OP_SIN: return "SIN"; case OP_POW: return "POW"; case OP_DP2A: return "DP2A";
    case OP_TXL: return "TXL"; case OP_TXB: return "TXB"; case OP_RFL: return "RFL";
    case OP_DIV: return "DIV"; case OP_DP2: return "DP2"; case OP_NRM: return "NRM";
    case OP_DIVSQ: return "DIVSQ"; case OP_LIF: return "LIF";
    case OP_PK4B: return "PK4B"; case OP_UP4B: return "UP4B"; case OP_PK2H: return "PK2H";
    case OP_UP2H: return "UP2H"; case OP_PK4UB: return "PK4UB"; case OP_UP4UB: return "UP4UB";
    case OP_PK2US: return "PK2US"; case OP_UP2US: return "UP2US"; case OP_PKG: return "PKG";
    case OP_UPG: return "UPG";
    case OP_FENCT: return "FENCT"; case OP_FENCB: return "FENCB";
    default:     return "?";
    }
}

/* ------------------------------------------------------------------------- */

u32 rsx_fp_read_word(const u8* p)
{
    u32 be = ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
    return (be << 16) | (be >> 16);   /* 16-bit half-word swap */
}

u32 rsx_fp_program_size(const u8* ucode, u32 max_bytes)
{
    if (!ucode) return 0;
    u32 off = 0;
    while (off + 16 <= max_bytes) {
        u32 w0 = rsx_fp_read_word(ucode + off + 0);
        u32 w1 = rsx_fp_read_word(ucode + off + 4);
        u32 w2 = rsx_fp_read_word(ucode + off + 8);
        u32 w3 = rsx_fp_read_word(ucode + off + 12);
        off += 16;
        /* A CONST source pulls an inline 16-byte constant slot. */
        if (((w1 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST ||
            ((w2 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST ||
            ((w3 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST) {
            if (off + 16 <= max_bytes) off += 16;
        }
        if (w0 & FP_END) return off;
    }
    return 0;
}

static int fp_instruction_has_constant(u32 w1, u32 w2, u32 w3)
{
    return
        ((w1 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) ==
            FP_REG_TYPE_CONST ||
        ((w2 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) ==
            FP_REG_TYPE_CONST ||
        ((w3 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) ==
            FP_REG_TYPE_CONST;
}

unsigned long long g_rsx_fp_nonfinite_constants = 0;

/* An inline fragment constant is never legitimately non-finite.  A NaN one
 * poisons every pixel the program touches, which on an FP16 HDR target shows
 * up as a saturated white fill rather than as an obviously broken draw.
 * Substitute zero and count it so the underlying bad read stays visible. */
u32 rsx_fp_sanitize_constant_word(u32 w)
{
    float f;
    memcpy(&f, &w, sizeof f);
    if (f == f && f <= 3.0e38f && f >= -3.0e38f)
        return w;
    g_rsx_fp_nonfinite_constants++;
    return 0;
}

int rsx_fp_collect_constants(
    const u8* ucode, u32 max_bytes, rsx_fp_constant_block* out)
{
    if (!ucode || !out)
        return -1;
    out->count = 0;

    u32 off = 0;
    while (off + 16 <= max_bytes) {
        const u32 w0 = rsx_fp_read_word(ucode + off + 0);
        const u32 w1 = rsx_fp_read_word(ucode + off + 4);
        const u32 w2 = rsx_fp_read_word(ucode + off + 8);
        const u32 w3 = rsx_fp_read_word(ucode + off + 12);
        off += 16;
        if (fp_instruction_has_constant(w1, w2, w3)) {
            if (off + 16 > max_bytes ||
                out->count >= RSX_FP_MAX_INLINE_CONSTANTS)
                return -1;
            for (u32 component = 0; component < 4; ++component)
                out->values[out->count][component] =
                    rsx_fp_sanitize_constant_word(
                        rsx_fp_read_word(ucode + off + component * 4));
            out->count++;
            off += 16;
        }
        if (w0 & FP_END)
            return (int)out->count;
    }
    return -1;
}

static u64 fp_hash_bytes(const void* data, u32 size, u64 hash)
{
    const u8* bytes = (const u8*)data;
    for (u32 i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

u64 rsx_fp_structural_hash(const u8* ucode, u32 max_bytes, u64 seed)
{
    if (!ucode)
        return 0;
    static const u32 structural_tag = 0x31535046u; /* "FPS1" */
    u64 hash = fp_hash_bytes(&structural_tag, sizeof(structural_tag), seed);
    u32 off = 0;
    u32 instruction_count = 0;
    u32 constant_count = 0;
    while (off + 16 <= max_bytes) {
        const u32 w0 = rsx_fp_read_word(ucode + off + 0);
        const u32 w1 = rsx_fp_read_word(ucode + off + 4);
        const u32 w2 = rsx_fp_read_word(ucode + off + 8);
        const u32 w3 = rsx_fp_read_word(ucode + off + 12);
        hash = fp_hash_bytes(ucode + off, 16, hash);
        instruction_count++;
        off += 16;
        if (fp_instruction_has_constant(w1, w2, w3)) {
            if (off + 16 > max_bytes ||
                constant_count >= RSX_FP_MAX_INLINE_CONSTANTS)
                return 0;
            constant_count++;
            off += 16;
        }
        if (w0 & FP_END) {
            hash = fp_hash_bytes(
                &instruction_count, sizeof(instruction_count), hash);
            hash = fp_hash_bytes(
                &constant_count, sizeof(constant_count), hash);
            return hash;
        }
    }
    return 0;
}

u64 rsx_fp_literal_source_hash(
    const u8* ucode, u32 max_bytes, u64 seed)
{
    const u64 structural =
        rsx_fp_structural_hash(ucode, max_bytes, seed);
    if (!structural)
        return 0;
    rsx_fp_constant_block constants;
    if (rsx_fp_collect_constants(ucode, max_bytes, &constants) < 0)
        return 0;
    u64 hash = structural;
    for (u32 slot = 0; slot < constants.count; ++slot) {
        float value[4];
        for (u32 component = 0; component < 4; ++component)
            memcpy(
                &value[component],
                &constants.values[slot][component],
                sizeof(value[component]));
        char literal[128];
        const int length = snprintf(
            literal, sizeof(literal), "float4(%g,%g,%g,%g)",
            value[0], value[1], value[2], value[3]);
        if (length < 0 || (u32)length >= sizeof(literal))
            return 0;
        hash = fp_hash_bytes(literal, (u32)length, hash);
    }
    return hash;
}

int rsx_fp_constant_ring_plan(
    u32 used, u32 capacity, u32 data_bytes,
    u32* out_offset, u32* out_allocation_bytes)
{
    if (!out_offset || !out_allocation_bytes || data_bytes == 0)
        return -1;
    if (data_bytes > 0xFFFFFF00u)
        return -1;
    const u32 allocation = (data_bytes + 255u) & ~255u;
    if (allocation > capacity)
        return -1;
    if (used > capacity || allocation > capacity - used)
        return 0;
    *out_offset = used;
    *out_allocation_bytes = allocation;
    return 1;
}

int rsx_vertex_constant_ring_plan(
    u32 used, u32 capacity, u32 block_bytes, u32* out_offset)
{
    if (!out_offset || block_bytes == 0 || (block_bytes & 255u) != 0 ||
        block_bytes > capacity)
        return -1;
    if (used > capacity || block_bytes > capacity - used)
        return 0;
    *out_offset = used;
    return 1;
}

/* Bounded string appender. */
typedef struct { char* p; u32 cap; u32 len; int ok; } Out;

static void out_puts(Out* o, const char* s)
{
    if (!o->ok) return;
    u32 n = (u32)strlen(s);
    if (o->len + n + 1 > o->cap) { o->ok = 0; return; }
    memcpy(o->p + o->len, s, n);
    o->len += n;
    o->p[o->len] = '\0';
}

/* Decoded view of one source word. */
typedef struct {
    u32 type;       /* FP_REG_TYPE_* */
    u32 index;
    int half;
    int negate;
    int abs;
    char swz[5];    /* e.g. "xyzw" */
} Src;

static void decode_src(u32 w, int abs_bit, Src* s)
{
    static const char comp[4] = {'x','y','z','w'};
    s->type   = (w & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT;
    s->index  = (w & FP_SRC_REG_MASK)  >> FP_SRC_REG_SHIFT;
    s->half   = (w & FP_SRC_HALF) ? 1 : 0;
    s->negate = (w & FP_SRC_NEGATE) ? 1 : 0;
    s->abs    = abs_bit ? 1 : 0;
    for (int i = 0; i < 4; i++)
        s->swz[i] = comp[(w >> (FP_SRC_SWZ_SHIFT + 2 * i)) & 3];
    s->swz[4] = '\0';
}

/* HLSL expression for the interpolated input selected by OPDEST.INPUT_SRC.
 * The backend's passthrough VS forwards COL0/COL1/FOG/TEXCOORD0..7; WPOS maps
 * to SV_POSITION (screen-space — an approximation of the RSX window coord). */
static const char* input_expr(u32 input_src)
{
    static const char* tc[8] = {
        "input.tc0", "input.tc1", "input.tc2", "input.tc3",
        "input.tc4", "input.tc5", "input.tc6", "input.tc7" };
    switch (input_src) {
    case 0x0: return "_rsx_wpos(input.position)"; /* WPOS (window origin applied below) */
    /* RSX clamps fragment color-input reads before swizzle/abs/negate.
     * Texture coordinates and the vertex output registers remain unbounded. */
    case 0x1: return "saturate(input.col0)"; /* COL0 */
    case 0x2: return "saturate(input.col1)"; /* COL1 */
    case 0x3: return "input.fog";      /* FOGC */
    case 0xC: return "input.tc8";      /* TC8 (VP o15) */
    case 0xD: return "input.tc9";      /* TC9 (VP o6) */
    /* FACING: +1 on a front face, -1 on a back face (RPCS3's `ssa`). Two-sided
     * materials multiply the interpolated normal by it; read as zero, inFAMOUS
     * 2's foliage and character heads normalised a null vector into the
     * G-buffer and lit black. */
    case 0xE: return "(input._rsx_front ? float4(1,1,1,1) : float4(-1,-1,-1,-1))";
    default:
        if (input_src >= 0x4 && input_src <= 0xB) return tc[input_src - 0x4]; /* TC0..7 */
        return "float4(0,0,0,0)";
    }
}

/* Build the swizzled/negated/abs'd HLSL for one source into `buf`. */
static void emit_src(const Src* s, u32 input_src, int persp, const float* k, int has_k,
                     int buffered, u32 constant_slot, char* buf, u32 bufsz)
{
    char base[96];
    if (s->type == FP_REG_TYPE_TEMP) {
        snprintf(base, sizeof(base), "%s[%u]", s->half ? "h" : "r", s->index);
    } else if (s->type == FP_REG_TYPE_INPUT) {
        /* SRC2.perspective_corr on a texcoord read (TC0..TC9) multiplies it
         * by 1/w (RPCS3 FragmentProgramDecompiler: "Perspective correct flag
         * multiplies the result by 1/w"). A vertex program that writes uv*w
         * relies on it: inFAMOUS 2's water samples its screen copy that way,
         * and read raw it lands far outside the copy and the river is black. */
        if (persp && input_src >= 0x4 && input_src <= 0xD)
            snprintf(base, sizeof(base), "(%s * _rsx_inv_w(input.position))",
                     input_expr(input_src));
        else
            snprintf(base, sizeof(base), "%s", input_expr(input_src));
    } else { /* CONST */
        if (buffered && has_k)
            snprintf(
                base, sizeof(base), "fp_constants[%u]", constant_slot);
        else if (has_k)
            snprintf(base, sizeof(base), "float4(%g,%g,%g,%g)",
                     k[0], k[1], k[2], k[3]);
        else
            snprintf(base, sizeof(base), "float4(0,0,0,0)");
    }

    char swz[160];
    snprintf(swz, sizeof(swz), "(%s).%s", base, s->swz);

    const char* pre = "";
    const char* post = "";
    if (s->abs)    { pre = "abs("; post = ")"; }
    if (s->negate) snprintf(buf, bufsz, "-(%s%s%s)", pre, swz, post);
    else           snprintf(buf, bufsz, "%s%s%s", pre, swz, post);
}

/* Dest write-mask letters from OPDEST bits 9..12 (X..W). */
static void dest_mask(u32 op0, char* m)
{
    int n = 0;
    if (op0 & (1u << (FP_OUT_MASK_SHIFT + 0))) m[n++] = 'x';
    if (op0 & (1u << (FP_OUT_MASK_SHIFT + 1))) m[n++] = 'y';
    if (op0 & (1u << (FP_OUT_MASK_SHIFT + 2))) m[n++] = 'z';
    if (op0 & (1u << (FP_OUT_MASK_SHIFT + 3))) m[n++] = 'w';
    if (n == 0) { m[0] = 'x'; m[1] = 'y'; m[2] = 'z'; m[3] = 'w'; n = 4; }
    m[n] = '\0';
}

/* CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS: when set, fragment output is fp32
 * (color = r0); when clear, output is fp16 (color = h0). Matches RPCS3
 * FragmentProgramDecompiler::BuildCode (gcm_enums.h). */
#define FP_CTRL_32BIT_EXPORTS 0x40u

/* The four colour exports live at fixed register indices, one set per export
 * width: r0/r2/r3/r4 with 32-bit exports and h0/h4/h6/h8 with 16-bit ones
 * (rsx_commands.h, SHADER_CONTROL 0x40; RPCS3 FragmentProgramDecompiler's
 * BuildCode table). Everything else in the file is a plain temp. */
#define FP_MAX_COLOR_TARGETS 4
static const u32 fp_export_reg[2][FP_MAX_COLOR_TARGETS] = {
    { 0, 2, 3, 4 },   /* 32-bit exports: r */
    { 0, 4, 6, 8 },   /* 16-bit exports: h */
};

/* Which colour target a destination register is, or -1 for a plain temp. */
static int fp_export_target(u32 index, int half)
{
    for (int t = 0; t < FP_MAX_COLOR_TARGETS; t++)
        if (fp_export_reg[half][t] == index) return t;
    return -1;
}

int rsx_fp_decompile(const u8* ucode, u32 max_bytes, u32 ctrl, char* out, u32 out_size)
{
    return rsx_fp_decompile_ex(ucode, max_bytes, ctrl, 0u, out, out_size);
}

void rsx_fp_shadow_desc_from_texture(rsx_fp_shadow_desc* out, u32 format,
    u32 dimension, u32 cube, u32 mips, u32 filter, u32 address, u32 control0)
{
    memset(out, 0, sizeof *out);
    const u32 base = format & 0x9Fu, func = (address >> 28) & 15u;
    if (!(control0 & 0x80000000u) || (control0 & 4u) ||
        (base != 0x90 && base != 0x91 && base != 0x92 && base != 0x93) ||
        func == 0 || func == 7) return;
    const u32 minf = (filter >> 16) & 7u, magf = (filter >> 24) & 7u;
    const int minlin = minf == 2 || minf == 4 || minf == 6;
    const int maglin = magf == 2;
    out->enabled = (base == 0x91 || base == 0x93 || func > 7 || dimension != 2 || cube || mips > 1 ||
        minf < 1 || minf > 6 || magf < 1 || magf > 2 || minlin != maglin) ? 2 : 1;
    out->func = (u8)func;
    out->linear = (u8)minlin;
    out->unnormalized = (u8)((format & 0x40u) != 0);
}

u32 rsx_fp_texture_units(const u8* ucode, u32 max_bytes)
{
    u32 units = 0;
    if (!ucode) return 0;
    for (u32 off = 0; off + 16 <= max_bytes;) {
        const u32 w0 = rsx_fp_read_word(ucode + off);
        const u32 w1 = rsx_fp_read_word(ucode + off + 4);
        const u32 w2 = rsx_fp_read_word(ucode + off + 8);
        const u32 w3 = rsx_fp_read_word(ucode + off + 12);
        const u32 op = (w0 >> 24) & 63u;
        if (!(w2 & FP_BRANCH) && (op == OP_TEX || op == OP_TXP ||
            op == OP_TXB || op == OP_TXL || op == OP_TXD ||
            op == OP_TEXBEM || op == OP_TXPBEM))
            units |= 1u << ((w0 >> FP_TEX_UNIT_SHIFT) & 15u);
        off += 16;
        if (!(w2 & FP_BRANCH) && ((w1 & 3u) == 2 || (w2 & 3u) == 2 || (w3 & 3u) == 2))
            off += 16;
        if (w0 & FP_END) break;
    }
    return units;
}

static void fp_shadow_helper(Out* p, u32 unit, u32 cube_mask,
                              const rsx_fp_shadow_desc* d)
{
    char tex[32], text[4096];
    snprintf(tex, sizeof tex, cube_mask ? "rsx_tex%u" : "rsx_tex[%u]", unit);
    /* The RSX compares the stored depth with the reference (depth FUNC reference), the reverse of
     * the ARB_shadow order (reference FUNC depth). See docs/SHADOW_COMPARISON.md. */
    const char* cmp[] = { "0.0", "depth < reference", "depth == reference",
        "depth <= reference", "depth > reference", "depth != reference",
        "depth >= reference", "1.0" };
    snprintf(text, sizeof text,
        "float _rsx_shadow_cmp%u(float reference, float depth) { return (float)(%s); }\n"
        "float4 _rsx_shadow%u(float3 coordinate) {\n"
        "    float reference = saturate(coordinate.z);\n",
        unit, cmp[d->func], unit);
    out_puts(p, text);
    if (d->linear || d->unnormalized) {
        snprintf(text, sizeof text, "    uint width, height; %s.GetDimensions(width, height);\n"
            "    float2 size = float2(width, height);\n", tex);
        out_puts(p, text);
    }
    out_puts(p, d->unnormalized ? "    float2 uv = coordinate.xy / size;\n" :
                                     "    float2 uv = coordinate.xy;\n");
    if (!d->linear) {
        snprintf(text, sizeof text,
            "    float depth = %s.SampleLevel(rsx_samp[%u], uv, 0.0).r;\n"
            "    return (float4)_rsx_shadow_cmp%u(reference, depth);\n}\n",
            tex, unit, unit);
        out_puts(p, text);
        return;
    }
    out_puts(p, "    float2 position = uv * size - 0.5;\n"
        "    float2 base = floor(position); float2 fraction = position - base;\n");
    for (u32 tap = 0; tap < 4; tap++) {
        snprintf(text, sizeof text,
            "    float d%u = %s.SampleLevel(rsx_samp[%u], (base + float2(%s, %s)) / size, 0.0).r;\n"
            "    float c%u = _rsx_shadow_cmp%u(reference, d%u);\n",
            tap, tex, unit, (tap & 1) ? "1.5" : "0.5", (tap & 2) ? "1.5" : "0.5", tap, unit, tap);
        out_puts(p, text);
    }
    out_puts(p, "    return (float4)lerp(lerp(c0, c1, fraction.x), lerp(c2, c3, fraction.x), fraction.y);\n}\n");
}

/* EXP_TEX overrides gamma on compatible normalized color formats. Metadata
 * has already excluded formats without gamma support. */
u32 rsx_fp_texture_gamma_units(const u8* ucode, u32 max_bytes,
                                const u8 gamma_masks[16])
{
    if (!ucode || !gamma_masks) return 0;
    u32 units = 0;
    for (u32 off = 0; off + 16 <= max_bytes;) {
        const u32 w0 = rsx_fp_read_word(ucode + off);
        const u32 w1 = rsx_fp_read_word(ucode + off + 4);
        const u32 w2 = rsx_fp_read_word(ucode + off + 8);
        const u32 w3 = rsx_fp_read_word(ucode + off + 12);
        const u32 op = (w0 >> 24) & 63u, unit = (w0 >> FP_TEX_UNIT_SHIFT) & 15u;
        if (!(w2 & FP_BRANCH) && !(w0 & (1u << 21)) && gamma_masks[unit] &&
            (op == OP_TEX || op == OP_TXP || op == OP_TXB || op == OP_TXL ||
             op == OP_TXD || op == OP_TEXBEM || op == OP_TXPBEM))
            units |= 1u << unit;
        off += 16;
        if (!(w2 & FP_BRANCH) && ((w1 & 3u) == 2 || (w2 & 3u) == 2 || (w3 & 3u) == 2)) off += 16;
        if (w0 & FP_END) return units;
    }
    return 0;
}

static void fp_gamma_helper(Out* out)
{
    out_puts(out,
        "float4 _rsx_gamma_input(float4 v, uint mask) {\n"
        "    float4 lo = v / 12.92;\n"
        "    float4 hi = pow(max((v + 0.055) / 1.055, (float4)0), (float4)2.4);\n"
        "    float4 decoded = float4(v.r > 0.04045 ? hi.r : lo.r,\n"
        "                            v.g > 0.04045 ? hi.g : lo.g,\n"
        "                            v.b > 0.04045 ? hi.b : lo.b,\n"
        "                            v.a > 0.04045 ? hi.a : lo.a);\n"
        "    return float4((mask & 1u) ? decoded.r : v.r,\n"
        "                  (mask & 2u) ? decoded.g : v.g,\n"
        "                  (mask & 4u) ? decoded.b : v.b,\n"
        "                  (mask & 8u) ? decoded.a : v.a);\n}\n");
}

/* Texture sign expansion: units a TEX/TXP/TXB/TXL fetches from whose
 * expand mask is set. The same walk as the gamma one. */
u32 rsx_fp_texture_expand_units(const u8* ucode, u32 max_bytes,
                                 const u8 expand_masks[16])
{
    if (!ucode || !expand_masks) return 0;
    u32 units = 0;
    for (u32 off = 0; off + 16 <= max_bytes;) {
        const u32 w0 = rsx_fp_read_word(ucode + off);
        const u32 w1 = rsx_fp_read_word(ucode + off + 4);
        const u32 w2 = rsx_fp_read_word(ucode + off + 8);
        const u32 w3 = rsx_fp_read_word(ucode + off + 12);
        const u32 op = (w0 >> 24) & 63u, unit = (w0 >> FP_TEX_UNIT_SHIFT) & 15u;
        if (!(w2 & FP_BRANCH) && expand_masks[unit] &&
            (op == OP_TEX || op == OP_TXP || op == OP_TXB || op == OP_TXL))
            units |= 1u << unit;
        off += 16;
        if (!(w2 & FP_BRANCH) && ((w1 & 3u) == 2 || (w2 & 3u) == 2 || (w3 & 3u) == 2)) off += 16;
        if (w0 & FP_END) return units;
    }
    return 0;
}

/* Units a TEX/TXP/TXB/TXL reads with EXP_TEX (OPDEST bit 21, _bx2). */
u32 rsx_fp_texture_bx2_units(const u8* ucode, u32 max_bytes)
{
    if (!ucode) return 0;
    u32 units = 0;
    for (u32 off = 0; off + 16 <= max_bytes;) {
        const u32 w0 = rsx_fp_read_word(ucode + off);
        const u32 w1 = rsx_fp_read_word(ucode + off + 4);
        const u32 w2 = rsx_fp_read_word(ucode + off + 8);
        const u32 w3 = rsx_fp_read_word(ucode + off + 12);
        const u32 op = (w0 >> 24) & 63u, unit = (w0 >> FP_TEX_UNIT_SHIFT) & 15u;
        if (!(w2 & FP_BRANCH) && (w0 & (1u << 21)) &&
            (op == OP_TEX || op == OP_TXP || op == OP_TXB || op == OP_TXL))
            units |= 1u << unit;
        off += 16;
        if (!(w2 & FP_BRANCH) && ((w1 & 3u) == 2 || (w2 & 3u) == 2 || (w3 & 3u) == 2)) off += 16;
        if (w0 & FP_END) return units;
    }
    return 0;
}

static void fp_expand_helper(Out* out)
{
    out_puts(out,
        "float4 _rsx_expand_input(float4 v, uint mask, bool bx2) {\n"
        "    float4 e = bx2 ? v * 2.0 - 1.0\n"
        "             : (mask & 16u) ? (floor(v * 65535.0 + 0.5) - 32768.0) / 32767.0\n"
        "                            : (floor(v * 255.0 + 0.5) - 128.0) / 127.0;\n"
        "    return float4((mask & 1u) ? e.r : v.r,\n"
        "                  (mask & 2u) ? e.g : v.g,\n"
        "                  (mask & 4u) ? e.b : v.b,\n"
        "                  (mask & 8u) ? e.a : v.a);\n}\n");
}

static int rsx_fp_decompile_internal(
    const u8* ucode, u32 max_bytes, u32 ctrl, u32 tex_cube_mask, u32 tex_vol_mask,
    int buffered, const rsx_fp_shadow_desc* shadow, const u8 gamma_masks[16],
    const u8* expand_masks,
    char* out, u32 out_size, u32* out_constant_count)
{
    if (!ucode || !out || out_size == 0) return -1;
    /* Cube and volume units are declared on their own t-register; with none
     * the 2D array keeps every 2D-only program byte-identical. */
    const u32 tex_sep_mask = tex_cube_mask | tex_vol_mask;
    const u32 gamma_units = rsx_fp_texture_gamma_units(ucode, max_bytes, gamma_masks);
    const u32 expand_units = rsx_fp_texture_expand_units(ucode, max_bytes, expand_masks);
    /* _bx2 (EXP_TEX) fetches expand whatever unit they read: the modifier is
     * the instruction's, not the texture's. inFAMOUS 2 stores tangent-space
     * normal maps and its occlusion-cone LUTs unsigned and reads them with
     * TEX_bx2; about a quarter of its fragment programs do. */
    const u32 bx2_units = rsx_fp_texture_bx2_units(ucode, max_bytes);

    const u32 sampled_units = rsx_fp_texture_units(ucode, max_bytes);
    if (shadow) for (u32 u = 0; u < 16; u++)
        if ((sampled_units & (1u << u)) && shadow[u].enabled &&
            (shadow[u].enabled != 1 || shadow[u].func > 7 ||
             shadow[u].linear > 1 || shadow[u].unnormalized > 1 ||
             (tex_sep_mask & (1u << u)))) {
            static u32 n_layout_errors;
            if (PS3_LOG_FIRST(n_layout_errors, 32))
                fprintf(stderr, "[RSX fp] unsupported depth comparison layout on texture unit %u\n", u);
            return -1;
        }
    u32 buffered_constant_count = 0;
    if (buffered) {
        rsx_fp_constant_block constants;
        const int result =
            rsx_fp_collect_constants(ucode, max_bytes, &constants);
        if (result < 0)
            return -1;
        buffered_constant_count = constants.count;
    }
    if (out_constant_count)
        *out_constant_count = buffered_constant_count;

    /* The BODY is emitted first and the preamble is inserted in front of it
     * afterwards. main's signature names the return type, and the return type
     * is a struct as soon as the program turns out to write a second colour
     * export -- which is only known once every instruction has been decoded.
     * Emitting the preamble here would mean guessing it. */
    Out o = { out, out_size, 0, 1 };
    out[0] = '\0';
    int uses_pack = 0;   /* a pack or unpack opcode was decoded: the preamble gets its helpers */
    out_puts(&o,
        "    float4 r[48]; float4 h[48];\n"
        /* Fully initialise both register files: RSX programs routinely read a
         * register lane before writing it (the hardware reads undefined), but
         * HLSL rejects use-before-init (error X4000). Zero everything. */
        "    [unroll] for (int _i = 0; _i < 48; _i++) { r[_i] = (float4)0; h[_i] = (float4)0; }\n"
        /* Condition-code registers for exec_if predication / set_cond. Reset
         * to 0 (NV40 CC power-up state). Dead in non-predicated programs. */
        "    float4 cc0 = (float4)0; float4 cc1 = (float4)0;\n");
    /* RSX_FP_DEBUG=<structural hash hex>:<n>:<reg[.swz]>: in the program
     * with that structural hash (rsx_fp_structural_hash with the FNV offset
     * basis as seed, the "fp" the engine's pipeline log and shader dump
     * print), copy register <reg> (r3, h1, h1.wwww ...) right after its
     * <n>-th translated instruction and export it as colour target 0 with
     * alpha 1. An intermediate of a long shader is then seen on screen
     * instead of reasoned out from the listing. */
    static long dbg_n = -2; static char dbg_reg[32]; static u64 dbg_hash;
    if (dbg_n == -2) {
        dbg_n = -1;
        const char* e = getenv("RSX_FP_DEBUG");
        if (e) {
            char* d;
            dbg_hash = strtoull(e, &d, 16);
            if (*d == ':') dbg_n = strtol(d + 1, &d, 10);
            if (*d == ':') snprintf(dbg_reg, sizeof dbg_reg, "%s", d + 1);
            if (!(dbg_reg[0] == 'r' || dbg_reg[0] == 'h')) dbg_n = -1;
        }
    }
    const int dbg_here = dbg_n >= 0 &&
        rsx_fp_structural_hash(ucode, max_bytes, 1469598103934665603ull) == dbg_hash;
    u32 dbg_count = 0;
    int dbg_done = 0;
    if (dbg_here) out_puts(&o, "    float4 _rsx_dbg = (float4)0;\n");

    /* Which colour export each register file received a write to, indexed
     * [half][target]. The whole file is tracked, not just target 0, because
     * the highest one written decides how many SV_TARGETs come out. */
    int wrote[2][FP_MAX_COLOR_TARGETS];
    memset(wrote, 0, sizeof wrote);
    int count = 0;
    u32 off = 0;
    u32 constant_slot = 0;

    /* Open IFE/LOOP/REP blocks: the byte offset each one's else branch starts
     * at (0 = none) and where it ends. See the branch case below. */
    enum { FP_MAX_BLOCKS = 32 };
    u32 blk_else[FP_MAX_BLOCKS], blk_end[FP_MAX_BLOCKS];
    int blk_loop[FP_MAX_BLOCKS];
    int n_blk = 0, n_loop = 0, loops_open = 0;

    while (off + 16 <= max_bytes) {
        /* Close blocks ending here, innermost first, then open an else. Ends
         * before elses, as RPCS3's FragmentProgramDecompiler::Decompile does. */
        while (n_blk > 0 && blk_end[n_blk - 1] == off) {
            out_puts(&o, "    }\n");
            loops_open -= blk_loop[--n_blk];
        }
        if (n_blk > 0 && blk_else[n_blk - 1] == off) {
            out_puts(&o, "    } else {\n");
            blk_else[n_blk - 1] = 0;
        }
        u32 w0 = rsx_fp_read_word(ucode + off + 0);
        u32 w1 = rsx_fp_read_word(ucode + off + 4);
        u32 w2 = rsx_fp_read_word(ucode + off + 8);
        u32 w3 = rsx_fp_read_word(ucode + off + 12);
        off += 16;
        count++;

        u32 opcode    = (w0 & FP_OPCODE_MASK) >> FP_OPCODE_SHIFT;
        u32 input_src = (w0 & FP_INPUT_SRC_MASK) >> FP_INPUT_SRC_SHIFT;
        u32 tex_unit  = (w0 & FP_TEX_UNIT_MASK) >> FP_TEX_UNIT_SHIFT;
        if (!(w2 & FP_BRANCH) && shadow && shadow[tex_unit].enabled &&
            (opcode == OP_TXB || opcode == OP_TXL || opcode == OP_TXD ||
             opcode == OP_TEXBEM || opcode == OP_TXPBEM)) {
            static u32 n_fetch_errors;
            if (PS3_LOG_FIRST(n_fetch_errors, 32))
                fprintf(stderr, "[RSX fp] unsupported depth comparison fetch on texture unit %u\n", tex_unit);
            return -1;
        }
        int is_branch = (w2 & FP_BRANCH) ? 1 : 0;

        /* Execution condition (exec_if) + condition-code write (set_cond).
         * Faithful to RPCS3 FragmentProgramDecompiler {GetRawCond, AddCodeCond,
         * SetDst}: all-set == unconditional, none-set == never, otherwise a
         * per-component select of the result vs the destination, gated by a
         * sign test of the cond-swizzled CC register. */
        int exec_lt   = (w1 & FP_EXEC_IF_LT) ? 1 : 0;
        int exec_eq   = (w1 & FP_EXEC_IF_EQ) ? 1 : 0;
        int exec_gr   = (w1 & FP_EXEC_IF_GR) ? 1 : 0;
        int is_uncond = (exec_lt && exec_eq && exec_gr);
        int is_never  = (!exec_lt && !exec_eq && !exec_gr);
        int set_cond  = (w0 & FP_COND_WRITE) ? 1 : 0;
        u32 cc_read   = (w1 & FP_COND_REG)     ? 1u : 0u;
        u32 cc_write  = (w1 & FP_COND_MOD_REG) ? 1u : 0u;
        static const char comp_c[4] = {'x','y','z','w'};
        char cswz[5];
        for (int ci = 0; ci < 4; ci++)
            cswz[ci] = comp_c[(w1 >> (FP_COND_SWZ_SHIFT + 2 * ci)) & 3];
        cswz[4] = '\0';
        /* HLSL comparison operator for the exec_if sign mask (matches the
         * oracle's SGE/SLE/SNE/SGT/SLT/SEQ selection in GetRawCond). */
        const char* cmp_op = "==";
        if      (exec_gr && exec_eq) cmp_op = ">=";
        else if (exec_lt && exec_eq) cmp_op = "<=";
        else if (exec_gr && exec_lt) cmp_op = "!=";
        else if (exec_gr)            cmp_op = ">";
        else if (exec_lt)            cmp_op = "<";
        else                         cmp_op = "==";  /* exec_eq only */

        Src s0, s1, s2;
        decode_src(w1, w1 & FP_SRC0_ABS, &s0);
        decode_src(w2, w2 & FP_SRC1_ABS, &s1);
        decode_src(w3, 0,                &s2);

        /* Inline constant: any CONST source pulls the next 16 bytes as a
         * float4 literal and advances past it. */
        float k[4] = {0,0,0,0};
        int has_k = 0;
        if (s0.type == FP_REG_TYPE_CONST || s1.type == FP_REG_TYPE_CONST ||
            s2.type == FP_REG_TYPE_CONST) {
            if (off + 16 <= max_bytes) {
                for (int i = 0; i < 4; i++) {
                    u32 cw = rsx_fp_sanitize_constant_word(
                        rsx_fp_read_word(ucode + off + i * 4));
                    memcpy(&k[i], &cw, 4);
                }
                off += 16;
            }
            has_k = 1;
        }

        /* SRC2 bit 31 is perspective_corr for the instruction's input read
         * (RPCS3 RSXFragmentProgram.h SRC2); on a branch the word is an
         * address instead. */
        const int persp = !is_branch && (w3 >> 31) != 0;
        char a[200], b[200], c[200];
        emit_src(
            &s0, input_src, persp, k, has_k, buffered, constant_slot,
            a, sizeof(a));
        emit_src(
            &s1, input_src, persp, k, has_k, buffered, constant_slot,
            b, sizeof(b));
        emit_src(
            &s2, input_src, persp, k, has_k, buffered, constant_slot,
            c, sizeof(c));
        if (has_k)
            constant_slot++;

        /* Flow control: SRC1.branch set, and OPDEST's opcode field selects
         * BRK/CAL/IFE/LOOP/REP/RET (RPCS3 RSXFragmentProgram.h, 0x40..0x45).
         * IFE carries the else and end addresses in SRC1/SRC2 [0:30] as word
         * offsets; LOOP/REP carry end/init/increment counters in SRC1 [2:9],
         * [10:17], [19:26] and the end address in SRC2. The condition is the
         * exec_if test on the first component of the condition swizzle.
         * Skipping these ran both sides of every if/else: Crazy Taxi's uber
         * material shader then overwrote its lit colour with a COLOR0 the
         * vertex program never writes, and the whole city drew black. */
        if (is_branch) {
            char cond[64], line[160];
            if (is_uncond)
                snprintf(cond, sizeof cond, "true");
            else if (is_never)
                snprintf(cond, sizeof cond, "false");
            else
                snprintf(cond, sizeof cond, "cc%u.%c %s 0.0", cc_read, cswz[0], cmp_op);
            const u32 end_at = (w3 & 0x7FFFFFFFu) << 2;
            switch (opcode) {
            case 0x02: /* IFE */
            case 0x03: /* LOOP */
            case 0x04: /* REP */
                if (n_blk == FP_MAX_BLOCKS || end_at <= off) {
                    out_puts(&o, "    /* TODO: malformed flow-control op skipped */\n");
                    break;
                }
                if (opcode == 0x02) {
                    const u32 else_at = (w2 & 0x7FFFFFFFu) << 2;
                    blk_else[n_blk] = else_at != end_at ? else_at : 0;
                    blk_loop[n_blk] = 0;
                    snprintf(line, sizeof line, "    if (%s) {\n", cond);
                } else {
                    const u32 end_n = (w2 >> 2) & 0xFFu, init = (w2 >> 10) & 0xFFu;
                    const u32 inc = (w2 >> 19) & 0xFFu;
                    blk_else[n_blk] = 0;
                    blk_loop[n_blk] = 1;
                    snprintf(line, sizeof line,
                             "    if (%s) [loop] for (int _l%d = %u; _l%d < %u; _l%d += %u) {\n",
                             cond, n_loop, init, n_loop, end_n, n_loop, inc ? inc : 1u);
                    n_loop++;
                    loops_open++;
                }
                blk_end[n_blk++] = end_at;
                out_puts(&o, line);
                break;
            case 0x00: /* BRK */
                snprintf(line, sizeof line, "    if (%s) break;\n", cond);
                if (loops_open) out_puts(&o, line);
                break;
            default:   /* CAL / RET: no subroutine support yet */
                out_puts(&o, "    /* TODO: CAL/RET flow-control op skipped */\n");
                break;
            }
            if (w0 & FP_END) break;
            continue;
        }

        if (gamma_masks && gamma_masks[tex_unit] && !(w0 & (1u << 21)) &&
            (opcode == OP_TXD || opcode == OP_TEXBEM || opcode == OP_TXPBEM)) {
            static u32 reported;
            if (PS3_LOG_FIRST(reported, 32))
                fprintf(stderr, "[RSX fp] gamma fetch opcode %u unsupported on unit %u\n", opcode, tex_unit);
            return -1;
        }

        if (dbg_here && !dbg_done && dbg_count == (u32)dbg_n) {
            /* "h1" -> (h[1]), "h1.wwww" -> (h[1]).wwww */
            const char* dot = strchr(dbg_reg, '.');
            const unsigned idx = (unsigned)strtoul(dbg_reg + 1, NULL, 10);
            char line[96];
            snprintf(line, sizeof line, "    _rsx_dbg = (float4)((%c[%u])%s);\n",
                     dbg_reg[0], idx < 48u ? idx : 0u, dot ? dot : "");
            out_puts(&o, line);
            dbg_done = 1;
        }
        /* Build the RHS expression for this opcode. */
        char rhs[640];
        int handled = 1;
        switch (opcode) {
        case OP_NOP: rhs[0] = '\0'; handled = 0; break;
        case OP_MOV: snprintf(rhs, sizeof(rhs), "%s", a); break;
        case OP_MUL: snprintf(rhs, sizeof(rhs), "(%s) * (%s)", a, b); break;
        case OP_ADD: snprintf(rhs, sizeof(rhs), "(%s) + (%s)", a, b); break;
        case OP_MAD: snprintf(rhs, sizeof(rhs), "(%s) * (%s) + (%s)", a, b, c); break;
        case OP_DP3: snprintf(rhs, sizeof(rhs), "dot((%s).xyz, (%s).xyz)", a, b); break;
        case OP_DP4: snprintf(rhs, sizeof(rhs), "dot((%s), (%s))", a, b); break;
        case OP_MIN: snprintf(rhs, sizeof(rhs), "min((%s), (%s))", a, b); break;
        case OP_MAX: snprintf(rhs, sizeof(rhs), "max((%s), (%s))", a, b); break;
        case OP_FRC: snprintf(rhs, sizeof(rhs), "frac(%s)", a); break;
        case OP_FLR: snprintf(rhs, sizeof(rhs), "floor(%s)", a); break;
        case OP_RCP: snprintf(rhs, sizeof(rhs), "(1.0 / (%s).x)", a); break;
        /* Pack/unpack (RPCS3 FragmentProgramDecompiler PK/UP cases and the pkg/upg
         * builtins): the packed word is a scalar that replicates. */
        case OP_PK4UB: uses_pack = 1; snprintf(rhs, sizeof(rhs), "asfloat(_rsx_pk4ub(%s))", a); break;
        case OP_UP4UB: uses_pack = 1; snprintf(rhs, sizeof(rhs), "_rsx_up4ub((%s).x)", a); break;
        case OP_PKG:   uses_pack = 1; snprintf(rhs, sizeof(rhs), "asfloat(_rsx_pk4ub(_rsx_lin2srgb(%s)))", a); break;
        case OP_UPG:   uses_pack = 1; snprintf(rhs, sizeof(rhs), "_rsx_srgb2lin(_rsx_up4ub((%s).x))", a); break;
        case OP_PK4B:  uses_pack = 1; snprintf(rhs, sizeof(rhs), "asfloat(_rsx_pk4b(%s))", a); break;
        case OP_UP4B:  uses_pack = 1; snprintf(rhs, sizeof(rhs), "_rsx_up4b((%s).x)", a); break;
        case OP_PK2US: uses_pack = 1; snprintf(rhs, sizeof(rhs), "asfloat(_rsx_pk2us(%s))", a); break;
        case OP_UP2US: uses_pack = 1; snprintf(rhs, sizeof(rhs), "_rsx_up2us((%s).x)", a); break;
        case OP_PK2H:  uses_pack = 1; snprintf(rhs, sizeof(rhs), "asfloat(_rsx_pk2h(%s))", a); break;
        case OP_UP2H:  uses_pack = 1; snprintf(rhs, sizeof(rhs), "_rsx_up2h((%s).x)", a); break;
        /* Real RSX RSQ ignores the input's sign — 1/sqrt(|x|), not 1/sqrt(x)
         * (oracle: RPCS3 FragmentProgramDecompiler.cpp, "RSQ ignores the sign
         * of the inputs (Metro Last Light, GTA4)"). The strict form NaN'd the
         * normal-map Z-reconstruction (1-x²-y² measurably negative for most
         * pixels, median -0.0078) and poisoned the whole lighting chain — the
         * s25-s27 black/blue character class (scratch/s26_fp_bisect.md s27
         * part 4; two independent shader pairs confirmed). */
        case OP_RSQ: snprintf(rhs, sizeof(rhs), "rsqrt(abs((%s).x))", a); break;
        case OP_EX2: snprintf(rhs, sizeof(rhs), "exp2((%s).x)", a); break;
        case OP_LG2: snprintf(rhs, sizeof(rhs), "log2((%s).x)", a); break;
        case OP_COS: snprintf(rhs, sizeof(rhs), "cos((%s).x)", a); break;
        case OP_SIN: snprintf(rhs, sizeof(rhs), "sin((%s).x)", a); break;
        case OP_POW: snprintf(rhs, sizeof(rhs), "pow((%s).x, (%s).x)", a, b); break;
        case OP_DIV: snprintf(rhs, sizeof(rhs), "(%s) / (%s).x", a, b); break;
        /* Real RSX DIVSQ shares RSQ's sign-ignoring sqrt on the denominator
         * (oracle: RPCS3 FragmentProgramDecompiler.cpp's _builtin_sqrt macro,
         * reused by _builtin_divsq) AND forces the result to exactly 0,
         * component-wise, wherever the NUMERATOR component is 0 -- even if
         * the denominator is also 0 (oracle: same file, "DIVSQ is not
         * compliant. Result is 0 if numerator is 0 regardless of
         * denominator", FragmentProgramDecompiler.cpp:1076). Using a true
         * per-component select (HLSL vector `?:`, not lerp/mix) avoids NaN
         * contamination from the 0/0 branch, matching the oracle's own note
         * on why it can't use a blend here. */
        case OP_DIVSQ:
            snprintf(rhs, sizeof(rhs),
                     "(abs(%s) > 0.0 ? (%s) / sqrt(abs((%s).x)) : (float4)0.0)",
                     a, a, b);
            break;
        case OP_DP2: snprintf(rhs, sizeof(rhs), "dot((%s).xy, (%s).xy)", a, b); break;
        case OP_NRM: snprintf(rhs, sizeof(rhs), "float4(normalize((%s).xyz), 1.0)", a); break;
        case OP_LRP: snprintf(rhs, sizeof(rhs), "lerp((%s), (%s), (%s))", c, b, a); break;
        /* Lighting coefficients: (1, y, y > 0 ? 2^w : 0, 1) (RPCS3
         * FragmentProgramDecompiler, RSX_FP_OPCODE_LIF). */
        case OP_LIF:
            snprintf(rhs, sizeof(rhs),
                     "float4(1.0, (%s).y, (%s).y > 0.0 ? exp2((%s).w) : 0.0, 1.0)", a, a, a);
            break;
        /* Texture/branch fences: ordering hints with no result -- no-op. */
        case OP_FENCT: case OP_FENCB: rhs[0] = '\0'; handled = 0; break;
        case OP_SLT: snprintf(rhs, sizeof(rhs), "(float4)((%s) <  (%s))", a, b); break;
        case OP_SGE: snprintf(rhs, sizeof(rhs), "(float4)((%s) >= (%s))", a, b); break;
        case OP_SLE: snprintf(rhs, sizeof(rhs), "(float4)((%s) <= (%s))", a, b); break;
        case OP_SGT: snprintf(rhs, sizeof(rhs), "(float4)((%s) >  (%s))", a, b); break;
        case OP_SNE: snprintf(rhs, sizeof(rhs), "(float4)((%s) != (%s))", a, b); break;
        case OP_SEQ: snprintf(rhs, sizeof(rhs), "(float4)((%s) == (%s))", a, b); break;
        case OP_TEX:
            if (shadow && shadow[tex_unit].enabled) {
                snprintf(rhs, sizeof(rhs), "_rsx_shadow%u((%s).xyz)", tex_unit, a);
                break;
            }
            if ((tex_sep_mask >> tex_unit) & 1u)
                /* Cubemap or volume: sample with all three coordinates. */
                snprintf(rhs, sizeof(rhs),
                         "rsx_tex%u.Sample(rsx_samp[%u], (%s).xyz)", tex_unit, tex_unit, a);
            else if (tex_sep_mask)
                snprintf(rhs, sizeof(rhs),
                         "rsx_tex%u.Sample(rsx_samp[%u], (%s).xy)", tex_unit, tex_unit, a);
            else
                snprintf(rhs, sizeof(rhs),
                         "rsx_tex[%u].Sample(rsx_samp[%u], (%s).xy)", tex_unit, tex_unit, a);
            break;
        case OP_TXP:
            if (shadow && shadow[tex_unit].enabled) {
                snprintf(rhs, sizeof(rhs), "_rsx_shadow%u((%s).xyz / (%s).w)", tex_unit, a, a);
                break;
            }
            if ((tex_cube_mask >> tex_unit) & 1u)
                /* Projective divide is meaningless for a cube lookup; sample
                 * the direction directly. */
                snprintf(rhs, sizeof(rhs),
                         "rsx_tex%u.Sample(rsx_samp[%u], (%s).xyz)", tex_unit, tex_unit, a);
            else if ((tex_vol_mask >> tex_unit) & 1u)
                snprintf(rhs, sizeof(rhs),
                         "rsx_tex%u.Sample(rsx_samp[%u], (%s).xyz / (%s).w)",
                         tex_unit, tex_unit, a, a);
            else if (tex_sep_mask)
                snprintf(rhs, sizeof(rhs),
                         "rsx_tex%u.Sample(rsx_samp[%u], (%s).xy / (%s).w)",
                         tex_unit, tex_unit, a, a);
            else
                snprintf(rhs, sizeof(rhs),
                         "rsx_tex[%u].Sample(rsx_samp[%u], (%s).xy / (%s).w)",
                         tex_unit, tex_unit, a, a);
            break;
        case OP_TXB: case OP_TXL: {
            /* Biased / explicit-LOD sample; bias or LOD is src1.x (RPCS3
             * TEXTURE_SAMPLE2D_BIAS / _LOD). GH3's fret buttons and fret lines
             * are TXB: unhandled, they sampled nothing and output alpha 0. */
            const int cube = (tex_sep_mask >> tex_unit) & 1u;   /* cube or volume: xyz */
            char tn[32];
            snprintf(tn, sizeof(tn), tex_sep_mask ? "rsx_tex%u" : "rsx_tex[%u]", tex_unit);
            snprintf(rhs, sizeof(rhs), "%s.%s(rsx_samp[%u], (%s).%s, (%s).x)", tn,
                     opcode == OP_TXB ? "SampleBias" : "SampleLevel", tex_unit, a,
                     cube ? "xyz" : "xy", b);
            break;
        }
        case OP_KIL:
            /* Fragment kill. Predicated by the same exec_if condition as any
             * other instruction (RPCS3 FragmentProgramDecompiler case
             * RSX_FP_OPCODE_KIL: AddFlowOp("discard") under GetCond());
             * unconditional encodings discard outright. Dropping this (the
             * old TODO) rendered every alpha-cutout material as a solid
             * quad. */
            if (is_never) {
                rhs[0] = '\0';
            } else if (is_uncond) {
                out_puts(&o, "    discard;\n");
            } else {
                char kil[128];
                snprintf(kil, sizeof(kil),
                         "    if (any(cc%u.%s %s (float4)0)) discard;\n",
                         cc_read, cswz, cmp_op);
                out_puts(&o, kil);
            }
            handled = 0;   /* no destination write */
            break;
        default: {
            /* The number and the raw words, so an opcode outside the table
             * can be identified from the dumped shader alone. */
            char todo[160];
            snprintf(todo, sizeof todo,
                     "    /* TODO: unhandled FP opcode 0x%02X (%s) words %08X %08X %08X %08X */\n",
                     opcode, rsx_fp_opcode_name(opcode), w0, w1, w2, w3);
            out_puts(&o, todo);
            handled = 0;
            break; }
            break;
        }

        if (gamma_masks && gamma_masks[tex_unit] && !(w0 & (1u << 21)) &&
            (opcode == OP_TEX || opcode == OP_TXP || opcode == OP_TXB || opcode == OP_TXL)) {
            char sample[sizeof rhs]; memcpy(sample, rhs, sizeof sample);
            const int length = snprintf(rhs, sizeof rhs, "_rsx_gamma_input(%s, %uu)",
                                        sample, gamma_masks[tex_unit] & 15u);
            if (length < 0 || (size_t)length >= sizeof rhs) return -1;
        }
        /* Texture sign expansion (RPCS3 _process_texel). A unit whose
         * SET_TEXTURE_ADDRESS asks for UNSIGNED_REMAP_BIASED returns every
         * fetch re-centred on the lanes the mask names, (round(x*255)-128)/127
         * (16-bit channels: /32767), after gamma and not on gamma lanes. A
         * fetch carrying EXP_TEX (OPDEST bit 21, the _bx2 modifier) returns
         * x*2-1 on every lane, whatever the unit says, and bypasses gamma.
         * A comparison fetch has nothing to expand. */
        if (((w0 & (1u << 21)) || (expand_masks && expand_masks[tex_unit])) &&
            !(shadow && shadow[tex_unit].enabled) &&
            (opcode == OP_TEX || opcode == OP_TXP || opcode == OP_TXB || opcode == OP_TXL)) {
            const int bx2 = (w0 & (1u << 21)) != 0;
            u32 mask = bx2 ? 15u : (expand_masks[tex_unit] & 0x1Fu);
            if (!bx2 && gamma_masks) mask &= ~(gamma_masks[tex_unit] & 15u);
            if (mask & 15u) {
                char sample[sizeof rhs]; memcpy(sample, rhs, sizeof sample);
                const int length = snprintf(rhs, sizeof rhs, "_rsx_expand_input(%s, %uu, %s)",
                                            sample, mask, bx2 ? "true" : "false");
                if (length < 0 || (size_t)length >= sizeof rhs) return -1;
            }
        } else if (((w0 & (1u << 21)) || (expand_masks && expand_masks[tex_unit])) &&
                   (opcode == OP_TXD || opcode == OP_TEXBEM || opcode == OP_TXPBEM)) {
            static u32 reported;
            if (PS3_LOG_FIRST(reported, 32))
                fprintf(stderr, "[RSX fp] sign-expand fetch opcode %u unsupported on unit %u (read unexpanded)\n",
                        opcode, tex_unit);
        }

        int has_dest = !(w0 & FP_OUT_NONE);
        if (dbg_here && handled && !dbg_done) dbg_count++;
        if (handled && (has_dest || set_cond)) {
            u32 dst_idx = (w0 & FP_OUT_REG_MASK) >> FP_OUT_REG_SHIFT;
            int dst_half = (w0 & FP_OUT_HALF) ? 1 : 0;
            const char* reg = dst_half ? "h" : "r";
            char m[5];
            dest_mask(w0, m);

            const char* sat = (w0 & FP_OUT_SAT) ? " _v = saturate(_v);" : "";
            /* NV40 per-instruction result-scale modifier (SRC1 word bits
             * 28-30): 1/2/3 = *2/*4/*8, 5/6/7 = /2//4//8; applied to the
             * result BEFORE saturate (RPCS3 FragmentProgramDecompiler.cpp
             * SetDst, RSXFragmentProgram.h SRC1.scale:3). Dropped entirely
             * until s32: draw 1625's two-tap box filter (÷2 on the final
             * ADD) summed unaveraged -- the flat 2x composite gain behind
             * the whiteout/overbright class (scratch/s32_gain_hunt.md). */
            u32 res_scale = (w2 >> 28) & 0x7u;
            static const char* scale_txt[8] = { "", " * 2.0", " * 4.0", " * 8.0",
                                                "", " / 2.0", " / 4.0", " / 8.0" };

            /* Broadcast the result to float4 first so the write-mask picks the
             * CORRESPONDING components: `dst.w = rhs` would HLSL-truncate a
             * vector rhs to its .x, but NV40 writes rhs.w to dst.w. Scalar
             * results (DPx/RCP/...) replicate, which is also the hardware
             * behavior. */
            if (has_dest && is_uncond && !set_cond) {
                /* Common unconditional path — kept byte-identical so the 39
                 * non-predicated programs in this capture emit unchanged. */
                char line[800];
                snprintf(line, sizeof(line),
                         "    { float4 _v = (float4)(%s)%s;%s %s[%u].%s = _v.%s; }\n",
                         rhs, scale_txt[res_scale], sat, reg, dst_idx, m, m);
                out_puts(&o, line);
            } else {
                /* Predicated and/or CC-writing instruction. */
                char buf[1200];
                snprintf(buf, sizeof(buf),
                         "    { float4 _v = (float4)(%s)%s;%s\n",
                         rhs, scale_txt[res_scale], sat);
                out_puts(&o, buf);
                if (has_dest) {
                    if (is_uncond) {
                        snprintf(buf, sizeof(buf),
                                 "      %s[%u].%s = _v.%s;\n", reg, dst_idx, m, m);
                    } else if (is_never) {
                        snprintf(buf, sizeof(buf),
                                 "      /* exec_if=none: write suppressed */\n");
                    } else {
                        /* Per-component select gated by the CC sign test.
                         * A real select, not lerp(dst, _v, mask): lerp is
                         * arithmetic (dst + mask * (_v - dst)), so an Inf or
                         * NaN in the unselected operand poisons the result
                         * (0 * Inf = NaN), and a register holding raw bits
                         * (a packed word) loses its denormal and NaN
                         * patterns on the way through the ALU. */
                        snprintf(buf, sizeof(buf),
                                 "      float4 _p = (float4)((cc%u).%s %s (float4)0.0);\n"
                                 "      %s[%u].%s = (_p.%s != 0.0) ? _v.%s : %s[%u].%s;\n",
                                 cc_read, cswz, cmp_op,
                                 reg, dst_idx, m, m, m, reg, dst_idx, m);
                    }
                    out_puts(&o, buf);
                }
                if (set_cond) {
                    /* CC receives the destination value (post-select), masked;
                     * for no-dest set_cond it receives the raw result. */
                    if (has_dest)
                        snprintf(buf, sizeof(buf),
                                 "      cc%u.%s = %s[%u].%s;\n", cc_write, m, reg, dst_idx, m);
                    else
                        snprintf(buf, sizeof(buf),
                                 "      cc%u.%s = _v.%s;\n", cc_write, m, m);
                    out_puts(&o, buf);
                }
                out_puts(&o, "    }\n");
            }

            if (has_dest) {
                const int target = fp_export_target(dst_idx, dst_half);
                if (target >= 0) wrote[dst_half][target] = 1;
            }
        }

        if (w0 & FP_END) break;
    }
    /* A block whose end address is at or past END never saw it: close it. */
    while (n_blk-- > 0)
        out_puts(&o, "    }\n");

    /* Fragment color output register selection. The NV40 hardware picks the
     * output from the SHADER_CONTROL word's 32_BITS_EXPORTS bit, not from
     * whichever temp happened to be written: fp32-export programs output r0,
     * fp16-export programs (the default) output h0 (RPCS3
     * FragmentProgramDecompiler::BuildCode). Many real material/lighting
     * shaders accumulate into fp32 temps (r2/r3/r4) but write their FINAL
     * color to h0 — the old "wrote_r0 => return r0" heuristic returned a stale
     * intermediate, producing flat/constant surfaces. */
    int half_exports;
    if (ctrl == RSX_FP_CTRL_AUTO) {
        /* No control word (standalone tests): legacy heuristic. */
        half_exports = (wrote[0][0] || !wrote[1][0]) ? 0 : 1;
    } else {
        half_exports = (ctrl & FP_CTRL_32BIT_EXPORTS) ? 0 : 1;
    }
    const char* export_file = half_exports ? "h" : "r";

    /* How many colour targets the program feeds: one past the highest export
     * register of the selected file it wrote. A program that writes only the
     * first keeps the single-value return it has always had, byte for byte;
     * only a program that reaches the second, third or fourth grows a struct.
     * Writes to the OTHER file's export registers do not count -- r2/r3/r4 are
     * ordinary temps under 16-bit exports, and accumulating into them while
     * the colour goes to h0 is what most material shaders do. */
    u32 targets = 1;
    for (u32 t = FP_MAX_COLOR_TARGETS; t-- > 1; )
        if (wrote[half_exports][t]) { targets = t + 1; break; }

    /* Depth export. SHADER_CONTROL bits 1-3 (0xE,
     * CELL_GCM_SHADER_CONTROL_DEPTH_EXPORT) make the program's depth the
     * fragment depth: always the fp32 register r1.z, whatever the colour
     * export width, clamped to [0,1]. Without SV_Depth the fragment keeps the
     * interpolated depth of the primitive, so a full-screen pass that rebuilds
     * a depth buffer writes the quad's constant depth instead. The output
     * becomes a struct even for one colour target. */
    const int depth_export = ctrl != RSX_FP_CTRL_AUTO && (ctrl & 0xEu) != 0u;
    if (dbg_here) {
        static u32 reported;
        if (PS3_LOG_FIRST(reported, 8))
            fprintf(stderr, "[RSX fp] RSX_FP_DEBUG %016llx: %s after instruction %ld%s\n",
                    (unsigned long long)dbg_hash, dbg_reg, dbg_n,
                    dbg_done ? "" : " not reached (program is shorter)");
        if (dbg_done)
            out_puts(&o, "    { float4 _d = _rsx_dbg; _d.w = 1.0;"
                         " r[0] = _d; h[0] = _d; }\n");
    }
    if (targets == 1 && !depth_export) {
        char tail[96];
        snprintf(tail, sizeof(tail),
                 "    float4 _o = %s[0];\n"
                 "    return (_o == _o) ? _o : (float4)0;\n}\n", export_file);
        out_puts(&o, tail);
    } else {
        /* Multiple render targets. The NaN guard is per target: a poisoned
         * G-buffer plane is as fatal as a poisoned colour one, and the planes
         * are computed independently. */
        out_puts(&o, "    PSOutput _out;\n");
        for (u32 t = 0; t < targets; t++) {
            char line[192];
            snprintf(line, sizeof(line),
                     "    float4 _o%u = %s[%u];\n"
                     "    _out.t%u = (_o%u == _o%u) ? _o%u : (float4)0;\n",
                     t, export_file, fp_export_reg[half_exports][t],
                     t, t, t, t);
            out_puts(&o, line);
        }
        if (depth_export)
            out_puts(&o, "    float _d = r[1].z;\n"
                         "    _out.depth = (_d == _d) ? saturate(_d) : 0.0;\n");
        out_puts(&o, "    return _out;\n}\n");
    }

    /* Preamble: PSInput matches the backend's placeholder layout; temp/half
     * register files; texture+sampler banks for TEX. Built here, in front of
     * the body, now that the return type is known. */
    char preamble[32768]; /* bounded room for all 16 comparison helpers */
    Out p = { preamble, sizeof(preamble), 0, 1 };
    preamble[0] = '\0';
    out_puts(&p,
        "struct PSInput {\n"
        "    float4 position : SV_POSITION; float4 col0 : COLOR0; float4 col1 : COLOR1;\n"
        "    float4 fog : FOG;\n"
        "    float4 tc0:TEXCOORD0; float4 tc1:TEXCOORD1; float4 tc2:TEXCOORD2; float4 tc3:TEXCOORD3;\n"
        "    float4 tc4:TEXCOORD4; float4 tc5:TEXCOORD5; float4 tc6:TEXCOORD6; float4 tc7:TEXCOORD7;\n");
    /* TC8/TC9 and the face flag only where the body reads them, so every other
     * program stays byte-identical (and keeps its cached pipeline). TC8 and
     * TC9 follow TC7 so the earlier interpolants keep their locations. */
    if (strstr(o.p, "input.tc8") || strstr(o.p, "input.tc9"))
        out_puts(&p, "    float4 tc8:TEXCOORD8; float4 tc9:TEXCOORD9;\n");
    if (strstr(o.p, "input._rsx_front"))
        out_puts(&p, "    bool _rsx_front : SV_IsFrontFace;\n");
    out_puts(&p, "};\n");
    /* 1/w of the fragment for perspective_corr reads. SV_POSITION.w is the
     * clip w under FXC (D3D12) but already 1/w once glslang lowers it to
     * SPIR-V FragCoord (Vulkan, and Metal's [[position]] is (x, y, z, 1/w));
     * the glslang paths define RSX_FRAGCOORD_W_IS_RCP. Emitted only where the
     * body uses it, so every other program stays byte-identical. */
    if (strstr(o.p, "_rsx_inv_w("))
        out_puts(&p,
            "#ifdef RSX_FRAGCOORD_W_IS_RCP\n"
            "#define _rsx_inv_w(p) ((p).w)\n"
            "#else\n"
            "#define _rsx_inv_w(p) (1.0 / (p).w)\n"
            "#endif\n");
    /* WPOS in the RSX window convention. The host position has its origin at the
     * top left; with SET_SHADER_WINDOW origin BOTTOM the RSX counts y up from the
     * bottom edge. A full-screen pass that builds its UV from it, e.g.
     * uv = wpos * (1/W, -1/H) + (0, 1), mirrored its source when it read the host y.
     * The draw engine passes the window height in fp_alpha.y when the origin is
     * BOTTOM (0 otherwise) and a pixel-center bias in fp_alpha.z (-0.5 for INTEGER
     * centers, 0 for HALF). A backend that leaves both 0 gets the host position. */
    if (strstr(o.p, "_rsx_wpos("))
        out_puts(&p, buffered
            ? "#define _rsx_wpos(p) float4((p).x + fp_alpha.z, "
              "(fp_alpha.y != 0.0 ? fp_alpha.y - (p).y : (p).y) + fp_alpha.z, (p).z, (p).w)\n"
            : "#define _rsx_wpos(p) (p)\n");
    /* Texture bank. With no cube units (the default) emit the exact legacy
     * array declaration so 2D-only programs are byte-identical. When any unit
     * is a cubemap, declare each unit individually at its t-register so the
     * cube units can be TextureCube while the rest stay Texture2D. */
    if (tex_sep_mask == 0) {
        out_puts(&p, "Texture2D    rsx_tex[16] : register(t0);\n");
    } else {
        char decl[64];
        for (u32 u = 0; u < 16; u++) {
            snprintf(decl, sizeof(decl), "%s rsx_tex%u : register(t%u);\n",
                     ((tex_cube_mask >> u) & 1u) ? "TextureCube" :
                     ((tex_vol_mask >> u) & 1u)  ? "Texture3D  " : "Texture2D  ", u, u);
            out_puts(&p, decl);
        }
    }
    out_puts(&p,
        "SamplerState rsx_samp[16] : register(s0);\n"
    );
    if (buffered) {
        char constants_decl[192];
        snprintf(
            constants_decl, sizeof(constants_decl),
            "cbuffer PSConstants : register(b1) {\n"
            "    float4 fp_constants[%u];\n"
            "    float4 fp_alpha;\n"
            "};\n",
            buffered_constant_count ? buffered_constant_count : 1u);
        out_puts(&p, constants_decl);
    }
    if (shadow) for (u32 u = 0; u < 16; u++)
        if ((sampled_units & (1u << u)) && shadow[u].enabled)
            fp_shadow_helper(&p, u, tex_sep_mask, &shadow[u]);
    if (gamma_units) fp_gamma_helper(&p);
    if (expand_units || bx2_units) fp_expand_helper(&p);
    /* NV40 pack/unpack. A packed word travels on in a float's bits, so the
     * packs bitcast and the unpacks bitcast back; the sRGB forms follow
     * RPCS3's _builtin_pkg/_builtin_upg (linear_to_srgb, srgb_to_linear).
     * These were the opcodes inFAMOUS 2's character materials used and the
     * decompiler skipped, leaving the destination with whatever it held.
     * Only for a program that uses one, so every other program's text is
     * unchanged. */
    if (uses_pack) out_puts(&p,
        "uint   _rsx_pk4ub(float4 v) { uint4 b = (uint4)round(saturate(v) * 255.0); return b.x | (b.y << 8) | (b.z << 16) | (b.w << 24); }\n"
        "float4 _rsx_up4ub(float v) { uint u = asuint(v); return float4(u & 255u, (u >> 8) & 255u, (u >> 16) & 255u, (u >> 24) & 255u) / 255.0; }\n"
        "uint   _rsx_pk4b(float4 v) { uint4 b = (uint4)((int4)round(clamp(v, -1.0, 1.0) * 127.0)) & 255u; return b.x | (b.y << 8) | (b.z << 16) | (b.w << 24); }\n"
        "float4 _rsx_up4b(float v) { uint u = asuint(v); int4 b = int4(u & 255u, (u >> 8) & 255u, (u >> 16) & 255u, (u >> 24) & 255u); b = (b << 24) >> 24; return clamp((float4)b / 127.0, -1.0, 1.0); }\n"
        "uint   _rsx_pk2us(float4 v) { uint2 b = (uint2)round(saturate(v.xy) * 65535.0); return b.x | (b.y << 16); }\n"
        "float4 _rsx_up2us(float v) { uint u = asuint(v); return float4(u & 65535u, u >> 16, u & 65535u, u >> 16) / 65535.0; }\n"
        "uint   _rsx_pk2h(float4 v) { return f32tof16(v.x) | (f32tof16(v.y) << 16); }\n"
        "float4 _rsx_up2h(float v) { uint u = asuint(v); return float4(f16tof32(u & 65535u), f16tof32(u >> 16), f16tof32(u & 65535u), f16tof32(u >> 16)); }\n"
        "float4 _rsx_lin2srgb(float4 cl) { cl = saturate(cl); float4 low = cl * 12.92; float4 high = 1.055 * pow(cl, 1.0 / 2.4) - 0.055; return saturate((cl < 0.0031308) ? low : high); }\n"
        "float4 _rsx_srgb2lin(float4 cs) { float4 a = cs / 12.92; float4 b = pow(max(cs + 0.055, 0.0) / 1.055, 2.4); return (cs > 0.04045) ? b : a; }\n");
    if (targets == 1 && !depth_export) {
        out_puts(&p, "float4 main(PSInput input) : SV_TARGET {\n");
    } else {
        out_puts(&p, "struct PSOutput {\n");
        for (u32 t = 0; t < targets; t++) {
            char member[64];
            snprintf(member, sizeof(member),
                     "    float4 t%u : SV_TARGET%u;\n", t, t);
            out_puts(&p, member);
        }
        if (depth_export)
            out_puts(&p, "    float depth : SV_Depth;\n");
        out_puts(&p, "};\n"
                     "PSOutput main(PSInput input) {\n");
    }

    if (!o.ok || !p.ok) return -1;
    if (o.len + p.len + 1 > out_size) return -1;
    memmove(out + p.len, out, o.len + 1);
    memcpy(out, preamble, p.len);
    return count;
}

int rsx_fp_decompile_ex(const u8* ucode, u32 max_bytes, u32 ctrl,
                        u32 tex_cube_mask, char* out, u32 out_size)
{
    return rsx_fp_decompile_internal(
        ucode, max_bytes, ctrl, tex_cube_mask, 0, 0, NULL, NULL, NULL, out, out_size, NULL);
}

int rsx_fp_decompile_buffered_ex(
    const u8* ucode, u32 max_bytes, u32 ctrl, u32 tex_cube_mask,
    char* out, u32 out_size, u32* out_constant_count)
{
    return rsx_fp_decompile_internal(
        ucode, max_bytes, ctrl, tex_cube_mask, 0, 1, NULL, NULL, NULL, out, out_size,
        out_constant_count);
}

int rsx_fp_decompile_buffered_shadow_ex(
    const u8* ucode, u32 max_bytes, u32 ctrl, u32 tex_cube_mask,
    const rsx_fp_shadow_desc shadow[16],
    char* out, u32 out_size, u32* out_constant_count)
{
    return rsx_fp_decompile_internal(ucode, max_bytes, ctrl, tex_cube_mask, 0,
        1, shadow, NULL, NULL, out, out_size, out_constant_count);
}

int rsx_fp_decompile_buffered_shadow_color_ex(
    const u8* ucode, u32 max_bytes, u32 ctrl, u32 tex_cube_mask,
    const rsx_fp_shadow_desc shadow[16], const u8 gamma_masks[16],
    char* out, u32 out_size, u32* out_constant_count)
{
    return rsx_fp_decompile_internal(ucode, max_bytes, ctrl, tex_cube_mask, 0,
        1, shadow, gamma_masks, NULL, out, out_size, out_constant_count);
}

int rsx_fp_decompile_buffered_shadow_color_expand_ex(
    const u8* ucode, u32 max_bytes, u32 ctrl, u32 tex_cube_mask,
    const rsx_fp_shadow_desc shadow[16], const u8 gamma_masks[16],
    const u8 expand_masks[16],
    char* out, u32 out_size, u32* out_constant_count)
{
    return rsx_fp_decompile_internal(ucode, max_bytes, ctrl, tex_cube_mask, 0,
        1, shadow, gamma_masks, expand_masks, out, out_size, out_constant_count);
}

int rsx_fp_decompile_buffered_shadow_color_expand_vol_ex(
    const u8* ucode, u32 max_bytes, u32 ctrl, u32 tex_cube_mask, u32 tex_vol_mask,
    const rsx_fp_shadow_desc shadow[16], const u8 gamma_masks[16],
    const u8 expand_masks[16],
    char* out, u32 out_size, u32* out_constant_count)
{
    return rsx_fp_decompile_internal(ucode, max_bytes, ctrl, tex_cube_mask, tex_vol_mask,
        1, shadow, gamma_masks, expand_masks, out, out_size, out_constant_count);
}

static float fp_half_to_float(u16 h)
{
    const u32 sign = (u32)(h & 0x8000u) << 16;
    u32 exponent = (h >> 10) & 0x1Fu;
    u32 mantissa = h & 0x03FFu;
    u32 bits;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            exponent = 127u - 15u + 1u;
            while ((mantissa & 0x0400u) == 0) {
                mantissa <<= 1;
                exponent--;
            }
            mantissa &= 0x03FFu;
            bits = sign | (exponent << 23) | (mantissa << 13);
        }
    } else if (exponent == 0x1Fu) {
        bits = sign | 0x7F800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent + (127u - 15u)) << 23) | (mantissa << 13);
    }
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

float rsx_fp_alpha_ref(u32 raw, u32 surface_color_format)
{
    /* NV4097 surface color formats: 14=W16Z16Y16X16,
     * 15=W32Z32Y32X32, 16=X32. */
    if (surface_color_format == 14u)
        return fp_half_to_float((u16)(raw & 0xFFFFu));
    if (surface_color_format == 15u || surface_color_format == 16u) {
        float value;
        memcpy(&value, &raw, sizeof(value));
        return value;
    }
    return (float)(raw & 0xFFu) / 255.0f;
}

/* The comparison the alpha test has to pass. The alpha it tests is colour
 * target 0's, which for a single-target program is the returned value itself
 * and for an MRT one is the returned struct's first member -- the NV40 alpha
 * test has one input however many targets the program feeds. `cmp` is the
 * CELL_GCM function minus 0x200, with NEVER (0) spelled out and ALWAYS (7)
 * already handled by the caller. */
static void fp_alpha_pass_expr(u32 cmp, const char* alpha, char* out, u32 out_size)
{
    static const char* op[8] = { NULL, "<", "==", "<=", ">", "!=", ">=", NULL };
    if (cmp == 0 || cmp >= 7 || !op[cmp])
        snprintf(out, out_size, "false");
    else
        snprintf(out, out_size, "%s.a %s _rsx_alpha_ref", alpha, op[cmp]);
}

/* Where that alpha lives, and what type the value being returned has. */
static int fp_returns_struct(const char* hlsl)
{
    return strstr(hlsl, "struct PSOutput") != NULL;
}

int rsx_fp_apply_alpha_test(char* hlsl, u32 out_size, u32 func, float ref)
{
    if (!hlsl || out_size == 0)
        return -1;

    const u32 cmp = func >= 0x0200u && func <= 0x0207u ? func - 0x0200u : 7u;
    if (cmp == 7u)
        return 0;

    char* marker = NULL;
    for (char* p = strstr(hlsl, "    return "); p;
         p = strstr(p + 1, "    return "))
        marker = p;
    if (!marker)
        return -1;
    char* expression = marker + strlen("    return ");
    char* terminator = strstr(expression, ";\n}\n");
    if (!terminator || terminator == expression ||
        (size_t)(terminator - expression) >= 96)
        return -1;
    char output[96];
    memcpy(output, expression, (size_t)(terminator - expression));
    output[terminator - expression] = '\0';

    const int mrt = fp_returns_struct(hlsl);
    char pass[96];
    fp_alpha_pass_expr(cmp, mrt ? "_rsx_out.t0" : "_rsx_out", pass, sizeof(pass));
    char suffix[512];
    const int n = snprintf(suffix, sizeof(suffix),
        "    %s _rsx_out = %s;\n"
        "    const float _rsx_alpha_ref = %.9g;\n"
        "    if (!(%s)) discard;\n"
        "    return _rsx_out;\n}\n",
        mrt ? "PSOutput" : "float4", output, (double)ref, pass);
    if (n < 0 || (u32)n >= sizeof(suffix))
        return -1;
    const size_t prefix = (size_t)(marker - hlsl);
    if (prefix + (size_t)n + 1 > out_size)
        return -1;
    memcpy(marker, suffix, (size_t)n + 1);
    return 1;
}

int rsx_fp_apply_alpha_test_buffered(
    char* hlsl, u32 out_size, u32 func)
{
    if (!hlsl || out_size == 0)
        return -1;

    const u32 cmp =
        func >= 0x0200u && func <= 0x0207u ? func - 0x0200u : 7u;
    if (cmp == 7u)
        return 0;

    char* marker = NULL;
    for (char* p = strstr(hlsl, "    return "); p;
         p = strstr(p + 1, "    return "))
        marker = p;
    if (!marker)
        return -1;
    char* expression = marker + strlen("    return ");
    char* terminator = strstr(expression, ";\n}\n");
    if (!terminator || terminator == expression ||
        (size_t)(terminator - expression) >= 96)
        return -1;
    char output[96];
    memcpy(output, expression, (size_t)(terminator - expression));
    output[terminator - expression] = '\0';

    const int mrt = fp_returns_struct(hlsl);
    char pass[96];
    fp_alpha_pass_expr(cmp, mrt ? "_rsx_out.t0" : "_rsx_out", pass, sizeof(pass));
    char suffix[512];
    const int n = snprintf(
        suffix, sizeof(suffix),
        "    %s _rsx_out = %s;\n"
        "    const float _rsx_alpha_ref = fp_alpha.x;\n"
        "    if (!(%s)) discard;\n"
        "    return _rsx_out;\n}\n",
        mrt ? "PSOutput" : "float4", output, pass);
    if (n < 0 || (u32)n >= sizeof(suffix))
        return -1;
    const size_t prefix = (size_t)(marker - hlsl);
    if (prefix + (size_t)n + 1 > out_size)
        return -1;
    memcpy(marker, suffix, (size_t)n + 1);
    return 1;
}

/* --- kept from this tree when adopting the newer decompiler ---------------
 * rsx_d3d12_backend.c calls both of these: extract_consts feeds the per-draw
 * fp_k[] slots, code_hash keys the PSO cache on the ucode rather than the
 * constants. The live-draw engine does not need them, so they are absent
 * upstream in caner's fork. */

/* Walk a program the same way the decompiler does and copy out its inline
 * constants in the SAME order the decompiler assigns fp_k[] indices. The backend
 * calls this per draw, so re-patched constants take effect without recompiling. */
int rsx_fp_extract_consts(const u8* ucode, u32 max_bytes, float* out, int max_out)
{
    if (!ucode || !out) return 0;
    u32 off = 0; int n = 0;
    while (off + 16 <= max_bytes) {
        u32 w0 = rsx_fp_read_word(ucode + off + 0);
        u32 w1 = rsx_fp_read_word(ucode + off + 4);
        u32 w2 = rsx_fp_read_word(ucode + off + 8);
        u32 w3 = rsx_fp_read_word(ucode + off + 12);
        off += 16;
        int is_branch = (w2 & FP_BRANCH) != 0;
        int has_k = !is_branch &&
            ((((w1 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST) ||
             (((w2 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST) ||
             (((w3 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST));
        if (has_k) {
            if (off + 16 <= max_bytes) {
                for (int i = 0; i < 4; i++) {
                    u32 cw = rsx_fp_sanitize_constant_word(
                        rsx_fp_read_word(ucode + off + i * 4));
                    if (n < max_out) memcpy(&out[n * 4 + i], &cw, 4);
                }
                off += 16;
            }
            if (n < max_out) n++;
        }
        if (w0 & FP_END) break;
    }
    { static int dbg = -1;
      if (dbg < 0) { const char* e = getenv("FP_KDBG"); dbg = e ? atoi(e) : 0; }
      if (dbg) { static int m = 0; if (PS3_LOG_FIRST(m, 3)) {
        fprintf(stderr, "[FPK] %d consts%c", n, 10);
        for (int _q = 0; _q < n; _q++)
            fprintf(stderr, "   k[%2d] = (%g %g %g %g)%c", _q, out[_q*4+0],
                    out[_q*4+1], out[_q*4+2], out[_q*4+3], 10); } }
    }
    return n;
}

/* FNV-1a over a program's INSTRUCTION words only, skipping the inline constant
 * slots. With constants hoisted into fp_k[] the compiled shader does not depend
 * on their values, so including them in the pipeline key made it change every
 * time the title re-patched a constant -- which is exactly what thrashed the
 * cache. */
u32 rsx_fp_code_hash(const u8* ucode, u32 max_bytes)
{
    u32 h = 2166136261u;
    if (!ucode) return h;
    u32 off = 0;
    while (off + 16 <= max_bytes) {
        u32 w0 = rsx_fp_read_word(ucode + off + 0);
        u32 w1 = rsx_fp_read_word(ucode + off + 4);
        u32 w2 = rsx_fp_read_word(ucode + off + 8);
        u32 w3 = rsx_fp_read_word(ucode + off + 12);
        for (u32 i = 0; i < 16; i++) { h ^= ucode[off + i]; h *= 16777619u; }
        off += 16;
        int is_branch = (w2 & FP_BRANCH) != 0;
        int has_k = !is_branch &&
            ((((w1 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST) ||
             (((w2 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST) ||
             (((w3 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST));
        if (has_k) off += 16;            /* skip the constant, do not hash it */
        if (w0 & FP_END) break;
    }
    return h;
}

/* Scale texel-space coordinates to the 0..1 range HLSL sampling expects.
 *
 * An RSX texture whose format carries RSX_TEX_FMT_UNNORM (0x40) is addressed in
 * TEXELS, not normalised units. ps1_netemu composites the PS1 framebuffer with
 * exactly that: format 0xE2 (A1R5G5B5 | UNNORM | LINEAR) over a 1024x512
 * texture, and a quad whose texture coordinates run 1..638 by 2..474 --
 *
 *     [uvdbg] unit=0 ps1fb 1024x512 pitch=2048 fmt=0xE2
 *     [uvdbg]   v0 pos(-1.0 -1.0) tc(1.0000 2.0000)
 *     [uvdbg]   v2 pos(0.8 0.8)   tc(638.0000 474.0000)
 *
 * The bit was only ever masked OUT of the base format so the decoder could
 * recognise A1R5G5B5, and never honoured for addressing. So the sampler was
 * handed coordinates hundreds of times out of range, which under clamp lands on
 * one edge texel for the whole quad -- a flat, uniform result. That is the
 * black screen: the PS1 renders, the composite pass runs, and it samples one
 * corner texel.
 *
 * D3D12 has no unnormalised addressing mode, so the division has to happen in
 * the shader. Doing it as a post-pass over the generated HLSL rather than
 * inside the emitter keeps it beside rsx_fp_apply_alpha_test, which already
 * works this way, and leaves every non-UNNORM program byte-identical.
 *
 * Cube units are skipped: their coordinate is a float3 direction, and
 * RSX_TEX_FMT_UNNORM has no meaning for one. A volume unit (Texture3D) is
 * sampled with three coordinates, all in texels, so it is divided by its depth
 * too (RPCS3's texture cache scales a 3D texture's r by 1/depth the same way).
 */
int rsx_fp_apply_unnorm_scale(char* hlsl, u32 hlsl_size, u32 unnorm_mask,
                              const u32 dim[][3], u32 cube_mask, u32 volume_mask)
{
    if (!hlsl || !unnorm_mask) return 0;
    int patched = 0;

    for (u32 u = 0; u < 16; u++) {
        if (!((unnorm_mask >> u) & 1u)) continue;
        if ((cube_mask >> u) & 1u) continue;
        const u32 w = dim[u][0], h = dim[u][1], d = dim[u][2];
        const int volume = (volume_mask >> u) & 1u;
        if (!w || !h || (volume && !d)) continue;

        /* Both spellings the emitter produces for a 2D unit. */
        char needles[2][40];
        snprintf(needles[0], sizeof needles[0], "rsx_tex[%u].Sample(rsx_samp[%u],", u, u);
        snprintf(needles[1], sizeof needles[1], "rsx_tex%u.Sample(rsx_samp[%u],", u, u);

        for (int nidx = 0; nidx < 2; nidx++) {
            const char* needle = needles[nidx];
            const u32 nlen = (u32)strlen(needle);
            for (;;) {
                char* at = strstr(hlsl, needle);
                if (!at) break;
                /* The '(' of Sample(, then its matching ')'. */
                char* open = at + nlen - 1;   /* the ',' ... step back to '(' */
                while (open > at && *open != '(') open--;
                if (*open != '(') break;
                int depth = 0;
                char* p = open;
                for (; *p; p++) {
                    if (*p == '(') depth++;
                    else if (*p == ')') { depth--; if (!depth) break; }
                }
                if (*p != ')') break;

                char scale[80];
                if (volume)
                    snprintf(scale, sizeof scale, " * float3(%.9g, %.9g, %.9g)",
                             1.0 / (double)w, 1.0 / (double)h, 1.0 / (double)d);
                else
                    snprintf(scale, sizeof scale, " * float2(%.9g, %.9g)",
                             1.0 / (double)w, 1.0 / (double)h);
                const u32 slen = (u32)strlen(scale);
                const u32 used = (u32)strlen(hlsl);
                if (used + slen + 1 > hlsl_size) return -1;

                memmove(p + slen, p, used - (u32)(p - hlsl) + 1);
                memcpy(p, scale, slen);
                patched++;

                /* Mark this call site done so the next strstr moves past it:
                 * the inserted text is inside the call, so an unmarked scan
                 * would find the same site forever. */
                memcpy(at, "rsx_TEX", 7);
            }
        }
        /* Restore the marker. */
        for (char* q = hlsl; (q = strstr(q, "rsx_TEX")) != NULL; )
            memcpy(q, "rsx_tex", 7);
    }
    return patched;
}

/*
 * WPOS on an antialiased surface (see the header).
 *
 * A backend that keeps a DIAGONAL_CENTERED_2 or SQUARE_*_4 target at its sample
 * resolution maps the viewport over the sample grid, so a pixel program runs
 * once per sample and SV_POSITION.x of a 960-pixel 2x1 target runs to 1920. A
 * program that addresses a screen-sized texture by WPOS then reads it at twice
 * the position: Resistance 3's material pass fetches its light and SSAO
 * buffers that way and drew its lighting shifted on the left half of the frame
 * and clamped to one edge column, full-width streaks, on the right half.
 *
 * Every sample of a pixel gets that pixel's centre, floor(pos / grid) + 0.5,
 * the half-pixel centre SV_POSITION already gives on a 1x1 grid. The line goes
 * first in main, before any read; z and w (the _rsx_inv_w source) are kept.
 * WPOS reaches the body only as the emitter's "_rsx_wpos(input.position)"
 * source (#589's window convention wraps it; _rsx_inv_w also reads
 * input.position, so match the wrapper), so a program without it stays
 * byte-identical. The grid applies first, then _rsx_wpos flips y in pixels.
 */
int rsx_fp_apply_wpos_grid(char* hlsl, u32 capacity, u32 sx, u32 sy)
{
    static const char wpos_read[] = "_rsx_wpos(input.position)";
    static const char* const entries[] = { "main(PSInput input) : SV_TARGET {\n",
                                           "main(PSInput input) {\n" };
    if (!hlsl || !capacity || !sx || !sy) return -1;
    if (sx == 1 && sy == 1) return 0;
    size_t len = 0;
    while (len < capacity && hlsl[len]) ++len;
    if (len == capacity) return -1;
    if (!strstr(hlsl, wpos_read)) return 0;
    if (strstr(hlsl, "input.position.xy = floor(")) return -1;
    char* body = NULL;
    for (size_t i = 0; i < sizeof entries / sizeof entries[0] && !body; ++i) {
        char* at = strstr(hlsl, entries[i]);
        if (at) body = at + strlen(entries[i]);
    }
    if (!body) return -1;
    char line[128];
    const int n = snprintf(line, sizeof line,
        "    input.position.xy = floor(input.position.xy / float2(%u, %u)) + 0.5;\n", sx, sy);
    if (n < 0 || (size_t)n >= sizeof line || len + (size_t)n + 1 > capacity) return -1;
    memmove(body + n, body, len - (size_t)(body - hlsl) + 1);
    memcpy(body, line, (size_t)n);
    return 1;
}

/* NV4097_SET_SHADER_PACKER accepts any nonzero value. Floating surfaces and
 * 32-bit exports bypass the integer framebuffer's RGB transfer function. */
int rsx_fp_srgb_output_enabled(u32 packer, u32 ctrl, u32 color_format)
{
    return packer != 0 && !(ctrl & FP_CTRL_32BIT_EXPORTS) &&
           color_format != 0x0Bu && color_format != 0x0Cu && color_format != 0x0Du;
}

int rsx_fp_apply_srgb_output(char* hlsl, u32 capacity)
{
    static const char helper[] =
        "float4 _rsx_srgb_output(float4 v) {\n"
        "    float3 lo = v.rgb * 12.92;\n"
        "    float3 hi = 1.055 * pow(max(v.rgb, (float3)0), (float3)(1.0 / 2.4)) - 0.055;\n"
        "    float3 rgb = float3(v.r < 0.0031308 ? lo.r : hi.r,\n"
        "                         v.g < 0.0031308 ? lo.g : hi.g,\n"
        "                         v.b < 0.0031308 ? lo.b : hi.b);\n"
        "    return float4(saturate(rgb), v.a);\n}\n";
    if (!hlsl || !capacity) return -1;
    size_t len = 0;
    while (len < capacity && hlsl[len]) ++len;
    if (len == capacity || strstr(hlsl, "_rsx_srgb_output")) return -1;
    char* marker = NULL;
    for (char* p = strstr(hlsl, "    return "); p;
         p = strstr(p + 1, "    return ")) marker = p;
    if (!marker) return -1;
    const char* expression = marker + strlen("    return ");
    const char* end = strstr(expression, ";\n}\n");
    if (!end || end == expression || (size_t)(end - expression) >= 96 || end[4])
        return -1;
    char output[96];
    memcpy(output, expression, (size_t)(end - expression));
    output[end - expression] = 0;
    const int mrt = strstr(hlsl, "struct PSOutput") != NULL;
    char suffix[1024];
    int n = snprintf(suffix, sizeof suffix, "    %s _rsx_color_out = %s;\n",
                     mrt ? "PSOutput" : "float4", output);
    if (n < 0 || (size_t)n >= sizeof suffix) return -1;
    if (mrt) {
        unsigned targets = 0;
        for (unsigned t = 0; t < 4; ++t) {
            char field[64];
            snprintf(field, sizeof field, "    float4 t%u : SV_TARGET%u;", t, t);
            if (!strstr(hlsl, field)) continue;
            const int added = snprintf(suffix + n, sizeof suffix - (size_t)n,
                "    _rsx_color_out.t%u = _rsx_srgb_output(_rsx_color_out.t%u);\n", t, t);
            if (added < 0 || (size_t)added >= sizeof suffix - (size_t)n) return -1;
            n += added; ++targets;
        }
        if (!targets) return -1;
    } else {
        const int added = snprintf(suffix + n, sizeof suffix - (size_t)n,
            "    _rsx_color_out = _rsx_srgb_output(_rsx_color_out);\n");
        if (added < 0 || (size_t)added >= sizeof suffix - (size_t)n) return -1;
        n += added;
    }
    const int added = snprintf(suffix + n, sizeof suffix - (size_t)n,
                              "    return _rsx_color_out;\n}\n");
    if (added < 0 || (size_t)added >= sizeof suffix - (size_t)n) return -1;
    n += added;
    const size_t prefix = (size_t)(marker - hlsl);
    const size_t helper_len = sizeof helper - 1;
    if (helper_len + prefix + (size_t)n + 1 > capacity) return -1;
    /* All parsing/capacity checks precede the first mutation. */
    memcpy(marker, suffix, (size_t)n + 1);
    memmove(hlsl + helper_len, hlsl, prefix + (size_t)n + 1);
    memcpy(hlsl, helper, helper_len);
    return 1;
}

int rsx_fp_decompile_buffered_color_ex(
    const u8* ucode, u32 max_bytes, u32 ctrl, u32 tex_cube_mask,
    const u8 gamma_masks[16], char* out, u32 out_size, u32* out_constant_count)
{
    return rsx_fp_decompile_internal(ucode, max_bytes, ctrl, tex_cube_mask, 0,
        1, NULL, gamma_masks, NULL, out, out_size, out_constant_count);
}
