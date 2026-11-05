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
 *   VP  MOV o0, v0 ; MOV o5, v3 (END)      + user clip planes (o5.yzw, o6.yzw)
 *   FP  MOV r0, COL0 (END)                 colour out
 *   FP  MOV r0, COL0.zyxw (END)            colour out, red and blue swapped:
 *                                          what the host's --shader mode runs
 *   FP  MOV r0, -abs(COL1.zyxw) (END)      colour clamp before source modifiers
 *   FP  PK4UB r0, COL0 ; UPG r0, r0 (END)  pack to 8-bit, unpack sRGB->linear:
 *                                          what a character material does
 *   FP  TEX r0, TC0 unit 0 (END)           texture unit 0 sampled at texcoord0
 *   FP  TEX r0, TC0 unit 0, CUBE (END)     ... with unit 0 a cube map
 *   FP  TEX r0, TC0 unit 0, 3D UN (END)    ... a volume addressed in texels
 *   VP  MOV o0, v0 ; TXL o1, v8 (END)      a vertex texture sampled at t16
 *   VP  519 slots, BRI past slot 512       a long effects program's branches
 *   VP  BRB, CLB on transform branch bits  its optional paths
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
#ifndef _WIN32
#include <dirent.h>
#include <unistd.h>
#endif

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

static void check_absent(const char* what, const char* text, const char* needle)
{
    if (text && !strstr(text, needle)) {
        printf("[PASS] %s -- no \"%s\"\n", what, needle);
        g_pass++;
    } else {
        printf("[FAIL] %s -- found \"%s\"\n", what, needle);
        g_fail++;
    }
}

static void check_count(const char* what, int got, int want)
{
    if (got == want) { printf("[PASS] %s: %d instruction(s)\n", what, got); g_pass++; }
    else { printf("[FAIL] %s: decompile returned %d, expected %d\n", what, got, want); g_fail++; }
}

/* ---- the translation step, shared by every case -------------------------- */

static char g_hlsl[262144];
static char g_msl[262144];
static u8 g_long_vp[519 * 16];   /* the 519-slot program; also has no BRB/CLB */

/* Flow instruction `sca_op` (0x09 BRI, 0x11 BRB, 0x12 CLB) to VP slot
 * `target`: cond [10:12] = 7 (TR), the target split over D0[23], D2[0:5] and
 * D3[29:31]; a boolean branch tests branch bit `bit` (D3[23:27]) for
 * `if_set` (D3[28]). No ALU writes. */
static void vp_branch(u8* p, u32 sca_op, u32 target, u32 bit, u32 if_set)
{
    const u32 d0 = (0x3Fu << 15) | (7u << 10) | (((target >> 9) & 1u) << 23);
    const u32 d1 = sca_op << 27;
    const u32 d2 = (target >> 3) & 0x3Fu;
    const u32 d3 = (0x3Fu << 7) | (bit << 23) | (if_set << 28) | ((target & 7u) << 29);
    rsx_test_put_le32(p + 0, d0); rsx_test_put_le32(p + 4, d1);
    rsx_test_put_le32(p + 8, d2); rsx_test_put_le32(p + 12, d3);
}
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

/* ---- the translation cache ----------------------------------------------
 * rsx_hlsl_to_msl keeps each translation in memory and, with
 * PS3RECOMP_MSL_CACHE=<dir>, on disk: a new process (simulated by
 * rsx_hlsl_to_msl_cache_reset) is served from the file with the same text, a
 * damaged file is a miss and is rewritten, and a failed translation is not
 * kept. */
static void check_true(const char* what, int ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) g_pass++; else g_fail++;
}

static RsxMslCacheStats cache_stats(void)
{
    RsxMslCacheStats s;
    rsx_hlsl_to_msl_cache_stats(&s);
    return s;
}

#ifndef _WIN32
/* The one .msl file in dir, or "" when there is not exactly one. */
static void only_msl_file(const char* dir, char* out, size_t size)
{
    DIR* d = opendir(dir);
    int n = 0;
    out[0] = 0;
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if (len > 4 && strcmp(e->d_name + len - 4, ".msl") == 0) {
            snprintf(out, size, "%s/%s", dir, e->d_name);
            n++;
        }
    }
    closedir(d);
    if (n != 1) out[0] = 0;
}
#endif

