#include "../rsx_folklore_composite.h"

#include <stdio.h>
#include <string.h>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
        }                                                                    \
    } while (0)

/* The decompiler's spelling of the composite's offset arithmetic and taps
 * (synthetic text, no title program). */
static const char* const k_program =
    "float4 main(PSInput input) : SV_TARGET {\n"
    "    { float4 _v = (float4)(rsx_tex[1].Sample(rsx_samp[1], ((input.tc0).xyzw).xy)); r[0].xyw = _v.xyw; }\n"
    "    { float4 _v = (float4)(max(((r[0]).xyzw), ((fp_constants[7]).xxxx))); r[0].x = _v.x; }\n"
    "    { float4 _v = (float4)(((r[0]).xyzw) * ((fp_constants[8]).xxxx)); r[1].x = _v.x; }\n"
    "    { float4 _v = (float4)(((r[0]).xxxx) * ((fp_constants[9]).yyyy)); r[2].w = _v.w; }\n"
    "    { float4 _v = (float4)(rsx_tex[0].Sample(rsx_samp[0], ((r[1]).zwzz).xy)); h[0].xyz = _v.xyz; }\n"
    "    { float4 _v = (float4)(rsx_tex[0].Sample(rsx_samp[0], ((r[0]).zwzz).xy)); h[1].xyz = _v.xyz; }\n"
    "}\n";

static void composite_constants(float c[11][4])
{
    memset(c, 0, 11 * 4 * sizeof(float));
    c[7][0] = 0.3f;
    c[8][0] = 0.00078125f;          /* 1/1280 */
    c[9][1] = 0.00138888892f;       /* 1/720 as the title stores it */
    c[6][3] = 0.85f;
}

int main(void)
{
    CHECK(rsx_folklore_composite_program(k_program));
    CHECK(!rsx_folklore_composite_program(NULL));
    {   /* any missing piece of the shape rejects the program */
        static const char* const pieces[] = {
            "fp_constants[7]", "fp_constants[8]", "fp_constants[9]",
            "rsx_tex[1].Sample", "((r[1]).zwzz)", "((r[0]).zwzz)" };
        for (unsigned i = 0; i < sizeof pieces / sizeof *pieces; ++i) {
            char text[2048];
            strcpy(text, k_program);
            char* at = strstr(text, pieces[i]);
            CHECK(at != NULL);
            if (at) at[strlen(pieces[i]) - 2] ^= 1;   /* alter one character */
            CHECK(!rsx_folklore_composite_program(text));
        }
    }

    float c[11][4];
    composite_constants(c);
    CHECK(rsx_folklore_composite_sharpen(c, sizeof c, 2.0f));
    CHECK(fabsf(c[7][0] - 0.15f) < 1e-7f);
    CHECK(c[8][0] == 0.00078125f && c[9][1] == 0.00138888892f && c[6][3] == 0.85f);
    /* applied once: the sharpened floor no longer matches */
    CHECK(!rsx_folklore_composite_sharpen(c, sizeof c, 2.0f));
    CHECK(fabsf(c[7][0] - 0.15f) < 1e-7f);

    composite_constants(c);
    CHECK(rsx_folklore_composite_sharpen(c, sizeof c, 3.0f));
    CHECK(fabsf(c[7][0] - 0.1f) < 1e-7f);

    /* no change at scale 1, for invalid scales, or other constants */
    const float bad_scales[] = { 1.0f, 0.5f, 0.0f, -2.0f, 9.0f, NAN, INFINITY };
    for (unsigned i = 0; i < sizeof bad_scales / sizeof *bad_scales; ++i) {
        composite_constants(c);
        CHECK(!rsx_folklore_composite_sharpen(c, sizeof c, bad_scales[i]));
        CHECK(c[7][0] == 0.3f);
    }
    composite_constants(c); c[7][0] = 0.25f;
    CHECK(!rsx_folklore_composite_sharpen(c, sizeof c, 2.0f) && c[7][0] == 0.25f);
    composite_constants(c); c[8][0] = 1.0f / 640.0f;
    CHECK(!rsx_folklore_composite_sharpen(c, sizeof c, 2.0f) && c[7][0] == 0.3f);
    composite_constants(c); c[9][1] = 1.0f / 480.0f;
    CHECK(!rsx_folklore_composite_sharpen(c, sizeof c, 2.0f) && c[7][0] == 0.3f);
    composite_constants(c);
    CHECK(!rsx_folklore_composite_sharpen(c, 10 * 16, 2.0f) && c[7][0] == 0.3f);
    CHECK(!rsx_folklore_composite_sharpen(c, 36 * 16, 2.0f) && c[7][0] == 0.3f);
    CHECK(!rsx_folklore_composite_sharpen(NULL, sizeof c, 2.0f));

    printf("test_rsx_folklore_composite: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
