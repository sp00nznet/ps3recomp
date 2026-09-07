/*
 * ps3recomp - guest shader -> MSL translation test
 *
 * Hand-assembles the smallest programs the Metal backend has to run (the
 * encoders live in rsx_test_programs.h, shared with the host harness),
 * pushes each through its decompiler, then through rsx_hlsl_to_msl, and
 * checks the MSL carries the bindings the backend depends on. Needs no GPU,
 * so it runs on Linux CI too -- which is the point: glslang's HLSL front end
 * and spirv-cross are checked against the decompilers' actual output on every
 * push, not only on the Mac that happens to have Metal.
 *
 * Programs:
 *   VP  MOV o0, v0 ; MOV o1, v3 (END)      position through, colour through
 *   FP  MOV r0, COL0 (END)                 colour out
 *   FP  MOV r0, COL0.zyxw (END)            colour out, red and blue swapped:
 *                                          what the host's --shader mode runs
 *   FP  TEX r0, TC0 unit 0 (END)           texture unit 0 sampled at texcoord0
 *   FP  TEX r0, TC0 unit 0, CUBE (END)     ... with unit 0 a cube map
 *   VP  MOV o0, v0 ; TXL o1, v8 (END)      a vertex texture sampled at t16
 *   FP  MOV r0, COL0 ; MOV r2, COL0.zyxw   two colour targets: what --mrt runs
 *
 * Each program's HLSL is checked for what was meant before the MSL is looked
 * at, so a wrong bit in an encoder shows up as a decompiler-level failure
 * rather than a mysterious MSL one.
 *
 * Built as a CMake target (test_shader_msl) because it links glslang and
 * spirv-cross. -v prints the HLSL and MSL of every program.
 */
#include "../rsx_vp_decompiler.h"
#include "../rsx_fp_decompiler.h"
#include "../rsx_shader_msl.h"
#include "../rsx_test_programs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0, g_fail = 0, g_verbose = 0;

static void check(const char* what, const char* text, const char* needle)
{
    if (text && strstr(text, needle)) {
        printf("[PASS] %s -- found \"%s\"\n", what, needle);
        g_pass++;
    } else {
        printf("[FAIL] %s -- missing \"%s\"\n", what, needle);
        if (!g_verbose && text) printf("----\n%s\n----\n", text);
        g_fail++;
    }
}

static void check_count(const char* what, int got, int want)
{
    if (got == want) { printf("[PASS] %s: %d instruction(s)\n", what, got); g_pass++; }
    else { printf("[FAIL] %s: decompile returned %d, expected %d\n", what, got, want); g_fail++; }
}

/* ---- the translation step, shared by every case -------------------------- */

static char g_hlsl[65536];
static char g_msl[65536];
static char g_log[4096];