static char g_msl2[65536];

static void test_cache(void)
{
    static const char* kPS =
        "float4 main(float4 pos : SV_Position, float4 col : COLOR0) : SV_Target0 {\n"
        "    return col.zyxw * 0.5;\n"
        "}\n";
    RsxMslCacheStats s;
    unsetenv("PS3RECOMP_MSL_CACHE");
    rsx_hlsl_to_msl_cache_reset();

    check_true("cache: first request translates",
               rsx_hlsl_to_msl(kPS, RSX_SHADER_STAGE_FRAGMENT, g_msl, sizeof g_msl, g_log, sizeof g_log) == 0);
    check_true("cache: the same request again",
               rsx_hlsl_to_msl(kPS, RSX_SHADER_STAGE_FRAGMENT, g_msl2, sizeof g_msl2, g_log, sizeof g_log) == 0 &&
               strcmp(g_msl, g_msl2) == 0);
    s = cache_stats();
    check_true("cache: ... from memory, translated once", s.translations == 1 && s.memory_hits == 1);
    {
        /* Valid as either stage: no inputs, one output. */
        static const char* kBoth = "float4 main() : SV_Target0 { return float4(1, 0, 0, 1); }\n";
        const int vs = rsx_hlsl_to_msl(kBoth, RSX_SHADER_STAGE_VERTEX, g_msl, sizeof g_msl, g_log, sizeof g_log);
        const int fs = rsx_hlsl_to_msl(kBoth, RSX_SHADER_STAGE_FRAGMENT, g_msl2, sizeof g_msl2, g_log, sizeof g_log);
        s = cache_stats();
        check_true("cache: the same text for two stages is two entries",
                   vs == 0 && fs == 0 && s.translations == 3 && s.memory_hits == 1 &&
                   strcmp(g_msl, g_msl2) != 0);
    }
    {
        char small[16];
        check_true("cache: a hit larger than the buffer fails",
                   rsx_hlsl_to_msl(kPS, RSX_SHADER_STAGE_FRAGMENT, small, sizeof small, NULL, 0) == -1);
    }
    const u32 before = cache_stats().translations;
    check_true("cache: broken HLSL fails",
               rsx_hlsl_to_msl("float4 main( {", RSX_SHADER_STAGE_FRAGMENT, g_msl2, sizeof g_msl2, g_log, sizeof g_log) != 0 &&
               rsx_hlsl_to_msl("float4 main( {", RSX_SHADER_STAGE_FRAGMENT, g_msl2, sizeof g_msl2, g_log, sizeof g_log) != 0 &&
               cache_stats().translations == before);

#ifndef _WIN32
    char dir[] = "/tmp/ps3recomp_msl_cache_XXXXXX";
    if (!mkdtemp(dir)) { check_true("cache: mkdtemp", 0); return; }
    setenv("PS3RECOMP_MSL_CACHE", dir, 1);
    rsx_hlsl_to_msl_cache_reset();
    check_true("cache: disk, first run translates",
               rsx_hlsl_to_msl(kPS, RSX_SHADER_STAGE_FRAGMENT, g_msl, sizeof g_msl, g_log, sizeof g_log) == 0);
    s = cache_stats();
    check_true("cache: ... with the translator", s.translations == 1 && s.disk_hits == 0);
    char path[1024];
    only_msl_file(dir, path, sizeof path);
    check_true("cache: ... and writes one file", path[0] != 0);

    rsx_hlsl_to_msl_cache_reset();
    check_true("cache: disk, next run",
               rsx_hlsl_to_msl(kPS, RSX_SHADER_STAGE_FRAGMENT, g_msl2, sizeof g_msl2, g_log, sizeof g_log) == 0);
    s = cache_stats();
    check_true("cache: ... from the file, no translation", s.disk_hits == 1 && s.translations == 0);
    check_true("cache: ... with the same MSL", strcmp(g_msl, g_msl2) == 0);

    /* Damage the stored key: the file no longer matches the request. */
    if (path[0]) {
        FILE* f = fopen(path, "r+b");
        if (f) { fseek(f, 16 * 4 + 10, SEEK_SET); fputc('#', f); fclose(f); }
    }
    rsx_hlsl_to_msl_cache_reset();
    check_true("cache: damaged file",
               rsx_hlsl_to_msl(kPS, RSX_SHADER_STAGE_FRAGMENT, g_msl2, sizeof g_msl2, g_log, sizeof g_log) == 0 &&
               strcmp(g_msl, g_msl2) == 0);
    s = cache_stats();
    check_true("cache: ... is a miss, translated again", s.translations == 1 && s.disk_hits == 0);
    rsx_hlsl_to_msl_cache_reset();
    rsx_hlsl_to_msl(kPS, RSX_SHADER_STAGE_FRAGMENT, g_msl2, sizeof g_msl2, g_log, sizeof g_log);
    s = cache_stats();
    check_true("cache: ... and was rewritten", s.disk_hits == 1 && s.translations == 0);

    if (path[0]) unlink(path);
    rmdir(dir);
    unsetenv("PS3RECOMP_MSL_CACHE");
    rsx_hlsl_to_msl_cache_reset();
#endif
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
            /* Invariant, so two programs computing the same position the
             * same way agree to the bit (depth-EQUAL multipass); see
             * rsx_shader_msl.cpp. PS3RECOMP_MSL_INVARIANT=0 drops it. */
            check("VP MSL", g_msl, getenv("PS3RECOMP_MSL_INVARIANT") ? "[[position" : "[[position, invariant]]");
            check("VP MSL", g_msl, "[[user(locn0)]]");    /* COLOR0 */
            check("VP MSL", g_msl, "[[user(locn10)]]");   /* TEXCOORD7 */
        }
    }

    /* ---- vertex program: branches past slot 512 ------------------------------
     * The transform program memory has 544 slots. A branching program is
     * emitted as one "if (_pc == N)" block per leader; a leader past slot 511
     * opened no block, so every later branch closed a block that was never
     * opened and main() ended early. A 543-slot effects program failed that
     * way and every draw using it was dropped. */
    {
        u8* vp = g_long_vp;
        for (u32 i = 0; i < 513; i++)
            rsx_test_vp_mov_out(vp + i * 16, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);   /* MOV o0, v0 */
        vp_branch(vp + 513 * 16, 0x09, 515, 0, 0);
        rsx_test_vp_mov_out(vp + 514 * 16, 3, RSX_TEST_VP_SWZ_IDENT, 1, 0);      /* skipped */
        rsx_test_vp_mov_out(vp + 515 * 16, 1, RSX_TEST_VP_SWZ_IDENT, 1, 0);      /* MOV o1, v1 */
        vp_branch(vp + 516 * 16, 0x09, 518, 0, 0);
        rsx_test_vp_mov_out(vp + 517 * 16, 3, RSX_TEST_VP_SWZ_IDENT, 2, 0);      /* skipped */
        rsx_test_vp_mov_out(vp + 518 * 16, 2, RSX_TEST_VP_SWZ_IDENT, 2, 1);      /* MOV o2, v2, END */
        rsx_vp_set_branch_base(0);
        if (rsx_vp_decompile(vp, sizeof g_long_vp, g_hlsl, sizeof g_hlsl) > 0) {
            check("long VP HLSL", g_hlsl, "if (_pc == 514) {");
            check("long VP HLSL", g_hlsl, "if (_pc == 515) {");
            check("long VP HLSL", g_hlsl, "if (_pc == 518) {");
            if (translate("long VP", RSX_SHADER_STAGE_VERTEX) == 0)
                check("long VP MSL", g_msl, "vertex ");
        } else {
            printf("[FAIL] long VP -- decompile failed\n");
            g_fail++;
        }
    }

    /* ---- vertex program: boolean branches --------------------------------
     * BRB and CLB branch on one bit of SET_TRANSFORM_BRANCH_BITS, not on a
     * condition code. An uber effects program skips its optional paths with
     * "BRB !bN"; run straight through, every path was taken and the glows
     * came out as red stretched polygons.
     *   0 MOV o0, v0       1 BRB !b3 -> 3     2 MOV o1, v1
     *   3 CLB b12 -> 6     4 MOV o2, v2       5 BRI -> 8
     *   6 MOV o3, v3       7 RET              8 MOV o4, v4 (END) */
    {
        static u8 vp[9 * 16];
        rsx_test_vp_mov_out(vp + 0 * 16, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
        vp_branch(vp + 1 * 16, 0x11, 3, 3, 0);
        rsx_test_vp_mov_out(vp + 2 * 16, 1, RSX_TEST_VP_SWZ_IDENT, 1, 0);
        vp_branch(vp + 3 * 16, 0x12, 6, 12, 1);
        rsx_test_vp_mov_out(vp + 4 * 16, 2, RSX_TEST_VP_SWZ_IDENT, 2, 0);
        vp_branch(vp + 5 * 16, 0x09, 8, 0, 0);
        rsx_test_vp_mov_out(vp + 6 * 16, 3, RSX_TEST_VP_SWZ_IDENT, 3, 0);
        vp_branch(vp + 7 * 16, 0x0C, 0, 0, 0);
        rsx_test_vp_mov_out(vp + 8 * 16, 4, RSX_TEST_VP_SWZ_IDENT, 4, 1);
        if (rsx_vp_uses_branch_bits(vp, sizeof vp) &&
            !rsx_vp_uses_branch_bits(g_long_vp, sizeof g_long_vp)) {
            printf("[PASS] bool VP -- keyed on the branch bits, the BRI-only program not\n");
            g_pass++;
        } else {
            printf("[FAIL] bool VP -- rsx_vp_uses_branch_bits\n");
            g_fail++;
        }
        static const struct { u32 bits; const char* brb; const char* clb; } cases[] = {
            { 0x0000u, "BRB @1 -> 3 cond=7 test=0 swz=xxxx */\n        _pc = 3;",
                       "CLB @3 -> 6 cond=7 test=0 swz=xxxx */\n        _pc = 4;" },
            { 0x1008u, "BRB @1 -> 3 cond=7 test=0 swz=xxxx */\n        _pc = 2;",
                       "CLB @3 -> 6 cond=7 test=0 swz=xxxx */\n"
                       "        _cs[_csp] = 4; _csp = min(_csp + 1, 7); _pc = 6;" },
        };
        for (u32 i = 0; i < 2; i++) {
            rsx_vp_set_branch_base(0);
            rsx_vp_set_branch_bits(cases[i].bits);
            if (rsx_vp_decompile(vp, sizeof vp, g_hlsl, sizeof g_hlsl) > 0) {
                check("bool VP HLSL", g_hlsl, cases[i].brb);
                check("bool VP HLSL", g_hlsl, cases[i].clb);
                check_absent("bool VP HLSL", g_hlsl, "not modelled");
                if (translate("bool VP", RSX_SHADER_STAGE_VERTEX) == 0)
                    check("bool VP MSL", g_msl, "vertex ");
            } else {
                printf("[FAIL] bool VP -- decompile failed\n");
                g_fail++;
            }
        }
        rsx_vp_set_branch_bits(0);
    }

    /* ---- user clip planes: MOV o0, v0 ; MOV o5, v3 ------------------------
     * The distances live in o5.yzw / o6.yzw and the planes are register
     * state (NV4097 0x1478), so they are added to the decompiled text:
     * plane 0 ENABLE_GE keeps d >= 0, plane 1 ENABLE_LT keeps d <= 0, the
     * rest of the vector never clips, and the varyings keep their slots. */
    {
        u8 vp[32];
        rsx_test_vp_mov_out(vp +  0, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);   /* MOV o0, v0 */
        rsx_test_vp_mov_out(vp + 16, 3, RSX_TEST_VP_SWZ_IDENT, 5, 1);   /* MOV o5, v3, END */
        check_count("clip VP", rsx_vp_decompile(vp, sizeof vp, g_hlsl, sizeof g_hlsl), 2);
        const int none = rsx_vp_apply_user_clip(g_hlsl, sizeof g_hlsl, 0);
        const int small = rsx_vp_apply_user_clip(g_hlsl, strlen(g_hlsl) + 8, 2u);
        const int two = rsx_vp_apply_user_clip(g_hlsl, sizeof g_hlsl, 2u | (1u << 2));
        if (none == 0 && small == -1 && two == 0) { printf("[PASS] clip VP patch results\n"); g_pass++; }
        else { printf("[FAIL] clip VP patch results %d %d %d (want 0 -1 0)\n", none, small, two); g_fail++; }
        check("clip VP HLSL", g_hlsl, "float4 clipd0:SV_ClipDistance0;");
        check("clip VP HLSL", g_hlsl, "Out.clipd0 = float4(o[5].y, -(o[5].z), 0.5, 0.5);");
        check_absent("clip VP HLSL", g_hlsl, "SV_ClipDistance1");
        if (translate("clip VP", RSX_SHADER_STAGE_VERTEX) == 0) {
            check("clip VP MSL", g_msl, "[[clip_distance]]");
            check("clip VP MSL", g_msl, "[[user(locn0)]]");    /* COLOR0 */
            check("clip VP MSL", g_msl, "[[user(locn10)]]");   /* TEXCOORD7 */
        }
        /* Plane 5 (o6.w) adds the second vector. */
        check_count("clip VP 6", rsx_vp_decompile(vp, sizeof vp, g_hlsl, sizeof g_hlsl), 2);
        if (rsx_vp_apply_user_clip(g_hlsl, sizeof g_hlsl, 2u << 10) == 0) { printf("[PASS] clip VP 6 patch\n"); g_pass++; }
        else { printf("[FAIL] clip VP 6 patch\n"); g_fail++; }
        check("clip VP 6 HLSL", g_hlsl, "float2 clipd1:SV_ClipDistance1;");
        check("clip VP 6 HLSL", g_hlsl, "Out.clipd1 = float2(0.5, o[6].w);");
        if (translate("clip VP 6", RSX_SHADER_STAGE_VERTEX) == 0)
            check("clip VP 6 MSL", g_msl, "[[clip_distance]]");

    }

    /* ---- fragment program: MOV r0, COL0 ---------------------------------- */
    {
        u8 fp[16];
        rsx_test_fp_mov_r0_col0(fp, RSX_TEST_FP_SWZ_IDENT);
        u32 nconst = 0;
        check_count("FP", rsx_fp_decompile_buffered_ex(fp, sizeof fp, 0x40u /* r0 exports */, 0,
                                                       g_hlsl, sizeof g_hlsl, &nconst), 1);
        check("FP HLSL", g_hlsl, "(saturate(input.col0)).xyzw");
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
        check("swizzled FP HLSL", g_hlsl, "(saturate(input.col0)).zyxw");
        check("swizzled FP HLSL", g_hlsl, "r[0].xyzw = _v.xyzw;");
        if (translate("swizzled FP", RSX_SHADER_STAGE_FRAGMENT) == 0)
            check("swizzled FP MSL", g_msl, "fragment ");
        /* No pack/unpack opcode, so none of the helpers: the text of a program
         * that does not use them is the same as before they existed. */
        check_absent("swizzled FP HLSL", g_hlsl, "_rsx_pk4ub");
    }

    /* ---- fragment program: PK4UB r0, COL0 ; UPG r0, r0 ---------------------
     * The pack/unpack pair a character material uses for its sRGB<->linear
     * conversion. The packed word is carried in r0's bits (asfloat), and the
     * unpack reads them back (asuint): the two must round-trip through
     * glslang and spirv-cross, which is where a bitcast can go wrong. */
    {
        u8 fp[32];
        rsx_test_fp_instr(fp,
                          RSX_TEST_FP_OPC(0x27 /* PK4UB */) | RSX_TEST_FP_MASK_XYZW |
                          RSX_TEST_FP_INSRC_COL0 | RSX_TEST_FP_OUTREG(0),
                          RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
        rsx_test_fp_instr(fp + 16,
                          RSX_TEST_FP_OPC(0x2D /* UPG */) | RSX_TEST_FP_MASK_XYZW |
                          RSX_TEST_FP_OUTREG(0) | RSX_TEST_FP_END,
                          /* src0 reg_type 0 (temp), reg 0 */ RSX_TEST_FP_SWZ_IDENT);
        check_count("pack FP", rsx_fp_decompile_buffered_ex(fp, sizeof fp, 0x40u, 0,
                                                            g_hlsl, sizeof g_hlsl, NULL), 2);
        check("pack FP HLSL", g_hlsl, "asfloat(_rsx_pk4ub((saturate(input.col0)).xyzw))");
        check("pack FP HLSL", g_hlsl, "_rsx_srgb2lin(_rsx_up4ub(((r[0]).xyzw).x))");
        check("pack FP HLSL", g_hlsl, "uint   _rsx_pk4ub(float4 v)");
        check("pack FP HLSL", g_hlsl, "float4 _rsx_srgb2lin(float4 cs)");
        if (translate("pack FP", RSX_SHADER_STAGE_FRAGMENT) == 0) {
            check("pack FP MSL", g_msl, "fragment ");
            check("pack FP MSL", g_msl, "[[color(0)]]");
        }
    }

    /* ---- fragment program: MOV r0, -abs(COL1.zyxw) ------------------------
     * Clamp the interpolated colour before abs/negate: clamping a negated
     * colour instead would discard the negative result. DWORD1 bits 29 and
     * 17 are source abs and negate, respectively. */
    {
        u8 fp[16];
        rsx_test_fp_instr(fp,
                          RSX_TEST_FP_OPC(RSX_TEST_FP_OP_MOV) | RSX_TEST_FP_MASK_XYZW |
                          RSX_TEST_FP_INSRC(2) /* COL1 */ | RSX_TEST_FP_END,
                          RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ(2, 1, 0, 3) |
                          (1u << 29) /* abs */ | (1u << 17) /* negate */);
        check_count("modified COL1 FP",
                    rsx_fp_decompile_buffered_ex(fp, sizeof fp, 0x40u, 0,
                                                 g_hlsl, sizeof g_hlsl, NULL), 1);
        check("modified COL1 FP HLSL", g_hlsl,
              "-(abs((saturate(input.col1)).zyxw))");
        if (translate("modified COL1 FP", RSX_SHADER_STAGE_FRAGMENT) == 0) {
            check("modified COL1 FP MSL", g_msl, "fragment ");
            check("modified COL1 FP MSL", g_msl, "[[user(locn1)]]");
            check("modified COL1 FP MSL", g_msl, "[[color(0)]]");
        }
    }

    /* ---- fragment program: MOV r0, FACING ---------------------------------
     * Input 0xE is the face flag, +1 front / -1 back. Two-sided materials
     * multiply the normal by it; it used to read as zero. */
    {
        u8 fp[16];
        rsx_test_fp_instr(fp,
                          RSX_TEST_FP_OPC(RSX_TEST_FP_OP_MOV) | RSX_TEST_FP_MASK_XYZW |
                          RSX_TEST_FP_INSRC(0xE) | RSX_TEST_FP_END,
                          RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
        check_count("FACING FP",
                    rsx_fp_decompile_buffered_ex(fp, sizeof fp, 0x40u, 0,
                                                 g_hlsl, sizeof g_hlsl, NULL), 1);
        check("FACING FP HLSL", g_hlsl, "bool _rsx_front : SV_IsFrontFace;");
        check("FACING FP HLSL", g_hlsl, "input._rsx_front ? float4(1,1,1,1) : float4(-1,-1,-1,-1)");
        if (translate("FACING FP", RSX_SHADER_STAGE_FRAGMENT) == 0)
            check("FACING FP MSL", g_msl, "[[front_facing]]");
    }

    /* ---- fragment program: MOV r0, TC8 / TC9 ------------------------------
     * They follow TEXCOORD7 (locn10), and the VP writes them from o15 / o6. */
    for (u32 n = 8; n <= 9; n++) {
        u8 fp[16];
        rsx_test_fp_instr(fp,
                          RSX_TEST_FP_OPC(RSX_TEST_FP_OP_MOV) | RSX_TEST_FP_MASK_XYZW |
                          RSX_TEST_FP_INSRC(0xC + (n - 8)) | RSX_TEST_FP_END,
                          RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
        check_count("TC8/9 FP",
                    rsx_fp_decompile_buffered_ex(fp, sizeof fp, 0x40u, 0,
                                                 g_hlsl, sizeof g_hlsl, NULL), 1);
        check("TC8/9 FP HLSL", g_hlsl, n == 8 ? "(input.tc8).xyzw" : "(input.tc9).xyzw");
        check("TC8/9 FP HLSL", g_hlsl, "float4 tc8:TEXCOORD8; float4 tc9:TEXCOORD9;");
        if (translate("TC8/9 FP", RSX_SHADER_STAGE_FRAGMENT) == 0)
            check("TC8/9 FP MSL", g_msl, n == 8 ? "[[user(locn11)]]" : "[[user(locn12)]]");
    }
    {
        u8 vp[16];
        rsx_test_vp_mov_out(vp, 0, RSX_TEST_VP_SWZ_IDENT, 15, 1);   /* MOV o15, v0, END */
        rsx_vp_decompile(vp, sizeof vp, g_hlsl, sizeof g_hlsl);
        check("TC8 VP HLSL", g_hlsl, "Out.t8=o[15]; Out.t9=o[6];");
        if (translate("TC8 VP", RSX_SHADER_STAGE_VERTEX) == 0)
            check("TC8 VP MSL", g_msl, "[[user(locn11)]]");
    }

    /* ---- fragment program: TEX r0, TC0 (unit 0) --------------------------- */
    {
        u8 fp[16];
        rsx_test_fp_tex_r0_tc0(fp, 0);
        check_count("TEX FP", rsx_fp_decompile_buffered_ex(fp, sizeof fp, 0x40u, 0,
                                                           g_hlsl, sizeof g_hlsl, NULL), 1);
        check("TEX FP HLSL", g_hlsl, "rsx_tex[0].Sample(rsx_samp[0]");
        /* Colour input clamping must not affect texture coordinates. */
        check("TEX FP HLSL", g_hlsl, "(input.tc0).xyzw");
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

    /* ---- fragment program: TEX r0, TC0 on a 3D unit ------------------------
     * A volume texture (SET_TEXTURE_FORMAT dimension 3): Texture3D in HLSL,
     * texture3d in MSL, sampled with three coordinates -- inFAMOUS 2's
     * occlusion volumes look their 8x8x8 cone LUT up this way. */
    {
        u8 fp[16];
        rsx_test_fp_tex_r0_tc0(fp, 0);
        check_count("3D TEX FP",
                    rsx_fp_decompile_buffered_shadow_color_expand_vol_ex(fp, sizeof fp, 0x40u,
                        0, 1u /* unit 0 is 3D */, NULL, NULL, NULL, g_hlsl, sizeof g_hlsl, NULL), 1);
        check("3D TEX FP HLSL", g_hlsl, "Texture3D   rsx_tex0 : register(t0);");
        check("3D TEX FP HLSL", g_hlsl, "rsx_tex0.Sample(rsx_samp[0]");
        if (translate("3D TEX FP", RSX_SHADER_STAGE_FRAGMENT) == 0)
            check("3D TEX FP MSL", g_msl, "texture3d");
    }

    /* ---- the same, with unnormalised (texel) coordinates ------------------
     * All three coordinates are texels and are divided by width, height and
     * depth. A float2 scale on the float3 coordinate does not compile, and
     * the pipeline was dropped: Resistance 3's 32x32x32 volume lookups. */
    {
        u8 fp[16];
        rsx_test_fp_tex_r0_tc0(fp, 0);
        rsx_fp_decompile_buffered_shadow_color_expand_vol_ex(fp, sizeof fp, 0x40u, 0, 1u,
            NULL, NULL, NULL, g_hlsl, sizeof g_hlsl, NULL);
        const u32 dim[16][3] = { { 8, 4, 2 } };
        check_count("3D UN TEX FP scale sites",
                    rsx_fp_apply_unnorm_scale(g_hlsl, sizeof g_hlsl, 1u, dim, 0, 1u), 1);
        check("3D UN TEX FP HLSL", g_hlsl, ".xyz * float3(0.125, 0.25, 0.5))");
        if (translate("3D UN TEX FP", RSX_SHADER_STAGE_FRAGMENT) == 0)
            check("3D UN TEX FP MSL", g_msl, "texture3d");
    }

    /* ---- fragment program: TEX r0, TC0 with EXP_TEX on a biased unit -----
     * The _bx2 fetch: the lanes the unit's biased remap names come back as
     * x*2-1. The helper must survive glslang/spirv-cross intact. */
    {
        u8 fp[16];
        u8 expand[16] = {0};
        expand[0] = 0x7;
        rsx_test_fp_instr(fp, RSX_TEST_FP_OPC(RSX_TEST_FP_OP_TEX) | RSX_TEST_FP_MASK_XYZW |
                              RSX_TEST_FP_INSRC_TC0 | RSX_TEST_FP_TEXU(0) | (1u << 21) | RSX_TEST_FP_END,
                              RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
        check_count("expand TEX FP", rsx_fp_decompile_buffered_shadow_color_expand_ex(fp, sizeof fp, 0x40u, 0,
                        NULL, NULL, expand, g_hlsl, sizeof g_hlsl, NULL), 1);
        check("expand TEX FP HLSL", g_hlsl, "_rsx_expand_input(rsx_tex[0].Sample(rsx_samp[0], ((input.tc0).xyzw).xy), 15u, true)");
        if (translate("expand TEX FP", RSX_SHADER_STAGE_FRAGMENT) == 0)
            check("expand TEX FP MSL", g_msl, "* 2.0");
    }

    /* ---- fragment program: TEX r0, TC0 with perspective_corr --------------
     * SRC2 bit 31 scales the texcoord by 1/w. Through glslang SV_POSITION.w
     * becomes FragCoord.w, which is already 1/w, so the helper must resolve
     * to the bare .w here (RSX_FRAGCOORD_W_IS_RCP) -- a division would undo
     * the correction and inFAMOUS 2's river would sample outside its copy. */
    {
        u8 fp[16];
        rsx_test_fp_tex_r0_tc0(fp, 0);
        rsx_test_fp_put_word(fp + 12, 1u << 31);
        check_count("persp TEX FP", rsx_fp_decompile_buffered_ex(fp, sizeof fp, 0x40u, 0,
                                                                 g_hlsl, sizeof g_hlsl, NULL), 1);
        check("persp TEX FP HLSL", g_hlsl, "(input.tc0 * _rsx_inv_w(input.position))");
        if (translate("persp TEX FP", RSX_SHADER_STAGE_FRAGMENT) == 0) {
            check("persp TEX FP MSL", g_msl, "gl_FragCoord.w");
            check_absent("persp TEX FP MSL", g_msl, "1.0 / gl_FragCoord.w");
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
        check("MRT FP HLSL", g_hlsl, "(saturate(input.col0)).zyxw");

        if (translate("MRT FP", RSX_SHADER_STAGE_FRAGMENT) == 0) {
            check("MRT FP MSL", g_msl, "fragment ");
            check("MRT FP MSL", g_msl, "[[color(0)]]");
            check("MRT FP MSL", g_msl, "[[color(1)]]");
        }
    }

    /* ---- max/min/clamp/normalize come out as precise:: math ------------- *
     * spirv-cross writes them as fast::, whose NaN results Metal leaves
     * undefined; D3D's max/min return the number when one side is NaN, and
     * the guest programs rely on it. rsx_hlsl_to_msl respells them precise::
     * unless PS3RECOMP_MSL_FAST is set. */
    if (!getenv("PS3RECOMP_MSL_FAST")) {
        snprintf(g_hlsl, sizeof g_hlsl,
                 "float4 main(float4 c : COLOR0) : SV_TARGET0 {\n"
                 "    float3 n = normalize(c.xyz);\n"
                 "    return float4(max(n, c.www), min(c.w, 0.5)) * clamp(c.x, 0.1, 0.9);\n"
                 "}\n");
        if (translate("precise math FP", RSX_SHADER_STAGE_FRAGMENT) == 0) {
            check("precise math FP MSL", g_msl, "precise::max");
            check("precise math FP MSL", g_msl, "precise::normalize");
            check_absent("precise math FP MSL", g_msl, "fast::");
        }
    }

    test_cache();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
