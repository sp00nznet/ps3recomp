/* Regression test: RSX TC8/TC9 fragment/vertex stage linkage.
 *
 * Every program below is hand-assembled raw RSX microcode fed straight to the
 * decompilers; nothing here inspects decompiler source text. Output HLSL is
 * whitespace-compacted before matching so harmless formatting changes pass.
 *
 * FP input attribute codes (word0 [13:16]): 0 WPOS, 1 COL0, 2 COL1, 3 FOGC,
 * 4..13 TC0..TC9. VP outputs o[15]/o[6] carry TEXCOORD8/TEXCOORD9.
 */
#include "rsx_fp_decompiler.h"
#include "rsx_vp_decompiler.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define HLSL_SIZE 32768

static int failures;
static int checks;

/* ---- FP encoding: halfword-swapped, then big-endian bytes ---- */
static void put_word(u8* p, u32 h)
{
    u32 be = (h << 16) | (h >> 16);
    p[0] = (u8)(be >> 24); p[1] = (u8)(be >> 16);
    p[2] = (u8)(be >> 8);  p[3] = (u8)(be);
}

#define OPC(x)      ((u32)(x) << 24)
#define OUTMASK_ALL (0xFu << 9)
#define END         (1u << 0)
#define INSRC(x)    ((u32)(x) << 13)
#define INSRC_MASK  (0xFu << 13)
#define SWZ_IDENT   ((0u<<9)|(1u<<11)|(2u<<13)|(3u<<15))
#define T_INPUT     1u
/* Unconditional exec_if; an all-zero condition would mean "never". */
#define EXEC_ALWAYS ((7u << 18) | (0u << 21) | (1u << 23) | (2u << 25) | (3u << 27))
#define CTRL_32BIT  0x40u

#define FP_MOV 0x01u

/* Captured host words for NRM h3.xyz, TC8 (input code 12). */
#define NRM_TC8_W0 0x39418E86u
#define NRM_TC8_W1 0x1C9DC801u

/* ---- VP encoding: little-endian dwords ---- */
static void vp_word(u8* p, u32 v)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (u8)(v >> (i * 8));
}

#define VP_MOV        1u
#define VP_REG_INPUT  2u
#define VP_SWZ_IDENT  (0x1Bu << 8)

/* ---- text helpers ---- */
static void compact(const char* in, char* out, size_t size)
{
    size_t n = 0;
    for (; *in && n + 1 < size; ++in)
        if (!isspace((unsigned char)*in)) out[n++] = *in;
    out[n] = 0;
}

/* Finds needle not immediately followed by an identifier character, so
 * "input.tc1" never matches inside "input.tc10". */
static const char* find_ident(const char* hay, const char* needle)
{
    size_t len = strlen(needle);
    for (const char* p = strstr(hay, needle); p; p = strstr(p + 1, needle)) {
        unsigned char c = (unsigned char)p[len];
        if (!isalnum(c) && c != '_') return p;
    }
    return NULL;
}

/* Copies the statement starting at the first occurrence of lead up to ';'. */
static int statement(const char* flat, const char* lead, char* out, size_t size)
{
    const char* p = strstr(flat, lead);
    size_t n = 0;
    out[0] = 0;
    if (!p) return 0;
    /* The decompiler evaluates into _v before the masked destination store. */
    const char* begin = p;
    while (begin > flat && begin[-1] != '{' && begin[-1] != '}') --begin;
    p = begin;
    while (p[n] && p[n] != ';' && n + 1 < size) { out[n] = p[n]; ++n; }
    out[n] = 0;
    return 1;
}

/* Finds a write to reg (e.g. "o[15]") of the form reg[.mask]=rhs (not ==);
 * copies rhs up to ';'. */
static int find_assign(const char* flat, const char* reg, char* rhs, size_t size)
{
    size_t len = strlen(reg);
    for (const char* p = strstr(flat, reg); p; p = strstr(p + 1, reg)) {
        const char* q = p + len;
        if (*q == '.') {
            ++q;
            while (*q && strchr("xyzw", *q)) ++q;
        }
        if (q[0] == '=' && q[1] != '=') {
            size_t n = 0;
            ++q;
            const char* begin = p;
            while (begin > flat && begin[-1] != '{' && begin[-1] != '}') --begin;
            if (strstr(begin,"float4_v=") == begin) q = begin + 10;
            while (q[n] && q[n] != ';' && n + 1 < size) { rhs[n] = q[n]; ++n; }
            rhs[n] = 0;
            return 1;
        }
    }
    rhs[0] = 0;
    return 0;
}

static void check(const char* test, const char* what, int ok, const char* flat)
{
    ++checks;
    if (ok) return;
    ++failures;
    fprintf(stderr, "FAIL [%s] %s\n", test, what);
    if (flat) fprintf(stderr, "  compacted HLSL: %.3000s\n", flat);
}