static int translate(const char* what, int stage)
{
    if (g_verbose) printf("---- %s HLSL ----\n%s\n", what, g_hlsl);
    int rc = rsx_hlsl_to_msl(g_hlsl, stage, g_msl, sizeof g_msl, g_log, sizeof g_log);
    if (rc != 0) {
        printf("[FAIL] %s -- translation failed: %s\n", what, g_log);
        if (!g_verbose) printf("---- HLSL ----\n%s\n", g_hlsl);
        g_fail++;
        return -1;
    }
    printf("[PASS] %s -- translated (%u bytes of MSL)\n", what, (unsigned)strlen(g_msl));
    g_pass++;
    if (g_verbose) printf("---- %s MSL ----\n%s\n", what, g_msl);
    return 0;
}

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "-v") == 0) g_verbose = 1;

    if (!rsx_hlsl_to_msl_available()) {
        printf("HLSL to MSL translation not built; nothing to test\n");
        return 2;
    }

    /* ---- vertex program: MOV o0, v0 ; MOV o1, v3 ------------------------- */
    {
        u8 vp[32];
        rsx_test_vp_mov_out(vp +  0, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);   /* MOV o0, v0 */
        rsx_test_vp_mov_out(vp + 16, 3, RSX_TEST_VP_SWZ_IDENT, 1, 1);   /* MOV o1, v3, END */

        check_count("VP", rsx_vp_decompile(vp, sizeof vp, g_hlsl, sizeof g_hlsl), 2);
        /* o0 takes v0 and o1 takes v3, unswizzled. */
        check("VP HLSL", g_hlsl, "(v[0]).xyzw");
        check("VP HLSL", g_hlsl, "o[0].xyzw = _v.xyzw;");
        check("VP HLSL", g_hlsl, "(v[3]).xyzw");
        check("VP HLSL", g_hlsl, "o[1].xyzw = _v.xyzw;");

        if (translate("VP", RSX_SHADER_STAGE_VERTEX) == 0) {
            check("VP MSL", g_msl, "vertex ");
            check("VP MSL", g_msl, "main0");
            /* The legalization pass drops inputs the program never reads,
             * so only v0 and v3 survive -- and v3 must still be attribute 3,
             * not renumbered to 1: the backend's vertex descriptor puts RSX
             * attribute i at offset i*16, used or not. */
            check("VP MSL", g_msl, "[[attribute(0)]]");
            check("VP MSL", g_msl, "[[attribute(3)]]");
            check("VP MSL", g_msl, "[[buffer(0)]]");      /* VPConst */
            check("VP MSL", g_msl, "[[position]]");
            check("VP MSL", g_msl, "[[user(locn0)]]");    /* COLOR0 */
            check("VP MSL", g_msl, "[[user(locn10)]]");   /* TEXCOORD7 */
        }
    }

    /* ---- fragment program: MOV r0, COL0 ---------------------------------- */
    {
        u8 fp[16];
        rsx_test_fp_mov_r0_col0(fp, RSX_TEST_FP_SWZ_IDENT);
        u32 nconst = 0;
        check_count("FP", rsx_fp_decompile_buffered_ex(fp, sizeof fp, 0x40u /* r0 exports */, 0,
                                                       g_hlsl, sizeof g_hlsl, &nconst), 1);
        check("FP HLSL", g_hlsl, "(input.col0).xyzw");
        check("FP HLSL", g_hlsl, "r[0].xyzw = _v.xyzw;");
        check("FP HLSL", g_hlsl, "cbuffer PSConstants : register(b1)");
        /* The alpha test patch must survive translation as well. */
        if (rsx_fp_apply_alpha_test_buffered(g_hlsl, sizeof g_hlsl, 0x204u /* GREATER */) < 0) {
            printf("[FAIL] FP alpha test patch\n"); g_fail++;
        } else { printf("[PASS] FP alpha test patch\n"); g_pass++; }
        check("FP HLSL", g_hlsl, "fp_alpha.x");

        if (translate("FP", RSX_SHADER_STAGE_FRAGMENT) == 0) {
            check("FP MSL", g_msl, "fragment ");
            check("FP MSL", g_msl, "main0");
            check("FP MSL", g_msl, "[[buffer(1)]]");      /* PSConstants */
            check("FP MSL", g_msl, "[[color(0)]]");
            check("FP MSL", g_msl, "[[user(locn0)]]");    /* COLOR0 from the VP */
            check("FP MSL", g_msl, "discard_fragment");
        }
    }

    /* ---- fragment program: MOV r0, COL0.zyxw (the host's --shader FP) ---- */
    {
        u8 fp[16];
        rsx_test_fp_mov_r0_col0(fp, RSX_TEST_FP_SWZ(2, 1, 0, 3));
        check_count("swizzled FP", rsx_fp_decompile_buffered_ex(fp, sizeof fp, 0x40u, 0,
                                                                g_hlsl, sizeof g_hlsl, NULL), 1);
        check("swizzled FP HLSL", g_hlsl, "(input.col0).zyxw");
        check("swizzled FP HLSL", g_hlsl, "r[0].xyzw = _v.xyzw;");
        if (translate("swizzled FP", RSX_SHADER_STAGE_FRAGMENT) == 0)
            check("swizzled FP MSL", g_msl, "fragment ");
    }

    /* ---- fragment program: TEX r0, TC0 (unit 0) --------------------------- */
    {
        u8 fp[16];
        rsx_test_fp_tex_r0_tc0(fp, 0);
        check_count("TEX FP", rsx_fp_decompile_buffered_ex(fp, sizeof fp, 0x40u, 0,
                                                           g_hlsl, sizeof g_hlsl, NULL), 1);
        check("TEX FP HLSL", g_hlsl, "rsx_tex[0].Sample(rsx_samp[0]");
        check("TEX FP HLSL", g_hlsl, "r[0].xyzw = _v.xyzw;");

        if (translate("TEX FP", RSX_SHADER_STAGE_FRAGMENT) == 0) {
            check("TEX FP MSL", g_msl, "[[texture(0)]]");
            check("TEX FP MSL", g_msl, "[[sampler(0)]]");
            check("TEX FP MSL", g_msl, "[[user(locn3)]]");    /* TEXCOORD0 */
        }
    }

    /* ---- vertex program sampling a VERTEX TEXTURE ------------------------
     * MOV o0, v0 ; TXL o1, v8 on vertex-texture unit 0 (END). A transform
     * program that samples is how a title places instances or displaces a
     * mesh, and the point of the case is the binding slot: the decompiler
     * declares the unit at HLSL register t16, and the backend binds it with
     * setVertexTexture at index 16, so what has to hold is that spirv-cross
     * carries that register number into the vertex function rather than
     * renumbering it from zero. */
    {
        u8 vp[32];
        rsx_test_vp_mov_out(vp +  0, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
        rsx_test_vp_txl_out(vp + 16, 8, RSX_TEST_VP_SWZ_IDENT, 0, 1, 1);

        check_count("vtex VP",
                    rsx_vp_decompile_ex(vp, sizeof vp, 1u /* unit 0 bound */,
                                        g_hlsl, sizeof g_hlsl), 2);
        check("vtex VP HLSL", g_hlsl, "Texture2D rsx_vtex0 : register(t16);");
        check("vtex VP HLSL", g_hlsl, "rsx_vtex0.SampleLevel(rsx_vsamp0,");

        if (translate("vtex VP", RSX_SHADER_STAGE_VERTEX) == 0) {
            check("vtex VP MSL", g_msl, "[[texture(16)]]");
            /* The sampler does NOT follow the texture's number: the
             * decompiler declares rsx_vsampN at register(sN), so unit 0's
             * sampler is [[sampler(0)]] while its texture is [[texture(16)]].
             * The backend binds them at those two different indices. */
            check("vtex VP MSL", g_msl, "[[sampler(0)]]");
        }

        /* Unmasked, the same program keeps its defined-zero fallback and
         * declares no texture at all -- which is why the mask has to be part
         * of the vertex program's cache key. */
        check_count("vtex VP unmasked",
                    rsx_vp_decompile(vp, sizeof vp, g_hlsl, sizeof g_hlsl), 2);
        if (strstr(g_hlsl, "rsx_vtex0")) {
            printf("[FAIL] vtex VP unmasked -- declared rsx_vtex0 with no unit bound\n");
            g_fail++;
        } else {
            printf("[PASS] vtex VP unmasked -- no texture declared\n");
            g_pass++;
        }
    }

    /* ---- fragment program: TEX r0, TC0 on a CUBE unit --------------------
     * The same program, decompiled with unit 0 marked as a cube map. The
     * declaration and the sample both have to change: a cube is looked up
     * with a 3-component direction, not a 2D coordinate, and the backend
     * binds an MTLTextureTypeCube to that slot -- Metal rejects a 2D texture
     * bound where the shader declared a texturecube, so a mask that did not
     * survive translation would be a validation failure rather than a wrong
     * pixel. */
    {
        u8 fp[16];
        rsx_test_fp_tex_r0_tc0(fp, 0);
        check_count("cube TEX FP",
                    rsx_fp_decompile_buffered_ex(fp, sizeof fp, 0x40u,
                                                 1u /* unit 0 is a cube */,
                                                 g_hlsl, sizeof g_hlsl, NULL), 1);
        check("cube TEX FP HLSL", g_hlsl, "TextureCube rsx_tex0 : register(t0);");
        check("cube TEX FP HLSL", g_hlsl, "rsx_tex0.Sample(rsx_samp[0]");

        if (translate("cube TEX FP", RSX_SHADER_STAGE_FRAGMENT) == 0) {
            check("cube TEX FP MSL", g_msl, "texturecube");
            check("cube TEX FP MSL", g_msl, "[[texture(0)]]");
            check("cube TEX FP MSL", g_msl, "[[sampler(0)]]");
        }
    }

    /* ---- fragment program writing TWO colour targets ---------------------
     * MOV r0, COL0 ; MOV r2, COL0.zyxw (END). With 32-bit exports r0 is
     * target A and r2 is target B, so the decompiler returns a struct of two
     * SV_TARGETs rather than one value. What has to hold on the way out is
     * that spirv-cross keeps them as [[color(0)]] and [[color(1)]] in that
     * order: the backend attaches the guest's MRT set in the set's own order,
     * and a pair that came back renumbered or collapsed would write the two
     * planes to the wrong surfaces with nothing to say so. This is the
     * program the host's --mrt mode draws with. */
    {
        u8 fp[32];
        rsx_test_fp_mrt_col0(fp, RSX_TEST_FP_SWZ_IDENT, RSX_TEST_FP_SWZ(2, 1, 0, 3));
        check_count("MRT FP", rsx_fp_decompile_buffered_ex(fp, sizeof fp, 0x40u, 0,
                                                           g_hlsl, sizeof g_hlsl, NULL), 2);
        check("MRT FP HLSL", g_hlsl, "float4 t0 : SV_TARGET0;");
        check("MRT FP HLSL", g_hlsl, "float4 t1 : SV_TARGET1;");
        check("MRT FP HLSL", g_hlsl, "PSOutput main(PSInput input) {");
        check("MRT FP HLSL", g_hlsl, "r[0].xyzw = _v.xyzw;");
        check("MRT FP HLSL", g_hlsl, "r[2].xyzw = _v.xyzw;");
        check("MRT FP HLSL", g_hlsl, "(input.col0).zyxw");

        if (translate("MRT FP", RSX_SHADER_STAGE_FRAGMENT) == 0) {
            check("MRT FP MSL", g_msl, "fragment ");
            check("MRT FP MSL", g_msl, "[[color(0)]]");
            check("MRT FP MSL", g_msl, "[[color(1)]]");
        }
    }

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
