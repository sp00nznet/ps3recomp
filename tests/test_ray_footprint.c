#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../libs/video/rsx_ray_footprint.h"

#define CP(a) ((const float (*)[3])(a))
#define CU(a) ((const float (*)[2])(a))

static int g_checks;
static int g_failures;

static void check(int cond, const char *what)
{
    ++g_checks;
    if (!cond) {
        ++g_failures;
        printf("FAIL: %s\n", what);
    }
}

static void expect_lod(const char *what, const float p[3][3], const float uv[3][2],
                       const float dir[3], float r, unsigned w, unsigned h, double expected,
                       double tol)
{
    float lod = -1.0f;
    int ok = rsx_ray_footprint_lod(p, uv, dir, r, w, h, &lod);
    ++g_checks;
    if (!ok || !(fabs((double)lod - expected) <= tol)) {
        ++g_failures;
        printf("FAIL: %s (ok=%d lod=%.7g expected=%.7g)\n", what, ok, (double)lod, expected);
    }
}

static void expect_fail(const char *what, const float p[3][3], const float uv[3][2],
                        const float dir[3], float r, unsigned w, unsigned h)
{
    float lod = 123.25f;
    int ok = rsx_ray_footprint_lod(p, uv, dir, r, w, h, &lod);
    check(!ok && lod == 123.25f, what);
}

static const float P_UNIT[3][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
static const float UV_ID[3][2] = {{0, 0}, {1, 0}, {0, 1}};
static const float DOWN[3] = {0, 0, -1};
static const float TILT[3] = {0, 1, -1}; /* incidence 1/sqrt(2) */

static void test_known_values(void)
{
    /* Unit right triangle, identity UV: |grad u| = |grad v| = 1, rho = 2r * 256. */
    expect_lod("unit r=1/64", P_UNIT, UV_ID, DOWN, 1.0f / 64.0f, 256, 256, 3.0, 1e-4);
    expect_lod("unit r=1/128", P_UNIT, UV_ID, DOWN, 1.0f / 128.0f, 256, 256, 2.0, 1e-4);
    expect_lod("unit rho=1", P_UNIT, UV_ID, DOWN, 1.0f / 512.0f, 256, 256, 0.0, 1e-4);
    expect_lod("unit rho=3", P_UNIT, UV_ID, DOWN, 3.0f / 512.0f, 256, 256, log2(3.0), 1e-4);

    /* Legs 2 and 4: |grad u| = 1/2, |grad v| = 1/4, rho = 2r * 128. */
    {
        static const float p[3][3] = {{0, 0, 0}, {2, 0, 0}, {0, 4, 0}};
        expect_lod("legs 2x4", p, UV_ID, DOWN, 1.0f / 32.0f, 256, 256, 3.0, 1e-4);
    }

    /* Skewed: u = x - y, v = y, |grad u| = sqrt(2), rho = 2r * 64 sqrt(2) = 16. */
    {
        static const float p[3][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}};
        expect_lod("skewed uv", p, UV_ID, DOWN, 0.125f / sqrtf(2.0f), 64, 64, 4.0, 1e-4);
    }

    /* Thin obtuse (sin ~ 0.01) still accepted: u = x, v = 100y, rho = 2r*1600. */
    {
        static const float p[3][3] = {{0, 0, 0}, {1, 0, 0}, {1, 0.01f, 0}};
        static const float uv[3][2] = {{0, 0}, {1, 0}, {1, 1}};
        expect_lod("thin obtuse", p, uv, DOWN, 1.0f / 800.0f, 16, 16, 2.0, 1e-4);
    }
}

static void test_magnification_and_grazing(void)
{
    expect_lod("magnification", P_UNIT, UV_ID, DOWN, 1e-4f, 256, 256, 0.0, 0.0);
    expect_lod("denormal radius", P_UNIT, UV_ID, DOWN, 1e-40f, 256, 256, 0.0, 0.0);
    expect_lod("tilted 45deg", P_UNIT, UV_ID, TILT, 1.0f / 64.0f, 256, 256, 3.5, 1e-4);

    /* In-plane ray: incidence floored to 1e-4, rho = 2e-7 / 1e-4 * 1024. */
    {
        static const float in_plane[3] = {1, 0, 0};
        static const float nearly[3] = {1, 0, -1e-6f};
        expect_lod("grazing floor", P_UNIT, UV_ID, in_plane, 1e-7f, 1024, 1024, log2(2.048),
                   1e-4);
        expect_lod("near grazing floor", P_UNIT, UV_ID, nearly, 1e-7f, 1024, 1024,
                   log2(2.048), 1e-4);
        expect_lod("grazing capped", P_UNIT, UV_ID, in_plane, 0.5f, 1024, 1024, 10.0, 0.0);
    }
}