static u32 swizzle_bits(const char* s)
{
    u32 bits = 0;
    for (unsigned k = 0; k < 4; ++k) {
        static const char components[] = "xyzw";
        u32 idx = (u32)(strchr(components, s[k]) - components);
        bits |= idx << (9 + 2 * k);
    }
    return bits;
}

static int fp_single(u32 w0, u32 w1, char* hlsl, char* flat)
{
    u8 prog[16];
    put_word(prog + 0, w0);
    put_word(prog + 4, w1);
    put_word(prog + 8, 0);
    put_word(prog + 12, 0);
    hlsl[0] = 0;
    int n = rsx_fp_decompile(prog, sizeof(prog), CTRL_32BIT, hlsl, HLSL_SIZE);
    compact(hlsl, flat, HLSL_SIZE);
    return n;
}

/* The statement must reference only the expected input among all varyings. */
static void check_only_input(const char* test, const char* stmt, const char* expect, const char* flat)
{
    static const char* const all[] = {
        "input.col0", "input.col1", "input.fogc", "input.fog",
        "input.tc0", "input.tc1", "input.tc2", "input.tc3", "input.tc4",
        "input.tc5", "input.tc6", "input.tc7", "input.tc8", "input.tc9",
    };
    char what[96];
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
        if (!strcmp(all[i], expect)) continue;
        if (!strcmp(expect, "input.fogc") && !strcmp(all[i], "input.fog")) continue;
        if (!strcmp(expect, "input.fog") && !strcmp(all[i], "input.fogc")) continue;
        snprintf(what, sizeof(what), "must not alias %s", all[i]);
        check(test, what, find_ident(stmt, all[i]) == NULL, flat);
    }
}

static void test_fp_tc_moves(char* hlsl, char* flat)
{
    static const char* const swz[10] = {
        "wzyx", "yzwx", "zwxy", "xxyy", "wwzz",
        "yxwz", "zyxw", "xwyz", "wxyz", "yyxx",
    };
    char test[64], need[64], stmt[512];

    for (unsigned i = 0; i < 10; ++i) {
        for (int variant = 0; variant < 2; ++variant) {
            u32 sw = variant ? swizzle_bits(swz[i]) : SWZ_IDENT;
            snprintf(test, sizeof(test), "fp_mov_tc%u_%s", i, variant ? swz[i] : "ident");
            int n = fp_single(OPC(FP_MOV) | OUTMASK_ALL | INSRC(4 + i) | END,
                              T_INPUT | sw | EXEC_ALWAYS, hlsl, flat);
            check(test, "decodes exactly one instruction", n == 1, flat);

            int found = statement(flat, "r[0].xyzw=", stmt, sizeof(stmt));
            check(test, "writes r[0].xyzw", found, flat);

            snprintf(need, sizeof(need), "input.tc%u", i);
            check(test, "references the matching texcoord input", find_ident(stmt, need) != NULL, flat);
            check(test, "no zero substitution", strstr(stmt, "float4(0") == NULL, flat);
            check_only_input(test, stmt, need, flat);

            if (variant) {
                snprintf(need, sizeof(need), "input.tc%u).%s", i, swz[i]);
                check(test, "keeps the non-identity swizzle", strstr(stmt, need) != NULL, flat);
            }

            if (i >= 7) {
                snprintf(need, sizeof(need), "float4tc%u:TEXCOORD%u", i, i);
                check(test, "PSInput declares the texcoord", find_ident(flat, need) != NULL, flat);
            }
        }
    }
}

static void test_fp_nrm_tc8(char* hlsl, char* flat)
{
    static const struct { u32 code; const char* name; const char* input; } cases[] = {
        { 12, "fp_nrm_tc8_captured", "input.tc8" },
        { 11, "fp_nrm_tc7_control",  "input.tc7" },
        { 13, "fp_nrm_tc9_control",  "input.tc9" },
    };
    char stmt[512];

    check("fp_nrm_tc8_captured", "captured word carries input code 12",
          ((NRM_TC8_W0 & INSRC_MASK) >> 13) == 12, NULL);

    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
        u32 w0 = ((NRM_TC8_W0 & ~INSRC_MASK) | INSRC(cases[c].code)) | END;
        int n = fp_single(w0, NRM_TC8_W1, hlsl, flat);
        check(cases[c].name, "decodes exactly one instruction", n == 1, flat);

        const char* p = find_ident(flat, cases[c].input);
        check(cases[c].name, "references the decoded texcoord input", p != NULL, flat);
        if (p) {
            /* Isolate the statement holding the reference. */
            const char* s = p;
            while (s > flat && s[-1] != ';' && s[-1] != '{' && s[-1] != '}') --s;
            size_t k = 0;
            while (s[k] && s[k] != ';' && k + 1 < sizeof(stmt)) { stmt[k] = s[k]; ++k; }
            stmt[k] = 0;
            check(cases[c].name, "no zero substitution", strstr(stmt, "float4(0") == NULL, flat);
            check_only_input(cases[c].name, stmt, cases[c].input, flat);
        }
    }
}

