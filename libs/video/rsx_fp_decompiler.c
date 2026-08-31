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
    OP_UP4UB=0x28, OP_PK2US=0x29, OP_UP2US=0x2A, OP_DP2A=0x2E, OP_TXL=0x2F,
    OP_TXB=0x31, OP_RFL=0x36, OP_DP2=0x38, OP_NRM=0x39, OP_DIV=0x3A,
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
    memset(out, 0, sizeof(*out));

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
    case 0x0: return "input.position"; /* WPOS */
    case 0x1: return "input.col0";     /* COL0 */
    case 0x2: return "input.col1";     /* COL1 */
    case 0x3: return "input.fog";      /* FOGC */
    default:
        if (input_src >= 0x4 && input_src <= 0xB) return tc[input_src - 0x4]; /* TC0..7 */
        return "float4(0,0,0,0)";      /* TC8/TC9/FACING not plumbed */
    }
}

/* Build the swizzled/negated/abs'd HLSL for one source into `buf`. */
static void emit_src(const Src* s, u32 input_src, const float* k, int has_k,
                     int buffered, u32 constant_slot, char* buf, u32 bufsz)
{
    char base[96];
    if (s->type == FP_REG_TYPE_TEMP) {
        snprintf(base, sizeof(base), "%s[%u]", s->half ? "h" : "r", s->index);
    } else if (s->type == FP_REG_TYPE_INPUT) {
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

int rsx_fp_decompile(const u8* ucode, u32 max_bytes, u32 ctrl, char* out, u32 out_size)
{
    return rsx_fp_decompile_ex(ucode, max_bytes, ctrl, 0u, out, out_size);
}

static int rsx_fp_decompile_internal(
    const u8* ucode, u32 max_bytes, u32 ctrl, u32 tex_cube_mask,
    int buffered, char* out, u32 out_size, u32* out_constant_count)
{
    if (!ucode || !out || out_size == 0) return -1;

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

    Out o = { out, out_size, 0, 1 };

    /* Preamble: PSInput matches the backend's placeholder layout; temp/half
     * register files; texture+sampler banks for TEX. */
    out_puts(&o,
        "struct PSInput {\n"
        "    float4 position : SV_POSITION; float4 col0 : COLOR0; float4 col1 : COLOR1;\n"
        "    float4 fog : FOG;\n"
        "    float4 tc0:TEXCOORD0; float4 tc1:TEXCOORD1; float4 tc2:TEXCOORD2; float4 tc3:TEXCOORD3;\n"
        "    float4 tc4:TEXCOORD4; float4 tc5:TEXCOORD5; float4 tc6:TEXCOORD6; float4 tc7:TEXCOORD7;\n"
        "};\n");
    /* Texture bank. With no cube units (the default) emit the exact legacy
     * array declaration so 2D-only programs are byte-identical. When any unit
     * is a cubemap, declare each unit individually at its t-register so the
     * cube units can be TextureCube while the rest stay Texture2D. */
    if (tex_cube_mask == 0) {
        out_puts(&o, "Texture2D    rsx_tex[16] : register(t0);\n");
    } else {
        char decl[64];
        for (u32 u = 0; u < 16; u++) {
            snprintf(decl, sizeof(decl), "%s rsx_tex%u : register(t%u);\n",
                     ((tex_cube_mask >> u) & 1u) ? "TextureCube" : "Texture2D  ", u, u);
            out_puts(&o, decl);
        }
    }
    out_puts(&o,
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
        out_puts(&o, constants_decl);
    }
    out_puts(&o,
        "float4 main(PSInput input) : SV_TARGET {\n"
        "    float4 r[48]; float4 h[48];\n"
        /* Fully initialise both register files: RSX programs routinely read a
         * register lane before writing it (the hardware reads undefined), but
         * HLSL rejects use-before-init (error X4000). Zero everything. */
        "    [unroll] for (int _i = 0; _i < 48; _i++) { r[_i] = (float4)0; h[_i] = (float4)0; }\n"
        /* Condition-code registers for exec_if predication / set_cond. Reset
         * to 0 (NV40 CC power-up state). Dead in non-predicated programs. */
        "    float4 cc0 = (float4)0; float4 cc1 = (float4)0;\n");

    int wrote_r0 = 0, wrote_h0 = 0;
    int count = 0;
    u32 off = 0;
    u32 constant_slot = 0;

    while (off + 16 <= max_bytes) {
        u32 w0 = rsx_fp_read_word(ucode + off + 0);
        u32 w1 = rsx_fp_read_word(ucode + off + 4);
        u32 w2 = rsx_fp_read_word(ucode + off + 8);
        u32 w3 = rsx_fp_read_word(ucode + off + 12);
        off += 16;
        count++;

        u32 opcode    = (w0 & FP_OPCODE_MASK) >> FP_OPCODE_SHIFT;
        u32 input_src = (w0 & FP_INPUT_SRC_MASK) >> FP_INPUT_SRC_SHIFT;
        u32 tex_unit  = (w0 & FP_TEX_UNIT_MASK) >> FP_TEX_UNIT_SHIFT;
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

        char a[200], b[200], c[200];
        emit_src(
            &s0, input_src, k, has_k, buffered, constant_slot,
            a, sizeof(a));
        emit_src(
            &s1, input_src, k, has_k, buffered, constant_slot,
            b, sizeof(b));
        emit_src(
            &s2, input_src, k, has_k, buffered, constant_slot,
            c, sizeof(c));
        if (has_k)
            constant_slot++;

        if (is_branch) {
            out_puts(&o, "    /* TODO: branch/flow-control op skipped */\n");
            if (w0 & FP_END) break;
            continue;
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
        /* Texture/branch fences: ordering hints with no result -- no-op. */
        case OP_FENCT: case OP_FENCB: rhs[0] = '\0'; handled = 0; break;
        case OP_SLT: snprintf(rhs, sizeof(rhs), "(float4)((%s) <  (%s))", a, b); break;
        case OP_SGE: snprintf(rhs, sizeof(rhs), "(float4)((%s) >= (%s))", a, b); break;
        case OP_SLE: snprintf(rhs, sizeof(rhs), "(float4)((%s) <= (%s))", a, b); break;
        case OP_SGT: snprintf(rhs, sizeof(rhs), "(float4)((%s) >  (%s))", a, b); break;
        case OP_SNE: snprintf(rhs, sizeof(rhs), "(float4)((%s) != (%s))", a, b); break;
        case OP_SEQ: snprintf(rhs, sizeof(rhs), "(float4)((%s) == (%s))", a, b); break;
        case OP_TEX:
            if ((tex_cube_mask >> tex_unit) & 1u)
                /* Cubemap: sample with the full 3-component direction vector. */
                snprintf(rhs, sizeof(rhs),
                         "rsx_tex%u.Sample(rsx_samp[%u], (%s).xyz)", tex_unit, tex_unit, a);
            else if (tex_cube_mask)
                snprintf(rhs, sizeof(rhs),
                         "rsx_tex%u.Sample(rsx_samp[%u], (%s).xy)", tex_unit, tex_unit, a);
            else
                snprintf(rhs, sizeof(rhs),
                         "rsx_tex[%u].Sample(rsx_samp[%u], (%s).xy)", tex_unit, tex_unit, a);
            break;
        case OP_TXP:
            if ((tex_cube_mask >> tex_unit) & 1u)
                /* Projective divide is meaningless for a cube lookup; sample
                 * the direction directly. */
                snprintf(rhs, sizeof(rhs),
                         "rsx_tex%u.Sample(rsx_samp[%u], (%s).xyz)", tex_unit, tex_unit, a);
            else if (tex_cube_mask)
                snprintf(rhs, sizeof(rhs),
                         "rsx_tex%u.Sample(rsx_samp[%u], (%s).xy / (%s).w)",
                         tex_unit, tex_unit, a, a);
            else
                snprintf(rhs, sizeof(rhs),
                         "rsx_tex[%u].Sample(rsx_samp[%u], (%s).xy / (%s).w)",
                         tex_unit, tex_unit, a, a);
            break;
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
        default:
            out_puts(&o, "    /* TODO: unhandled FP opcode ");
            out_puts(&o, rsx_fp_opcode_name(opcode));
            out_puts(&o, " */\n");
            handled = 0;
            break;
        }

        int has_dest = !(w0 & FP_OUT_NONE);
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
                         * lerp(dst, _v, mask) == mask ? _v : dst since the
                         * mask is exactly 0.0/1.0 from the bool->float cast. */
                        snprintf(buf, sizeof(buf),
                                 "      float4 _p = (float4)((cc%u).%s %s (float4)0.0);\n"
                                 "      %s[%u].%s = lerp(%s[%u].%s, _v.%s, _p.%s);\n",
                                 cc_read, cswz, cmp_op,
                                 reg, dst_idx, m, reg, dst_idx, m, m, m);
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

            if (has_dest && !dst_half && dst_idx == 0) wrote_r0 = 1;
            if (has_dest && dst_half  && dst_idx == 0) wrote_h0 = 1;
        }

        if (w0 & FP_END) break;
    }

    /* Fragment color output register selection. The NV40 hardware picks the
     * output from the SHADER_CONTROL word's 32_BITS_EXPORTS bit, not from
     * whichever temp happened to be written: fp32-export programs output r0,
     * fp16-export programs (the default) output h0 (RPCS3
     * FragmentProgramDecompiler::BuildCode). Many real material/lighting
     * shaders accumulate into fp32 temps (r2/r3/r4) but write their FINAL
     * color to h0 — the old "wrote_r0 => return r0" heuristic returned a stale
     * intermediate, producing flat/constant surfaces. */
    if (ctrl == RSX_FP_CTRL_AUTO) {
        /* No control word (standalone tests): legacy heuristic. */
        if (wrote_r0 || !wrote_h0)
            out_puts(&o, "    float4 _o = r[0];\n"
                         "    return (_o == _o) ? _o : (float4)0;\n}\n");
        else
            out_puts(&o, "    float4 _o = h[0];\n"
                         "    return (_o == _o) ? _o : (float4)0;\n}\n");
    } else if (ctrl & FP_CTRL_32BIT_EXPORTS) {
        out_puts(&o, "    float4 _o = r[0];\n"
                     "    return (_o == _o) ? _o : (float4)0;\n}\n");
    } else {
        out_puts(&o, "    float4 _o = h[0];\n"
                     "    return (_o == _o) ? _o : (float4)0;\n}\n");
    }
    (void)wrote_r0; (void)wrote_h0;

    if (!o.ok) return -1;
    return count;
}

int rsx_fp_decompile_ex(const u8* ucode, u32 max_bytes, u32 ctrl,
                        u32 tex_cube_mask, char* out, u32 out_size)
{
    return rsx_fp_decompile_internal(
        ucode, max_bytes, ctrl, tex_cube_mask, 0, out, out_size, NULL);
}

int rsx_fp_decompile_buffered_ex(
    const u8* ucode, u32 max_bytes, u32 ctrl, u32 tex_cube_mask,
    char* out, u32 out_size, u32* out_constant_count)
{
    return rsx_fp_decompile_internal(
        ucode, max_bytes, ctrl, tex_cube_mask, 1, out, out_size,
        out_constant_count);
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

    static const char* pass_expr[8] = {
        "false", "_rsx_out.a < _rsx_alpha_ref",
        "_rsx_out.a == _rsx_alpha_ref", "_rsx_out.a <= _rsx_alpha_ref",
        "_rsx_out.a > _rsx_alpha_ref", "_rsx_out.a != _rsx_alpha_ref",
        "_rsx_out.a >= _rsx_alpha_ref", "true"
    };
    char suffix[512];
    const int n = snprintf(suffix, sizeof(suffix),
        "    float4 _rsx_out = %s;\n"
        "    const float _rsx_alpha_ref = %.9g;\n"
        "    if (!(%s)) discard;\n"
        "    return _rsx_out;\n}\n",
        output, (double)ref, pass_expr[cmp]);
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

    static const char* pass_expr[8] = {
        "false", "_rsx_out.a < _rsx_alpha_ref",
        "_rsx_out.a == _rsx_alpha_ref", "_rsx_out.a <= _rsx_alpha_ref",
        "_rsx_out.a > _rsx_alpha_ref", "_rsx_out.a != _rsx_alpha_ref",
        "_rsx_out.a >= _rsx_alpha_ref", "true"
    };
    char suffix[512];
    const int n = snprintf(
        suffix, sizeof(suffix),
        "    float4 _rsx_out = %s;\n"
        "    const float _rsx_alpha_ref = fp_alpha.x;\n"
        "    if (!(%s)) discard;\n"
        "    return _rsx_out;\n}\n",
        output, pass_expr[cmp]);
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
      if (dbg) { static int m = 0; if (m++ < 3) {
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