static void test_zero_radius_and_constant_uv(void)
{
    static const float uv_const[3][2] = {{5.25f, -3.5f}, {5.25f, -3.5f}, {5.25f, -3.5f}};
    expect_lod("zero radius", P_UNIT, UV_ID, DOWN, 0.0f, 256, 256, 0.0, 0.0);
    expect_lod("negative zero radius", P_UNIT, UV_ID, DOWN, -0.0f, 256, 256, 0.0, 0.0);
    expect_lod("zero radius constant uv", P_UNIT, uv_const, DOWN, 0.0f, 256, 256, 0.0, 0.0);
    expect_lod("constant uv", P_UNIT, uv_const, DOWN, 1.0f, 256, 256, 0.0, 0.0);
    expect_lod("constant uv grazing", P_UNIT, uv_const, TILT, 100.0f, 256, 256, 0.0, 0.0);
}

static void test_direction_scale(void)
{
    static const float scales[] = {1e-30f, 1e-3f, 1.0f, 1e3f, 1e30f, -1.0f, -1e20f};
    size_t i;
    for (i = 0; i < sizeof scales / sizeof scales[0]; ++i) {
        float d[3];
        d[0] = TILT[0] * scales[i];
        d[1] = TILT[1] * scales[i];
        d[2] = TILT[2] * scales[i];
        expect_lod("direction scale", P_UNIT, UV_ID, d, 1.0f / 64.0f, 256, 256, 3.5, 1e-4);
    }
}

static void test_translation_rotation(void)
{
    static const float offsets[][3] = {
        {123.25f, -456.5f, 789.75f}, {-1e6f, 1e6f, 1e6f}, {0.5f, 0.25f, -1000.0f}};
    size_t i;
    int j, k;
    for (i = 0; i < sizeof offsets / sizeof offsets[0]; ++i) {
        float p[3][3];
        for (j = 0; j < 3; ++j)
            for (k = 0; k < 3; ++k)
                p[j][k] = P_UNIT[j][k] + offsets[i][k];
        expect_lod("translation", CP(p), UV_ID, DOWN, 1.0f / 64.0f, 256, 256, 3.0, 1e-4);
        expect_lod("translation tilt", CP(p), UV_ID, TILT, 1.0f / 64.0f, 256, 256, 3.5, 1e-4);
    }

    /* Rodrigues rotation about (1,2,3) by 0.7 rad applied to points and ray. */
    {
        static const float skew[3][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}};
        double ax[3] = {1.0, 2.0, 3.0}, len, c, s, t, R[3][3];
        float p[3][3], q[3][3], d[3], dd[3];
        len = sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
        for (k = 0; k < 3; ++k)
            ax[k] /= len;
        c = cos(0.7);
        s = sin(0.7);
        t = 1.0 - c;
        R[0][0] = c + ax[0] * ax[0] * t;
        R[0][1] = ax[0] * ax[1] * t - ax[2] * s;
        R[0][2] = ax[0] * ax[2] * t + ax[1] * s;
        R[1][0] = ax[1] * ax[0] * t + ax[2] * s;
        R[1][1] = c + ax[1] * ax[1] * t;
        R[1][2] = ax[1] * ax[2] * t - ax[0] * s;
        R[2][0] = ax[2] * ax[0] * t - ax[1] * s;
        R[2][1] = ax[2] * ax[1] * t + ax[0] * s;
        R[2][2] = c + ax[2] * ax[2] * t;
        for (j = 0; j < 3; ++j) {
            for (k = 0; k < 3; ++k) {
                p[j][k] = (float)(R[k][0] * P_UNIT[j][0] + R[k][1] * P_UNIT[j][1] +
                                  R[k][2] * P_UNIT[j][2]);
                q[j][k] = (float)(R[k][0] * skew[j][0] + R[k][1] * skew[j][1] +
                                  R[k][2] * skew[j][2]);
            }
        }
        for (k = 0; k < 3; ++k) {
            d[k] = (float)(R[k][0] * TILT[0] + R[k][1] * TILT[1] + R[k][2] * TILT[2]);
            dd[k] = (float)(R[k][0] * DOWN[0] + R[k][1] * DOWN[1] + R[k][2] * DOWN[2]);
        }
        expect_lod("rotation tilt", CP(p), UV_ID, d, 1.0f / 64.0f, 256, 256, 3.5, 1e-3);
        expect_lod("rotation skewed", CP(q), UV_ID, dd, 0.125f / sqrtf(2.0f), 64, 64, 4.0,
                   1e-3);
    }
}

