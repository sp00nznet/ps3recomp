/*
 * Standalone unit test for the RSX fragment-program decompiler.
 *
 * Build (no D3D12 dependency), from this directory:
 *   cc -std=c11 -Wall -Wextra -I../../../include \
 *       test_fp_decompiler.c ../rsx_fp_decompiler.c -o test_fp
 *
 * Exercises the decode path (word swap, source swizzle/type, opcode -> HLSL,
 * dest mask, inline constants) by hand-assembling tiny programs and checking
 * the generated HLSL contains the expected expressions, then the colour
 * export selection: which register file the fragment colour comes out of, and
 * how many SV_TARGETs a program that writes more than one export gets.
 */

#include "../rsx_fp_decompiler.h"
#include <stdio.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;

/* Store host word H into guest bytes so that rsx_fp_read_word(p) == H.
 * rsx_fp_read_word does be32-load then 16-bit half-swap, so we invert: the
 * guest stores big-endian of swap16(H). */
static void put_word(u8* p, u32 h)
{
    u32 be = (h << 16) | (h >> 16);
    p[0] = (u8)(be >> 24); p[1] = (u8)(be >> 16);
    p[2] = (u8)(be >> 8);  p[3] = (u8)(be);
}

static void put_float(u8* p, float f)
{
    u32 h; memcpy(&h, &f, 4);
    put_word(p, h);
}

static void check(const char* test, const char* hlsl, const char* needle)
{
    if (strstr(hlsl, needle)) {
        printf("[PASS] %s -- found \"%s\"\n", test, needle);
        g_pass++;
    } else {
        printf("[FAIL] %s -- missing \"%s\"\nHLSL:\n%s\n", test, needle, hlsl);
        g_fail++;
    }
}

static void check_absent(const char* test, const char* hlsl, const char* needle)
{
    if (!strstr(hlsl, needle)) {
        printf("[PASS] %s -- no \"%s\"\n", test, needle);
        g_pass++;
    } else {
        printf("[FAIL] %s -- unexpected \"%s\"\nHLSL:\n%s\n", test, needle, hlsl);
        g_fail++;
    }
}

static void check_true(const char* test, int cond)
{
    if (cond) { printf("[PASS] %s\n", test); g_pass++; }
    else      { printf("[FAIL] %s\n", test); g_fail++; }
}

/* OPDEST/SRC bit helpers (mirror the decoder's macros). */
#define OPC(x)      ((u32)(x) << 24)
#define OUTMASK_ALL (0xFu << 9)
#define END         (1u << 0)
#define OUTREG(i)   ((u32)(i) << 1)
#define OUTHALF     (1u << 7)
#define INSRC(x)    ((u32)(x) << 13)
#define TEXU(x)     ((u32)(x) << 17)
#define SWZ_IDENT   ((0u<<9)|(1u<<11)|(2u<<13)|(3u<<15))
#define T_TEMP      0u
#define T_INPUT     1u
#define T_CONST     2u
#define REG(i)      ((u32)(i) << 2)
/* The execution condition, exec_if lt/eq/gt at [18:20] with its swizzle at
 * [21:28]. All three bits set is unconditional; an all-zero source word is
 * "never", which suppresses the write -- so every instruction below carries
 * this, as a real program's does. */
#define EXEC_ALWAYS ((7u << 18) | (0u << 21) | (1u << 23) | (2u << 25) | (3u << 27))

/* SHADER_CONTROL: 0x40 exports fp32 (colour from r0/r2/r3/r4), clear exports
 * fp16 (colour from h0/h4/h6/h8). */
#define CTRL_32BIT  0x40u
#define CTRL_16BIT  0x00u