static void test_fp_legacy_inputs(char* hlsl, char* flat)
{
    static const struct { u32 code; const char* name; const char* input; } cases[] = {
        { 1, "fp_mov_col0", "input.col0" },
        { 2, "fp_mov_col1", "input.col1" },
        { 3, "fp_mov_fogc", "input.fog" },
    };
    char stmt[512];

    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
        int n = fp_single(OPC(FP_MOV) | OUTMASK_ALL | INSRC(cases[c].code) | END,
                          T_INPUT | SWZ_IDENT | EXEC_ALWAYS, hlsl, flat);
        check(cases[c].name, "decodes exactly one instruction", n == 1, flat);
        int found = statement(flat, "r[0].xyzw=", stmt, sizeof(stmt));
        check(cases[c].name, "writes r[0].xyzw", found, flat);
        check(cases[c].name, "references the legacy input", strstr(stmt, cases[c].input) != NULL, flat);
        check(cases[c].name, "does not reference a texcoord", strstr(stmt, "input.tc") == NULL, flat);
    }
}

static void vp_emit_mov_out(u8* b, unsigned input, unsigned output, int end)
{
    u32 src = VP_REG_INPUT | VP_SWZ_IDENT;
    vp_word(b + 0, (63u << 15) | (1u << 30));                      /* no temp, vec result -> output */
    vp_word(b + 4, (VP_MOV << 22) | ((u32)input << 8) | (src >> 9));
    vp_word(b + 8, (src & 511u) << 23);
    vp_word(b + 12, ((u32)output << 2) | (63u << 7) | (0xFu << 13) | (end ? 1u : 0u));
}

static void test_vp_tc8_tc9(char* hlsl, char* flat)
{
    const char* t = "vp_o15_o6";
    u8 prog[3 * 16];
    char rhs15[256], rhs6[256], rhs0[256];

    vp_emit_mov_out(prog + 0, 0, 0, 0);    /* o0  = v0  */
    vp_emit_mov_out(prog + 16, 9, 15, 0);  /* o15 = v9  */
    vp_emit_mov_out(prog + 32, 10, 6, 1); /* o6 = v10, END */

    hlsl[0] = 0;
    int n = rsx_vp_decompile(prog, sizeof(prog), hlsl, HLSL_SIZE);
    compact(hlsl, flat, HLSL_SIZE);

    check(t, "decodes exactly three instructions", n == 3, flat);
    check(t, "o[0] write survives", find_assign(flat, "o[0]", rhs0, sizeof(rhs0)), flat);
    int has15 = find_assign(flat, "o[15]", rhs15, sizeof(rhs15));
    int has6 = find_assign(flat, "o[6]", rhs6, sizeof(rhs6));
    check(t, "o[15] write survives", has15, flat);
    check(t, "o[6] write survives", has6, flat);
    if (has15 && has6)
        check(t, "o[15] and o[6] sources are distinct", strcmp(rhs15, rhs6) != 0, flat);

    check(t, "VSOutput declares t8:TEXCOORD8", find_ident(flat, "float4t8:TEXCOORD8") != NULL, flat);
    check(t, "VSOutput declares t9:TEXCOORD9", find_ident(flat, "float4t9:TEXCOORD9") != NULL, flat);
    check(t, "Out.t8 = o[15]", strstr(flat, "Out.t8=o[15];") != NULL, flat);
    check(t, "Out.t9 = o[6]", strstr(flat, "Out.t9=o[6];") != NULL, flat);

    check(t, "t8 not linked to o[6]", strstr(flat, "Out.t8=o[6]") == NULL, flat);
    check(t, "t9 not linked to o[15]", strstr(flat, "Out.t9=o[15]") == NULL, flat);
    check(t, "t7 not linked to o[15]", strstr(flat, "Out.t7=o[15]") == NULL, flat);
    check(t, "t7 not linked to o[6]", strstr(flat, "Out.t7=o[6]") == NULL, flat);
    /* RSX has16 output registers;16 must fail, never wrap to position o0. */
    vp_emit_mov_out(prog,10,16,1);
    check(t,"invalid output16 rejects",rsx_vp_decompile(prog,16,hlsl,HLSL_SIZE)<0,NULL);
    vp_emit_mov_out(prog,10,30,1);
    check(t,"invalid output30 rejects",rsx_vp_decompile(prog,16,hlsl,HLSL_SIZE)<0,NULL);

}

int main(void)
{
    static char hlsl[HLSL_SIZE];
    static char flat[HLSL_SIZE];

    test_fp_tc_moves(hlsl, flat);
    test_fp_nrm_tc8(hlsl, flat);
    test_fp_legacy_inputs(hlsl, flat);
    test_vp_tc8_tc9(hlsl, flat);

    if (failures) {
        fprintf(stderr, "test_shader_tc8: %d of %d checks FAILED\n", failures, checks);
        return 1;
    }
    printf("test_shader_tc8: all %d checks passed\n", checks);
    return 0;
}