static void test_geometry_radius_scale(void)
{
    static const float scales[] = {1e-30f, 1e-12f, 1e-3f, 1e3f, 1e12f, 1e30f};
    size_t i;
    int j, k;
    for (i = 0; i < sizeof scales / sizeof scales[0]; ++i) {
        float p[3][3];
        for (j = 0; j < 3; ++j)
            for (k = 0; k < 3; ++k)
                p[j][k] = P_UNIT[j][k] * scales[i];
        expect_lod("geometry/radius scale", CP(p), UV_ID, DOWN, scales[i] / 64.0f, 256, 256,
                   3.0, 1e-3);
        expect_lod("geometry/radius scale tilt", CP(p), UV_ID, TILT, scales[i] / 64.0f, 256,
                   256, 3.5, 1e-3);
    }
}

static void test_texture_extent_and_uv(void)
{
    expect_lod("non-square u", P_UNIT, UV_ID, DOWN, 1.0f / 64.0f, 512, 128, 4.0, 1e-4);
    {
        /* |grad v| = 8: height 128 * 8 = 1024 beats width 512. */
        static const float p[3][3] = {{0, 0, 0}, {1, 0, 0}, {0, 0.125f, 0}};
        expect_lod("non-square v", p, UV_ID, DOWN, 1.0f / 256.0f, 512, 128, 3.0, 1e-4);
    }
    expect_lod("npot cap", P_UNIT, UV_ID, DOWN, 10.0f, 300, 200, 8.0, 0.0);
    expect_lod("1x1 cap", P_UNIT, UV_ID, DOWN, 10.0f, 1, 1, 0.0, 0.0);
    expect_lod("1x1024 cap", P_UNIT, UV_ID, DOWN, 10.0f, 1, 1024, 10.0, 0.0);
    {
        static const float wrap[3][2] = {{10, -7}, {11, -7}, {10, -6}};
        static const float neg[3][2] = {{0, 0}, {-2, 0}, {0, -2}};
        expect_lod("wrapped uv", P_UNIT, wrap, DOWN, 1.0f / 64.0f, 256, 256, 3.0, 1e-4);
        expect_lod("negative uv", P_UNIT, neg, DOWN, 1.0f / 64.0f, 128, 128, 3.0, 1e-4);
    }
}

static void test_overflow(void)
{
    float p[3][3];
    int j, k;
    for (j = 0; j < 3; ++j)
        for (k = 0; k < 3; ++k)
            p[j][k] = P_UNIT[j][k] * 1e-30f;
    expect_lod("tiny triangle big cone", CP(p), UV_ID, DOWN, 1.0f, 256, 256, 8.0, 0.0);
    for (j = 0; j < 3; ++j)
        for (k = 0; k < 3; ++k)
            p[j][k] = P_UNIT[j][k] * 1e30f;
    expect_lod("huge triangle tiny cone", CP(p), UV_ID, DOWN, 1e-30f, 256, 256, 0.0, 0.0);
    {
        static const float big[3][2] = {{-3e38f, 0}, {3e38f, 0}, {0, 3e38f}};
        static const float wide[3][2] = {{-1e30f, 0}, {1e30f, 0}, {0, 1e30f}};
        expect_lod("uv near FLT_MAX", P_UNIT, big, DOWN, 1.0f / 64.0f, 256, 256, 8.0, 0.0);
        expect_lod("huge uv range", P_UNIT, wide, DOWN, 1.0f / 64.0f, 256, 256, 8.0, 0.0);
    }
}