int main(void)
{
    char hlsl[16384];

    /* Test 1: MOV r0, COL0  (write all, END). */
    {
        u8 prog[16];
        put_word(prog + 0, OPC(0x01) | OUTMASK_ALL | INSRC(1) | END); /* MOV, COL0 */
        put_word(prog + 4, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);        /* src0 = INPUT */
        put_word(prog + 8, 0);
        put_word(prog + 12, 0);
        int n = rsx_fp_decompile(prog, sizeof(prog), CTRL_32BIT, hlsl, sizeof(hlsl));
        printf("-- test1 MOV: %d instr\n", n);
        check("mov_dest",   hlsl, "r[0].xyzw =");
        check("mov_input",  hlsl, "input.col0");
        check("mov_entry",  hlsl, "float4 main(PSInput input) : SV_TARGET {");
        check("mov_return", hlsl, "    float4 _o = r[0];\n"
                                  "    return (_o == _o) ? _o : (float4)0;\n}\n");
    }

    /* Test 2: MUL r1.xy, r0, r0  (partial write mask). */
    {
        u8 prog[16];
        u32 mask_xy = (1u<<9)|(1u<<10);
        put_word(prog + 0, OPC(0x02) | mask_xy | OUTREG(1) | END); /* MUL, dest r1 */
        put_word(prog + 4, T_TEMP | REG(0) | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 8, T_TEMP | REG(0) | SWZ_IDENT);
        put_word(prog + 12, 0);
        rsx_fp_decompile(prog, sizeof(prog), CTRL_32BIT, hlsl, sizeof(hlsl));
        check("mul_mask", hlsl, "r[1].xy =");
        check("mul_expr", hlsl, "(r[0]).xyzw) * ((r[0]).xyzw)");
    }

    /* Test 3: MAD r0, r0, CONST, r0 with inline constant + saturate. */
    {
        u8 prog[32];
        put_word(prog + 0, OPC(0x04) | OUTMASK_ALL | (1u<<31) | END); /* MAD + SAT */
        put_word(prog + 4, T_TEMP  | REG(0) | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 8, T_CONST | SWZ_IDENT);                      /* src1 CONST */
        put_word(prog + 12, T_TEMP | REG(0) | SWZ_IDENT);
        put_float(prog + 16, 0.5f);  /* inline constant float4 */
        put_float(prog + 20, 0.25f);
        put_float(prog + 24, 0.0f);
        put_float(prog + 28, 1.0f);
        int n = rsx_fp_decompile(prog, sizeof(prog), CTRL_32BIT, hlsl, sizeof(hlsl));
        printf("-- test3 MAD: %d instr\n", n);
        check("mad_sat",   hlsl, "saturate(");
        check("mad_const", hlsl, "float4(0.5,0.25,0,1)");

        /* ...and the buffered variant, which hoists that constant into b1. */
        u32 nk = 0;
        rsx_fp_decompile_buffered_ex(prog, sizeof(prog), CTRL_32BIT, 0,
                                     hlsl, sizeof(hlsl), &nk);
        check_true("mad_buffered_count", nk == 1);
        check("mad_buffered", hlsl, "fp_constants[0]");
    }

    /* Test 4: TEX r0, TC0 from texture unit 3. */
    {
        u8 prog[16];
        put_word(prog + 0, OPC(0x17) | OUTMASK_ALL | INSRC(4) | TEXU(3) | END); /* TEX */
        put_word(prog + 4, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 8, 0);
        put_word(prog + 12, 0);
        rsx_fp_decompile(prog, sizeof(prog), CTRL_32BIT, hlsl, sizeof(hlsl));
        check("tex_sample", hlsl, "rsx_tex[3].Sample(rsx_samp[3]");
        check("tex_coord",  hlsl, "input.tc0");
    }

    /* Test 5: negate + abs on a source (MOV r0, -|r1|). */
    {
        u8 prog[16];
        put_word(prog + 0, OPC(0x01) | OUTMASK_ALL | END);
        put_word(prog + 4, T_TEMP | REG(1) | SWZ_IDENT | (1u<<17) | (1u<<29)
                           | EXEC_ALWAYS); /* neg+abs */
        put_word(prog + 8, 0);
        put_word(prog + 12, 0);
        rsx_fp_decompile(prog, sizeof(prog), CTRL_32BIT, hlsl, sizeof(hlsl));
        check("neg_abs", hlsl, "-(abs((r[1]).xyzw))");
    }

    /* Test 6: the export register file follows SHADER_CONTROL, not whichever
     * temp was written. A program writing h0 exports h0 with 16-bit exports
     * and r0 with 32-bit ones. */
    {
        u8 prog[16];
        put_word(prog + 0, OPC(0x01) | OUTMASK_ALL | INSRC(1) | OUTHALF | END);
        put_word(prog + 4, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 8, 0);
        put_word(prog + 12, 0);
        rsx_fp_decompile(prog, sizeof(prog), CTRL_16BIT, hlsl, sizeof(hlsl));
        check("h0_16bit", hlsl, "float4 _o = h[0];");
        rsx_fp_decompile(prog, sizeof(prog), CTRL_32BIT, hlsl, sizeof(hlsl));
        check("h0_32bit", hlsl, "float4 _o = r[0];");
    }

    /* Test 7: MRT. The second colour export is r2 with 32-bit exports and h4
     * with 16-bit ones, so the same instruction is a second render target in
     * one mode and an ordinary temp write in the other. A deferred pass
     * writing both planes has to come out as a struct of SV_TARGETs; a
     * material shader accumulating into r2 while its colour goes to h0 must
     * not. */
    {
        u8 prog[32];
        put_word(prog +  0, OPC(0x01) | OUTMASK_ALL | INSRC(1) | OUTREG(0));
        put_word(prog +  4, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog +  8, 0);
        put_word(prog + 12, 0);
        put_word(prog + 16, OPC(0x01) | OUTMASK_ALL | INSRC(2) | OUTREG(2) | END);
        put_word(prog + 20, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 24, 0);
        put_word(prog + 28, 0);

        int n = rsx_fp_decompile(prog, sizeof(prog), CTRL_32BIT, hlsl, sizeof(hlsl));
        printf("-- test7 MRT: %d instr\n", n);
        check("mrt_struct",  hlsl, "struct PSOutput {\n"
                                   "    float4 t0 : SV_TARGET0;\n"
                                   "    float4 t1 : SV_TARGET1;\n"
                                   "};\n");
        check("mrt_entry",   hlsl, "PSOutput main(PSInput input) {");
        check("mrt_t0",      hlsl, "float4 _o0 = r[0];\n"
                                   "    _out.t0 = (_o0 == _o0) ? _o0 : (float4)0;");
        check("mrt_t1",      hlsl, "float4 _o1 = r[2];\n"
                                   "    _out.t1 = (_o1 == _o1) ? _o1 : (float4)0;");
        check("mrt_return",  hlsl, "    return _out;\n}\n");
        check_absent("mrt_no_target2", hlsl, "SV_TARGET2");

        /* ...and the alpha test still patches it, testing target 0's alpha. */
        check_true("mrt_alpha_patched",
                   rsx_fp_apply_alpha_test(hlsl, sizeof(hlsl), 0x204u, 0.5f) == 1);
        check("mrt_alpha_type", hlsl, "PSOutput _rsx_out = _out;");
        check("mrt_alpha_cmp",  hlsl, "if (!(_rsx_out.t0.a > _rsx_alpha_ref)) discard;");

        /* The same program under 16-bit exports: r2 is a plain temp there, so
         * the colour is h0 alone and nothing about the output changes. */
        rsx_fp_decompile(prog, sizeof(prog), CTRL_16BIT, hlsl, sizeof(hlsl));
        check_absent("mrt_16bit_single", hlsl, "PSOutput");
        check("mrt_16bit_h0", hlsl, "float4 _o = h[0];");
    }

    /* Test 8: all four exports, and the highest written one decides how many
     * targets come out -- r3 alone still declares SV_TARGET0 through 2. */
    {
        u8 prog[16];
        put_word(prog + 0, OPC(0x01) | OUTMASK_ALL | INSRC(1) | OUTREG(3) | END);
        put_word(prog + 4, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 8, 0);
        put_word(prog + 12, 0);
        rsx_fp_decompile(prog, sizeof(prog), CTRL_32BIT, hlsl, sizeof(hlsl));
        check("r3_target0", hlsl, "float4 t0 : SV_TARGET0;");
        check("r3_target2", hlsl, "float4 t2 : SV_TARGET2;");
        check("r3_reads",   hlsl, "float4 _o2 = r[3];");
        check_absent("r3_no_target3", hlsl, "SV_TARGET3");
    }

    /* Test 9: the structural hash separates programs that differ only in
     * which export register they write -- so a pipeline cache keyed on it
     * cannot hand an MRT program the single-target program's shader. It is
     * the instruction bytes that are hashed, and the destination register is
     * one of them; nothing had to be added for the MRT case. */
    {
        u8 single[16], mrt[16];
        put_word(single + 0, OPC(0x01) | OUTMASK_ALL | INSRC(1) | OUTREG(0) | END);
        put_word(single + 4, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);
        put_word(single + 8, 0);
        put_word(single + 12, 0);
        memcpy(mrt, single, sizeof mrt);
        put_word(mrt + 0, OPC(0x01) | OUTMASK_ALL | INSRC(1) | OUTREG(2) | END);

        const u64 seed = 1469598103934665603ull;
        const u64 hs = rsx_fp_structural_hash(single, sizeof single, seed);
        const u64 hm = rsx_fp_structural_hash(mrt, sizeof mrt, seed);
        check_true("hash_nonzero", hs != 0 && hm != 0);
        check_true("hash_distinguishes_mrt", hs != hm);
        check_true("hash_stable",
                   hs == rsx_fp_structural_hash(single, sizeof single, seed));
    }

    /* Test 10: IFE with an else branch. The IFE's SRC1/SRC2 hold the else and
     * end addresses in words; each side must land in its own HLSL block, not
     * run one after the other (Crazy Taxi's uber shader drew the city black
     * that way). @0 IFE(cc0.x > 0) else=@32 end=@48; @16 MOV r0,COL0;
     * @32 MOV r0,COL1; @48 LIF r1,r0 END. */
    {
        u8 prog[64];
        const u32 exec_gt_x = (1u << 20);   /* exec_if gr, cond swizzle x */
        put_word(prog +  0, OPC(0x02));
        put_word(prog +  4, exec_gt_x);
        put_word(prog +  8, (1u << 31) | (32u >> 2));   /* branch, else */
        put_word(prog + 12, 48u >> 2);                  /* end */
        put_word(prog + 16, OPC(0x01) | OUTMASK_ALL | INSRC(1));
        put_word(prog + 20, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 24, 0);
        put_word(prog + 28, 0);
        put_word(prog + 32, OPC(0x01) | OUTMASK_ALL | INSRC(2));
        put_word(prog + 36, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 40, 0);
        put_word(prog + 44, 0);
        put_word(prog + 48, OPC(0x3C) | OUTMASK_ALL | OUTREG(1) | END);
        put_word(prog + 52, T_TEMP | REG(0) | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 56, 0);
        put_word(prog + 60, 0);
        rsx_fp_decompile(prog, sizeof(prog), CTRL_32BIT, hlsl, sizeof(hlsl));
        const char* ife  = strstr(hlsl, "    if (cc0.x > 0.0) {\n");
        const char* then = strstr(hlsl, "input.col0");
        const char* els  = strstr(hlsl, "    } else {\n");
        const char* alt  = strstr(hlsl, "input.col1");
        const char* lif  = strstr(hlsl, "float4(1.0, ");
        check_true("ife_order", ife && then && els && alt && lif &&
                                ife < then && then < els && els < alt && alt < lif);
        check_absent("ife_not_skipped", hlsl, "flow-control op skipped");
        check_absent("lif_handled", hlsl, "unhandled FP opcode");
    }

    /* A 4096-byte program fits the D3D12 caller's input bound, but its
     * predicated, CC-writing instructions expand beyond 32 KiB of HLSL.
     * Assemble it here instead of depending on a captured game shader. */
    {
        enum { INSTRUCTION_COUNT = 256 };
        u8 prog[INSTRUCTION_COUNT * 16];
        static char backend_hlsl[RSX_FP_HLSL_BUFFER_SIZE];
        for (int i = 0; i < INSTRUCTION_COUNT; i++) {
            u8* ins = prog + i * 16;
            u32 end = i == INSTRUCTION_COUNT - 1 ? END : 0u;
            u32 opcode = i == 0 ? OPC(0x01) : OPC(0x04);
            u32 condition = i == 0 ? EXEC_ALWAYS : (1u << 20);
            put_word(ins + 0, opcode | OUTMASK_ALL | INSRC(1) |
                             (1u << 8) | end); /* MOV/MAD r0, set CC0 */
            put_word(ins + 4, T_INPUT | SWZ_IDENT | condition);
            put_word(ins + 8, T_TEMP | REG(0) | SWZ_IDENT);
            put_word(ins + 12, T_TEMP | REG(1) | SWZ_IDENT);
        }
        int n = rsx_fp_decompile(prog, sizeof(prog), RSX_FP_CTRL_AUTO,
                                 backend_hlsl, sizeof(backend_hlsl));
        printf("-- source capacity: %d instr, %zu HLSL bytes\n",
               n, strlen(backend_hlsl));
        check_true("backend_source_capacity", n == INSTRUCTION_COUNT);
        check_true("source_exceeds_old_capacity", strlen(backend_hlsl) > 32768u);
        check("large_program_return", backend_hlsl, "return (_o == _o) ? _o");
    }

    printf("\n===========================================\n");
    printf("Results: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
