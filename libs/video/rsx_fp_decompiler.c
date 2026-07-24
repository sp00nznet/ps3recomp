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

#include "rsx_fp_decompiler.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

/* Format one FP constant lane as an HLSL float literal. NaN/Inf constants
 * (a light-pass FP embeds them; %g prints the bare tokens "nan"/"inf" which
 * HLSL rejects -- X3004 undeclared identifier -> the whole PS fails to compile
 * and its pass renders black) are reproduced bit-exactly via asfloat(). */
static void fp_fmt_float(float v, char* buf, size_t n)
{
    if (isnan(v) || isinf(v)) {
        u32 bits; memcpy(&bits, &v, 4);
        snprintf(buf, n, "asfloat(0x%08Xu)", bits);
    } else {
        snprintf(buf, n, "%g", (double)v);
    }
}

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
        /* A CONST source pulls an inline 16-byte constant slot (branch
         * instructions reuse the src words as jump offsets -- no constant). */
        if (!(w2 & FP_BRANCH) &&
            (((w1 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST ||
             ((w2 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST ||
             ((w3 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST)) {
            if (off + 16 <= max_bytes) off += 16;
        }
        if (w0 & FP_END) return off;
    }
    return 0;
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
                     char* buf, u32 bufsz)
{
    char base[96];
    if (s->type == FP_REG_TYPE_TEMP) {
        /* h and r are modelled as SEPARATE register files. On hardware the
         * h pairs overlay r bit-wise (h0 = r0.xy bits, h1 = r0.zw bits), so
         * cgc code like dbgfont's [r0.w = coverage; h0 = colour; r0.w *=
         * h0.w] works because the h0 write leaves r0.w's bits intact --
         * a lane-wise alias (h[N] = r[N>>1]) clobbers it (cellmark text
         * flooded white). Value-level cross-view reads are rare; keep the
         * files separate and select the export bank via SET_SHADER_CONTROL. */
        snprintf(base, sizeof(base), "%s[%u]", s->half ? "h" : "r", s->index);
    } else if (s->type == FP_REG_TYPE_INPUT) {
        snprintf(base, sizeof(base), "%s", input_expr(input_src));
    } else { /* CONST */
        if (has_k) {
            char c0[24], c1[24], c2[24], c3[24];
            fp_fmt_float(k[0], c0, sizeof c0); fp_fmt_float(k[1], c1, sizeof c1);
            fp_fmt_float(k[2], c2, sizeof c2); fp_fmt_float(k[3], c3, sizeof c3);
            snprintf(base, sizeof(base), "float4(%s,%s,%s,%s)", c0, c1, c2, c3);
        } else
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

int rsx_fp_decompile(const u8* ucode, u32 max_bytes, char* out, u32 out_size,
                     int exports32)
{
    if (!ucode || !out || out_size == 0) return -1;

    Out o = { out, out_size, 0, 1 };

    /* Preamble: PSInput matches the backend's placeholder layout; temp/half
     * register files; texture+sampler banks for TEX. */
    out_puts(&o,
        "struct PSInput {\n"
        "    float4 position : SV_POSITION; float4 col0 : COLOR0; float4 col1 : COLOR1;\n"
        "    float4 fog : FOG;\n"
        "    float4 tc0:TEXCOORD0; float4 tc1:TEXCOORD1; float4 tc2:TEXCOORD2; float4 tc3:TEXCOORD3;\n"
        "    float4 tc4:TEXCOORD4; float4 tc5:TEXCOORD5; float4 tc6:TEXCOORD6; float4 tc7:TEXCOORD7;\n"
        "};\n"
        /* 4 discrete texture/sampler registers (t0-t3/s0-s3) rather than a [16]
         * array: the backend's root signature binds a 4-descriptor SRV table +
         * 4 static samplers, and an array declaration would require all 16
         * registers valid on tier-1 hardware. RSX FPs in practice use units
         * 0-3; higher units clamp to 3 in the TEX emission below. */
        "Texture2D    rsx_tex0 : register(t0); Texture2D rsx_tex1 : register(t1);\n"
        "Texture2D    rsx_tex2 : register(t2); Texture2D rsx_tex3 : register(t3);\n"
        /* Per-draw texcoord scale (b1): RSX textures with the UNnormalized
         * flag are sampled in texel space; the backend supplies 1/size for
         * those units and 1.0 for normalized ones. */
        "cbuffer FPTex : register(b1) { float4 rsx_texscale[4]; float4 rsx_alphatest; };\n"
        "SamplerState rsx_samp0 : register(s0); SamplerState rsx_samp1 : register(s1);\n"
        "SamplerState rsx_samp2 : register(s2); SamplerState rsx_samp3 : register(s3);\n"
        "struct PSOut { float4 c0:SV_Target0; float4 c1:SV_Target1;\n"
        "               float4 c2:SV_Target2; float4 c3:SV_Target3; };\n"
        "PSOut main(PSInput input) {\n"
        "    float4 r[48]; float4 h[48];\n"
        /* Fully initialise both register files: RSX programs routinely read a
         * register lane before writing it (the hardware reads undefined), but
         * HLSL rejects use-before-init (error X4000). Zero everything. */
        "    [unroll] for (int _i = 0; _i < 48; _i++) { r[_i] = (float4)0; h[_i] = (float4)0; }\n"
        /* NV40 FP condition registers: set_cond instructions latch their
         * result; later instructions execute per-lane where the (swizzled)
         * cc value passes the exec_if_lt/eq/gr test. */
        "    float4 cc0 = (float4)0, cc1 = (float4)0;\n");

    int wrote_r0 = 0, wrote_h0 = 0;
    int count = 0;
    u32 off = 0;
    /* Structured-branch bookkeeping (IFE): close/else points by ucode byte
     * offset. Stacks are small -- RSX FPs nest shallowly. */
    u32 else_offs[16]; int n_else = 0;
    u32 end_offs[16];  int n_end = 0;

    while (off + 16 <= max_bytes) {
        u32 w0 = rsx_fp_read_word(ucode + off + 0);
        u32 w1 = rsx_fp_read_word(ucode + off + 4);
        u32 w2 = rsx_fp_read_word(ucode + off + 8);
        u32 w3 = rsx_fp_read_word(ucode + off + 12);

        /* Close pending else/endif blocks that land at this offset. */
        if (n_else > 0 && else_offs[n_else-1] == off) {
            out_puts(&o, "    } else {\n");
            n_else--;
        }
        while (n_end > 0 && end_offs[n_end-1] == off) {
            out_puts(&o, "    } }\n");
            n_end--;
        }

        off += 16;
        count++;

        u32 opcode    = (w0 & FP_OPCODE_MASK) >> FP_OPCODE_SHIFT;
        u32 input_src = (w0 & FP_INPUT_SRC_MASK) >> FP_INPUT_SRC_SHIFT;
        u32 tex_unit  = (w0 & FP_TEX_UNIT_MASK) >> FP_TEX_UNIT_SHIFT;
        int is_branch = (w2 & FP_BRANCH) ? 1 : 0;

        Src s0, s1, s2;
        decode_src(w1, w1 & FP_SRC0_ABS, &s0);
        decode_src(w2, w2 & FP_SRC1_ABS, &s1);
        decode_src(w3, 0,                &s2);

        /* Per-instruction predication (SRC0 word): exec_if_lt/eq/gr @18-20,
         * cond swizzle @21-28, cond_mod reg @30 (set_cond target), cond reg
         * @31 (gate source). exec=7 -> unconditional; exec=0 -> never runs. */
        u32 exec_cond = (w1 >> 18) & 7u;
        int cond_reg  = (int)((w1 >> 31) & 1u);
        int cond_mod  = (int)((w1 >> 30) & 1u);
        int set_cond  = (w0 & FP_COND_WRITE) ? 1 : 0;
        char cswz[5];
        {
            static const char comp2[4] = {'x','y','z','w'};
            for (int i = 0; i < 4; i++) cswz[i] = comp2[(w1 >> (21 + 2*i)) & 3];
            cswz[4] = 0;
        }
        /* Result scale modifier (SRC1 word bits 28-30): the value is scaled
         * before saturation/write. demosaic's parity math is floor(coord)/2
         * then frac() -- without the /2 the parity network is constant 0. */
        u32 scale_bits = (w2 >> 28) & 7u;
        const char* scale = NULL;
        switch (scale_bits) {
        case 1: scale = " _v = _v * 2.0;";   break;
        case 2: scale = " _v = _v * 4.0;";   break;
        case 3: scale = " _v = _v * 8.0;";   break;
        case 5: scale = " _v = _v * 0.5;";   break;
        case 6: scale = " _v = _v * 0.25;";  break;
        case 7: scale = " _v = _v * 0.125;"; break;
        default: break;
        }

        const char* cmp = NULL;   /* NULL = unconditional */
        switch (exec_cond) {
        case 1: cmp = "< 0";  break;
        case 2: cmp = "== 0"; break;
        case 3: cmp = "<= 0"; break;
        case 4: cmp = "> 0";  break;
        case 5: cmp = "!= 0"; break;
        case 6: cmp = ">= 0"; break;
        default: break;       /* 7 unconditional, 0 never */
        }

        /* Inline constant: any CONST source pulls the next 16 bytes as a
         * float4 literal and advances past it. */
        float k[4] = {0,0,0,0};
        int has_k = 0;
        if (!is_branch &&
            (s0.type == FP_REG_TYPE_CONST || s1.type == FP_REG_TYPE_CONST ||
             s2.type == FP_REG_TYPE_CONST)) {
            if (off + 16 <= max_bytes) {
                for (int i = 0; i < 4; i++) {
                    u32 cw = rsx_fp_read_word(ucode + off + i * 4);
                    memcpy(&k[i], &cw, 4);
                }
                off += 16;
            }
            has_k = 1;
        }

        char a[200], b[200], c[200];
        emit_src(&s0, input_src, k, has_k, a, sizeof(a));
        emit_src(&s1, input_src, k, has_k, b, sizeof(b));
        emit_src(&s2, input_src, k, has_k, c, sizeof(c));

        if (is_branch) {
            /* Branch opcodes live in a second table (base | 0x40): 0x42 = IFE
             * (if/else/endif); SRC1/SRC2 carry the else and endif ucode byte
             * offsets. Others (LOOP/REP/CAL) still unhandled. */
            if (opcode == 0x02 /* IFE = 0x42 & 0x3F */) {
                /* SRC1/SRC2 store the else/endif targets in words (<< 2 for
                 * ucode byte offsets); bit 31 of SRC1 is the branch flag. */
                u32 else_b = (w2 & 0x7FFFFFFFu) << 2;
                u32 end_b  = (w3 & 0x7FFFFFFFu) << 2;
                char bl[220];
                if (cmp)
                    snprintf(bl, sizeof(bl),
                             "    { float4 _cs = cc%d.%s; if (any(_cs %s)) {\n",
                             cond_reg, cswz, cmp);
                else
                    snprintf(bl, sizeof(bl), "    { if (true) {\n");
                out_puts(&o, bl);
                if (n_end < 16) end_offs[n_end++] = end_b;
                if (else_b != end_b && else_b > off && n_else < 16)
                    else_offs[n_else++] = else_b;
            } else {
                out_puts(&o, "    /* TODO: branch/flow-control op skipped */\n");
            }
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
        case OP_RSQ: snprintf(rhs, sizeof(rhs), "rsqrt((%s).x)", a); break;
        case OP_EX2: snprintf(rhs, sizeof(rhs), "exp2((%s).x)", a); break;
        case OP_LG2: snprintf(rhs, sizeof(rhs), "log2((%s).x)", a); break;
        case OP_COS: snprintf(rhs, sizeof(rhs), "cos((%s).x)", a); break;
        case OP_SIN: snprintf(rhs, sizeof(rhs), "sin((%s).x)", a); break;
        case OP_POW: snprintf(rhs, sizeof(rhs), "pow((%s).x, (%s).x)", a, b); break;
        case OP_DIV: snprintf(rhs, sizeof(rhs), "(%s) / (%s).x", a, b); break;
        case OP_DIVSQ: snprintf(rhs, sizeof(rhs), "(%s) / sqrt((%s).x)", a, b); break;
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
            snprintf(rhs, sizeof(rhs),
                     "rsx_tex%u.Sample(rsx_samp%u, (%s).xy * rsx_texscale[%u].xy)",
                     tex_unit & 3u, tex_unit & 3u, a, tex_unit & 3u);
            break;
        case OP_TXP:
            snprintf(rhs, sizeof(rhs),
                     "rsx_tex%u.Sample(rsx_samp%u, (%s).xy / (%s).w * rsx_texscale[%u].xy)",
                     tex_unit & 3u, tex_unit & 3u, a, a, tex_unit & 3u);
            break;
        case OP_KIL: {
            if (exec_cond != 0) {
                char kl[220];
                if (cmp)
                    snprintf(kl, sizeof(kl),
                             "    { float4 _cs = cc%d.%s; if (any(_cs %s)) discard; }\n",
                             cond_reg, cswz, cmp);
                else
                    snprintf(kl, sizeof(kl), "    discard;\n");
                out_puts(&o, kl);
            }
            handled = 0;
            break; }
        default:
            out_puts(&o, "    /* TODO: unhandled FP opcode ");
            out_puts(&o, rsx_fp_opcode_name(opcode));
            out_puts(&o, " */\n");
            handled = 0;
            break;
        }

        if (handled && exec_cond != 0 && (!(w0 & FP_OUT_NONE) || set_cond)) {
            u32 dst_idx = (w0 & FP_OUT_REG_MASK) >> FP_OUT_REG_SHIFT;
            int dst_half = (w0 & FP_OUT_HALF) ? 1 : 0;
            const char* rf = dst_half ? "h" : "r";
            char m[5];
            dest_mask(w0, m);

            /* Broadcast the result to float4 first so the write-mask picks
             * the CORRESPONDING components: `dst.w = rhs` would HLSL-truncate
             * a vector rhs to its .x, but NV40 writes rhs.w to dst.w. Scalar
             * results (DPx/RCP/...) replicate, which is also the hardware
             * behavior. */
            char line[2048];
            int pos = 0;
            const char* sat = (w0 & FP_OUT_SAT) ? " _v = saturate(_v);" : "";
            pos += snprintf(line + pos, sizeof(line) - pos,
                            "    { float4 _v = (float4)(%s);%s%s",
                            rhs, scale ? scale : "", sat);
            if (cmp)
                pos += snprintf(line + pos, sizeof(line) - pos,
                                " float4 _cs = cc%d.%s;", cond_reg, cswz);
            if (!(w0 & FP_OUT_NONE)) {
                if (cmp) {
                    /* Per-lane predicated write: lane L updates only where
                     * the swizzled cc lane passes the test. */
                    for (const char* L = m; *L; L++)
                        pos += snprintf(line + pos, sizeof(line) - pos,
                                        " %s[%u].%c = (_cs.%c %s) ? _v.%c : %s[%u].%c;",
                                        rf, dst_idx, *L, *L, cmp, *L, rf, dst_idx, *L);
                } else {
                    pos += snprintf(line + pos, sizeof(line) - pos,
                                    " %s[%u].%s = _v.%s;", rf, dst_idx, m, m);
                }
            }
            if (set_cond) {
                if (cmp) {
                    for (const char* L = m; *L; L++)
                        pos += snprintf(line + pos, sizeof(line) - pos,
                                        " cc%d.%c = (_cs.%c %s) ? _v.%c : cc%d.%c;",
                                        cond_mod, *L, *L, cmp, *L, cond_mod, *L);
                } else {
                    pos += snprintf(line + pos, sizeof(line) - pos,
                                    " cc%d.%s = _v.%s;", cond_mod, m, m);
                }
            }
            snprintf(line + pos, sizeof(line) - pos, " }\n");
            out_puts(&o, line);

            if (!(w0 & FP_OUT_NONE) && dst_idx == 0) {
                if (dst_half) wrote_h0 = 1; else wrote_r0 = 1;
            }
        }

        if (w0 & FP_END) break;
    }

    /* Close any blocks whose end offset was never reached (defensive: a
     * mis-decoded target must not leave the HLSL unbalanced). */
    while (n_else-- > 0) out_puts(&o, "    } else {\n");
    while (n_end--  > 0) out_puts(&o, "    } }\n");

    /* Fragment colour outputs, selected by SET_SHADER_CONTROL: 32-bit
     * programs export r0/r2/r3/r4, half programs h0/h4/h6/h8 (wave's water
     * height FP writes its result to h0 and uses r0 lanes as scratch --
     * heuristics picked r0 and the pond stayed flat forever). Unbound MRT
     * writes are discarded. */
    /* Colour exports: SET_SHADER_CONTROL bit 0x40 selects the 32-bit bank
     * (r0/r2/r3/r4) vs half (h0/h4/h6/h8); fall back to whichever file the
     * program actually wrote for c0 (PSL1GHT runs half-export control over
     * r0-writing programs). Secondaries use the zero-init sum trick. */
    if (exports32 ? wrote_r0 : !wrote_h0)
        out_puts(&o, "    PSOut _po; _po.c0 = r[0];\n");
    else
        out_puts(&o, "    PSOut _po; _po.c0 = h[0];\n");
    out_puts(&o, "    _po.c1 = r[2] + h[4]; _po.c2 = r[3] + h[6];\n"
                 "    _po.c3 = r[4] + h[8];\n"
        /* Guest alpha test (D3D12 has none fixed-function): func codes are
         * RSX 0x200+f with f: 0=NEVER 1=LESS 2=EQUAL 3=LEQUAL 4=GREATER
         * 5=NOTEQUAL 6=GEQUAL 7=ALWAYS. */
        "    if (rsx_alphatest.x != 0.0) {\n"
        "        float _a = _po.c0.a, _r = rsx_alphatest.y;\n"
        "        int _f = (int)rsx_alphatest.z;\n"
        "        bool _p = (_f == 7) || (_f == 1 && _a < _r) || (_f == 2 && _a == _r) ||\n"
        "                  (_f == 3 && _a <= _r) || (_f == 4 && _a > _r) ||\n"
        "                  (_f == 5 && _a != _r) || (_f == 6 && _a >= _r);\n"
        "        if (!_p) discard;\n"
        "    }\n"
        "    return _po;\n}\n");

    if (!o.ok) return -1;
    return count;
}