static void test_invalid(void)
{
    float p[3][3], uv[3][2], d[3], lod;
    static const float zero_dir[3] = {0, 0, 0};
    static const float collinear[3][3] = {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}};
    static const float coincident[3][3] = {{1, 2, 3}, {1, 2, 3}, {1, 2, 3}};
    static const float repeated[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 1, 0}};
    static const float sliver[3][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1e-6f, 0}};
    static const float short_edge[3][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1e-7f, 0}};

    memcpy(p, P_UNIT, sizeof p);
    p[1][2] = NAN;
    expect_fail("nan position", CP(p), UV_ID, DOWN, 0.01f, 256, 256);
    p[1][2] = INFINITY;
    expect_fail("inf position", CP(p), UV_ID, DOWN, 0.01f, 256, 256);
    expect_fail("inf position zero radius", CP(p), UV_ID, DOWN, 0.0f, 256, 256);

    memcpy(uv, UV_ID, sizeof uv);
    uv[2][0] = NAN;
    expect_fail("nan uv", P_UNIT, CU(uv), DOWN, 0.01f, 256, 256);
    uv[2][0] = -INFINITY;
    expect_fail("inf uv", P_UNIT, CU(uv), DOWN, 0.01f, 256, 256);

    memcpy(d, DOWN, sizeof d);
    d[0] = NAN;
    expect_fail("nan direction", P_UNIT, UV_ID, d, 0.01f, 256, 256);
    d[0] = INFINITY;
    expect_fail("inf direction", P_UNIT, UV_ID, d, 0.01f, 256, 256);
    expect_fail("zero direction", P_UNIT, UV_ID, zero_dir, 0.01f, 256, 256);
    expect_fail("zero direction zero radius", P_UNIT, UV_ID, zero_dir, 0.0f, 256, 256);

    expect_fail("nan radius", P_UNIT, UV_ID, DOWN, NAN, 256, 256);
    expect_fail("inf radius", P_UNIT, UV_ID, DOWN, INFINITY, 256, 256);
    expect_fail("negative radius", P_UNIT, UV_ID, DOWN, -0.01f, 256, 256);
    expect_fail("zero width", P_UNIT, UV_ID, DOWN, 0.01f, 0, 256);
    expect_fail("zero height", P_UNIT, UV_ID, DOWN, 0.01f, 256, 0);

    expect_fail("collinear", collinear, UV_ID, DOWN, 0.01f, 256, 256);
    expect_fail("collinear zero radius", collinear, UV_ID, DOWN, 0.0f, 256, 256);
    expect_fail("coincident", coincident, UV_ID, DOWN, 0.01f, 256, 256);
    expect_fail("repeated vertex", repeated, UV_ID, DOWN, 0.01f, 256, 256);
    expect_fail("sliver", sliver, UV_ID, DOWN, 0.01f, 256, 256);
    expect_fail("extreme edge ratio", short_edge, UV_ID, DOWN, 0.01f, 256, 256);

    lod = 123.25f;
    check(!rsx_ray_footprint_lod(NULL, UV_ID, DOWN, 0.01f, 256, 256, &lod) && lod == 123.25f,
          "null positions");
    check(!rsx_ray_footprint_lod(P_UNIT, NULL, DOWN, 0.01f, 256, 256, &lod) && lod == 123.25f,
          "null uv");
    check(!rsx_ray_footprint_lod(P_UNIT, UV_ID, NULL, 0.01f, 256, 256, &lod) &&
              lod == 123.25f,
          "null direction");
    check(!rsx_ray_footprint_lod(P_UNIT, UV_ID, DOWN, 0.01f, 256, 256, NULL), "null output");
}

int main(void)
{
    test_known_values();
    test_magnification_and_grazing();
    test_zero_radius_and_constant_uv();
    test_direction_scale();
    test_translation_rotation();
    test_geometry_radius_scale();
    test_texture_extent_and_uv();
    test_overflow();
    test_invalid();
    printf("ray footprint: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
