/*
 * Independent numerical test for rsx_homogeneous_weights().
 *
 * Build: clang -std=c11 -O2 -Wall -Wextra -Werror test_homogeneous_interpolation.c -lm
 *
 * Expected values come from the raster definition of perspective-correct
 * interpolation: a varying at a pixel with screen-space barycentrics lambda is
 *     sum(lambda_i * a_i / clip_w_i) / sum(lambda_i / clip_w_i).
 * Each case starts from Euclidean view-space vertices, a per-vertex source
 * homogeneous scale h and a known screen barycentric. The source position
 * (h*x, h*y, h*z, h) is projected with clip.z = A*view.z + B*view.w and
 * clip.w = view.z, then clip z is inverse-viewport-normalized for nondefault
 * depth ranges. The Euclidean ray barycentric handed to the function is derived
 * from lambda and view z alone (b_i ~ lambda_i / z_i) and verified by
 * reprojecting the Euclidean hit point. Nothing here reuses the
 * implementation's beta/h formula.
 *
 * Tolerances (absolute, on weights; attributes in [-2,2] use 6x):
 *   2e-6  exact dyadic constructions where recovered h is bit exact; covers
 *         float rounding of bary inputs and the normalization.
 *   1e-5  fixed cases with nondefault depth range or non-dyadic h.
 *   1e-4  random cases. Recovering h subtracts A*clip.w from clip.z and loses
 *         about log2(A*z/|B|) bits; generators bound A<=2, z<=4, |B|>=0.25
 *         (at most ~5 bits), so relative h error stays near 1e-5 worst case.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../libs/video/rsx_homogeneous_interpolation.h"

static int g_checks;
static int g_failures;

#define CHECK(cond, ...)                         \
    do {                                         \
        ++g_checks;                              \
        if (!(cond)) {                           \
            ++g_failures;                        \
            printf("  FAIL line %d: ", __LINE__); \
            printf(__VA_ARGS__);                 \
            printf("\n");                        \
        }                                        \
    } while (0)

typedef struct {
    const char *name;
    double proj[4];    /* sx, sy, A, B */
    double depth[2];   /* viewport depth min, max */
    double view[3][3]; /* Euclidean view-space x, y, z (z > 0) */
    double h[3];       /* source homogeneous scale */
    double lambda[3];  /* screen-space barycentrics, sum 1 */
} Scene;

typedef struct {
    Scene scene;
    double wtol;
    int exact_beta;     /* expect bit-exact beta (equal recovered h) */
    double regress_min; /* unadjusted beta must miss the reference by more */
} FixedCase;

static int call_weights(float clip[3][4], const float proj[4], const float depth[2],
                        const float bary[2], float out[3])
{
    return rsx_homogeneous_weights((const float (*)[4])clip, proj, depth, bary, out);
}

static void build_inputs(const Scene *s, float clip[3][4], float proj[4], float depth[2])
{
    const double sx = s->proj[0], sy = s->proj[1], a = s->proj[2], b = s->proj[3];
    const double dmin = s->depth[0], dmax = s->depth[1];

    for (int i = 0; i < 4; ++i)
        proj[i] = (float)s->proj[i];
    depth[0] = (float)dmin;
    depth[1] = (float)dmax;
    for (int i = 0; i < 3; ++i) {
        const double h = s->h[i];
        const double vx = h * s->view[i][0], vy = h * s->view[i][1];
        const double vz = h * s->view[i][2], vw = h;
        const double cw = vz;
        const double cz_final = a * vz + b * vw;
        /* Replay clip z precedes the viewport mapping z' = dmin*w + (dmax-dmin)*z. */
        const double cz = (cz_final - dmin * cw) / (dmax - dmin);
        clip[i][0] = (float)(sx * vx);
        clip[i][1] = (float)(sy * vy);
        clip[i][2] = (float)cz;
        clip[i][3] = (float)cw;
    }
}

/* Euclidean barycentrics of the view-space point under screen barycentric lambda. */
static void euclidean_bary(const Scene *s, double b[3])
{
    double sum = 0.0;
    for (int i = 0; i < 3; ++i) {
        b[i] = s->lambda[i] / s->view[i][2];
        sum += b[i];
    }
    for (int i = 0; i < 3; ++i)
        b[i] /= sum;
}

/* The Euclidean hit point must project to the screen point sum(lambda_i * ndc_i). */
static void check_hit_point(const Scene *s, const double b[3])
{
    const double sx = s->proj[0], sy = s->proj[1];
    double p[3] = { 0.0, 0.0, 0.0 };
    double lin[2] = { 0.0, 0.0 };

    for (int i = 0; i < 3; ++i) {
        for (int k = 0; k < 3; ++k)
            p[k] += b[i] * s->view[i][k];
        lin[0] += s->lambda[i] * sx * s->view[i][0] / s->view[i][2];
        lin[1] += s->lambda[i] * sy * s->view[i][1] / s->view[i][2];
    }
    CHECK(fabs(sx * p[0] / p[2] - lin[0]) <= 1e-10 && fabs(sy * p[1] / p[2] - lin[1]) <= 1e-10,
          "%s: Euclidean bary does not reproject to screen bary", s->name);
}

static double raster_reference(const Scene *s, const double attr[3])
{
    double num = 0.0, den = 0.0;
    for (int i = 0; i < 3; ++i) {
        const double w = s->h[i] * s->view[i][2];
        num += s->lambda[i] * attr[i] / w;
        den += s->lambda[i] / w;
    }
    return num / den;
}

static double run_reference(const Scene *s, const double scalar[3], const double vec[3][3],
                            double wtol, int exact_beta, double regress_min)
{
    float clip[3][4], proj[4], depth[2], bary[2];
    float out[3] = { -7.0f, -7.0f, -7.0f };
    double b[3], ref_w[3], max_err = 0.0;

    build_inputs(s, clip, proj, depth);
    euclidean_bary(s, b);
    check_hit_point(s, b);
    bary[0] = (float)b[1];
    bary[1] = (float)b[2];

    if (!call_weights(clip, proj, depth, bary, out)) {
        CHECK(0, "%s: valid input rejected", s->name);
        return INFINITY;
    }

    for (int k = 0; k < 3; ++k) {
        const double e[3] = { k == 0, k == 1, k == 2 };
        ref_w[k] = raster_reference(s, e);
        if (fabs(out[k] - ref_w[k]) > max_err)
            max_err = fabs(out[k] - ref_w[k]);
    }
    CHECK(max_err <= wtol, "%s: weight error %.3g > %.3g", s->name, max_err, wtol);

    {
        const double got = out[0] * scalar[0] + out[1] * scalar[1] + out[2] * scalar[2];
        const double expv = raster_reference(s, scalar);
        CHECK(fabs(got - expv) <= 6.0 * wtol, "%s: scalar %.9g vs raster %.9g", s->name, got,
              expv);
    }
    for (int c = 0; c < 3; ++c) {
        const double a[3] = { vec[0][c], vec[1][c], vec[2][c] };
        const double got = out[0] * a[0] + out[1] * a[1] + out[2] * a[2];
        const double expv = raster_reference(s, a);
        CHECK(fabs(got - expv) <= 6.0 * wtol, "%s: vec[%d] %.9g vs raster %.9g", s->name, c,
              got, expv);
    }

    if (exact_beta || regress_min > 0.0) {
        const float beta0 = (1.0f - bary[0]) - bary[1];
        if (exact_beta)
            CHECK(out[0] == beta0 && out[1] == bary[0] && out[2] == bary[1],
                  "%s: equal-scale beta not preserved exactly", s->name);
        if (regress_min > 0.0) {
            const double beta[3] = { beta0, bary[0], bary[1] };
            double diff = 0.0;
            for (int k = 0; k < 3; ++k)
                if (fabs(beta[k] - ref_w[k]) > diff)
                    diff = fabs(beta[k] - ref_w[k]);
            CHECK(diff > regress_min, "%s: unadjusted beta error %.3g not above %.3g", s->name,
                  diff, regress_min);
        }
    }
    return max_err;
}

#define TRI_VIEW { { -0.75, -0.5, 1.5 }, { 1.0, -0.25, 2.0 }, { 0.25, 1.0, 3.0 } }
#define TRI_LAMBDA { 0.25, 0.375, 0.375 }
/* A=1.25, B=-0.5 keep every clip value and the recovered h dyadic and exact. */
#define EXACT_PROJ { 1.0, 1.0, 1.25, -0.5 }
#define DEFAULT_RANGE { 0.0, 1.0 }
#define MEASURED_H { 1.0, 0.9993896484375, 0.9998779296875 }
#define STRONG_H { 0.5, 1.0, 2.0 }

static const double k_scalar[3] = { 0.75, -1.5, 2.0 };
static const double k_vec[3][3] = { { 1.0, 0.0, -0.5 }, { -2.0, 0.25, 1.0 }, { 0.5, 1.75, -1.25 } };

static void test_reference_fixed(void)
{
    static const FixedCase cases[] = {
        { { "unit-h", EXACT_PROJ, DEFAULT_RANGE, TRI_VIEW, { 1.0, 1.0, 1.0 }, TRI_LAMBDA },
          2e-6, 1, 0.0 },
        { { "equal-h-2", EXACT_PROJ, DEFAULT_RANGE, TRI_VIEW, { 2.0, 2.0, 2.0 }, TRI_LAMBDA },
          2e-6, 1, 0.0 },
        { { "equal-h-2-range", EXACT_PROJ, { 0.25, 0.75 }, TRI_VIEW, { 2.0, 2.0, 2.0 },
            TRI_LAMBDA },
          2e-6, 1, 0.0 },
        { { "measured-h", EXACT_PROJ, DEFAULT_RANGE, TRI_VIEW, MEASURED_H, TRI_LAMBDA }, 2e-6, 0,
          4e-5 },
        { { "measured-h-range", EXACT_PROJ, { 0.25, 1.0 }, TRI_VIEW, MEASURED_H, TRI_LAMBDA },
          1e-5, 0, 4e-5 },
        { { "strong-h", EXACT_PROJ, DEFAULT_RANGE, TRI_VIEW, STRONG_H, TRI_LAMBDA }, 2e-6, 0,
          0.05 },
        { { "strong-h-range", EXACT_PROJ, { 0.125, 0.625 }, TRI_VIEW, STRONG_H, TRI_LAMBDA },
          1e-5, 0, 0.05 },
        { { "strong-h-proj", { 1.5, 0.75, 1.0078125, -0.25 }, { 0.0, 0.5 }, TRI_VIEW,
            { 2.0, 0.5, 1.0 }, { 0.125, 0.5, 0.375 } },
          1e-5, 0, 0.05 },
        { { "tiny-valid-h", EXACT_PROJ, DEFAULT_RANGE, TRI_VIEW, { 1e-19, 2e-19, 4e-19 },
            TRI_LAMBDA },
          1e-4, 0, 0.0 },
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        const FixedCase *fc = &cases[i];
        const double err = run_reference(&fc->scene, k_scalar, k_vec, fc->wtol, fc->exact_beta,
                                         fc->regress_min);
        printf("  case %-18s max weight err %.3g\n", fc->scene.name, err);
    }
}

static uint32_t g_rng = 0x2468ace1u;

static int rng_int(int lo, int hi)
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return lo + (int)((g_rng >> 8) % (uint32_t)(hi - lo + 1));
}

static void test_reference_random(void)
{
    static const double ranges[5][2] = {
        { 0.0, 1.0 }, { 0.25, 0.75 }, { 0.0, 0.5 }, { 0.5, 1.0 }, { 0.125, 0.875 }
    };
    const int count = 2000;
    double max_err = 0.0;
    char name[32];

    for (int n = 0; n < count; ++n) {
        Scene s;
        double scalar[3], vec[3][3];
        int r, l1, l2;

        memset(&s, 0, sizeof s);
        snprintf(name, sizeof name, "random-%d", n);
        s.name = name;
        s.proj[0] = rng_int(32, 128) / 64.0;
        s.proj[1] = rng_int(32, 128) / 64.0;
        s.proj[2] = 1.0 + rng_int(0, 256) / 256.0;
        s.proj[3] = -rng_int(64, 256) / 256.0;
        r = rng_int(0, 4);
        s.depth[0] = ranges[r][0];
        s.depth[1] = ranges[r][1];
        for (int i = 0; i < 3; ++i) {
            const double z = rng_int(64, 256) / 64.0;
            s.view[i][0] = rng_int(-64, 64) / 64.0 * z;
            s.view[i][1] = rng_int(-64, 64) / 64.0 * z;
            s.view[i][2] = z;
            s.h[i] = rng_int(128, 512) / 256.0;
            scalar[i] = rng_int(-128, 128) / 64.0;
            for (int c = 0; c < 3; ++c)
                vec[i][c] = rng_int(-128, 128) / 64.0;
        }
        if (n % 7 == 1)
            s.h[1] = s.h[2] = s.h[0];
        else if (n % 7 == 2)
            s.h[0] = s.h[1] = s.h[2] = 1.0;
        l1 = rng_int(1, 62);
        l2 = rng_int(1, 63 - l1);
        s.lambda[0] = (64 - l1 - l2) / 64.0;
        s.lambda[1] = l1 / 64.0;
        s.lambda[2] = l2 / 64.0;

        {
            const double err = run_reference(&s, scalar, (const double (*)[3])vec, 1e-4, 0, 0.0);
            if (err > max_err)
                max_err = err;
        }
    }
    printf("  random %d cases max weight err %.3g\n", count, max_err);
}

/* Slightly negative beta from edge roundoff is accepted and not clamped. */
static void test_edge_roundoff(void)
{
    static const Scene equal = { "edge-equal", EXACT_PROJ, DEFAULT_RANGE, TRI_VIEW,
                                 { 2.0, 2.0, 2.0 }, TRI_LAMBDA };
    static const Scene strong = { "edge-strong", EXACT_PROJ, DEFAULT_RANGE, TRI_VIEW, STRONG_H,
                                  TRI_LAMBDA };
    const float bary[2] = { 0.5f, 0.5f + 0x1p-24f };
    const float beta0 = (1.0f - bary[0]) - bary[1];
    float clip[3][4], proj[4], depth[2], out[3];

    build_inputs(&equal, clip, proj, depth);
    CHECK(call_weights(clip, proj, depth, bary, out) == 1, "edge-equal rejected");
    CHECK(beta0 < 0.0f && out[0] == beta0 && out[1] == bary[0] && out[2] == bary[1],
          "edge-equal: negative roundoff beta not preserved");

    build_inputs(&strong, clip, proj, depth);
    CHECK(call_weights(clip, proj, depth, bary, out) == 1, "edge-strong rejected");
    CHECK(isfinite(out[0]) && isfinite(out[1]) && isfinite(out[2]) &&
              fabs((double)out[0] + out[1] + out[2] - 1.0) <= 1e-6 && out[0] < 0.0f,
          "edge-strong: weights not normalized or roundoff sign lost");
    printf("  case %-18s ok\n", "edge-roundoff");
}

static void test_aliasing(void)
{
    static const Scene s = { "alias", EXACT_PROJ, DEFAULT_RANGE, TRI_VIEW, STRONG_H, TRI_LAMBDA };
    float clip[3][4], proj[4], depth[2], bary[2], ref[3];
    double b[3];

    build_inputs(&s, clip, proj, depth);
    euclidean_bary(&s, b);
    bary[0] = (float)b[1];
    bary[1] = (float)b[2];
    CHECK(call_weights(clip, proj, depth, bary, ref) == 1, "alias reference rejected");

    {
        float buf[3] = { bary[0], bary[1], 99.0f };
        CHECK(call_weights(clip, proj, depth, buf, buf) == 1 && memcmp(buf, ref, sizeof ref) == 0,
              "alias out=bary mismatch");
    }
    {
        float c2[3][4];
        memcpy(c2, clip, sizeof c2);
        CHECK(call_weights(c2, proj, depth, bary, &c2[2][0]) == 1 &&
                  memcmp(&c2[2][0], ref, sizeof ref) == 0,
              "alias out=clip[2] mismatch");
    }
    {
        float p2[4];
        memcpy(p2, proj, sizeof p2);
        CHECK(call_weights(clip, p2, depth, bary, p2) == 1 && memcmp(p2, ref, sizeof ref) == 0,
              "alias out=projection mismatch");
    }
    printf("  case %-18s ok\n", "aliasing");
}

static int g_invalid_cases;

static void expect_reject(const char *name, float clip[3][4], const float *proj,
                          const float *depth, const float *bary)
{
    static const float sentinel[3] = { 123.25f, -456.5f, 789.75f };
    float out[3];
    int r;

    memcpy(out, sentinel, sizeof out);
    r = call_weights(clip, proj, depth, bary, out);
    CHECK(r == 0, "%s: invalid input accepted", name);
    CHECK(memcmp(out, sentinel, sizeof out) == 0, "%s: output modified on failure", name);
    ++g_invalid_cases;
}

static void test_invalid(void)
{
    static const Scene base = { "base", EXACT_PROJ, DEFAULT_RANGE, TRI_VIEW, STRONG_H, TRI_LAMBDA };
    static const Scene tiny = { "tiny", EXACT_PROJ, DEFAULT_RANGE, TRI_VIEW, { 1e-21, 1.0, 1.0 },
                                TRI_LAMBDA };
    static const Scene skew = { "skew", EXACT_PROJ, DEFAULT_RANGE, TRI_VIEW, { 0.5, 1.0, 1.0 },
                                TRI_LAMBDA };
    static const Scene huge = { "huge", EXACT_PROJ, DEFAULT_RANGE, TRI_VIEW, { 1.0, 1.0, 1e-19 },
                                TRI_LAMBDA };
    const float bad[3] = { NAN, INFINITY, -INFINITY };
    const char *bad_name[3] = { "nan", "inf", "-inf" };
    float clip[3][4], proj[4], depth[2], bary[2];
    float c2[3][4], p2[4], d2[2], b2[2];
    char name[64];
    double b[3];

    build_inputs(&base, clip, proj, depth);
    euclidean_bary(&base, b);
    bary[0] = (float)b[1];
    bary[1] = (float)b[2];
    {
        float out[3];
        CHECK(call_weights(clip, proj, depth, bary, out) == 1, "invalid-suite base rejected");
    }

    expect_reject("null-clip", NULL, proj, depth, bary);
    expect_reject("null-projection", clip, NULL, depth, bary);
    expect_reject("null-depth", clip, proj, NULL, bary);
    expect_reject("null-bary", clip, proj, depth, NULL);
    CHECK(call_weights(clip, proj, depth, bary, NULL) == 0, "null-out accepted");
    ++g_invalid_cases;

    for (int k = 0; k < 3; ++k) {
        for (int v = 0; v < 3; ++v) {
            for (int c = 0; c < 4; ++c) {
                memcpy(c2, clip, sizeof c2);
                c2[v][c] = bad[k];
                snprintf(name, sizeof name, "clip[%d][%d]=%s", v, c, bad_name[k]);
                expect_reject(name, c2, proj, depth, bary);
            }
        }
        for (int i = 0; i < 4; ++i) {
            memcpy(p2, proj, sizeof p2);
            p2[i] = bad[k];
            snprintf(name, sizeof name, "projection[%d]=%s", i, bad_name[k]);
            expect_reject(name, clip, p2, depth, bary);
        }
        for (int i = 0; i < 2; ++i) {
            memcpy(d2, depth, sizeof d2);
            d2[i] = bad[k];
            snprintf(name, sizeof name, "depth[%d]=%s", i, bad_name[k]);
            expect_reject(name, clip, proj, d2, bary);
            memcpy(b2, bary, sizeof b2);
            b2[i] = bad[k];
            snprintf(name, sizeof name, "bary[%d]=%s", i, bad_name[k]);
            expect_reject(name, clip, proj, depth, b2);
        }
    }

    {
        static const struct { int idx; float value; const char *name; } shapes[] = {
            { 0, 0.0f, "sx=0" },     { 0, -1.0f, "sx<0" },  { 1, 0.0f, "sy=0" },
            { 1, -1.0f, "sy<0" },    { 2, 0.999f, "A<1" },  { 2, 0.0f, "A=0" },
            { 3, 0.0f, "B=0" },      { 3, 0.5f, "B>0" },
        };
        for (size_t i = 0; i < sizeof shapes / sizeof shapes[0]; ++i) {
            memcpy(p2, proj, sizeof p2);
            p2[shapes[i].idx] = shapes[i].value;
            expect_reject(shapes[i].name, clip, p2, depth, bary);
        }
    }
    {
        static const struct { float lo, hi; const char *name; } ranges[] = {
            { -0.125f, 0.75f, "depth-min<0" }, { 0.25f, 1.125f, "depth-max>1" },
            { 0.5f, 0.5f, "depth-min=max" },   { 0.75f, 0.25f, "depth-min>max" },
        };
        for (size_t i = 0; i < sizeof ranges / sizeof ranges[0]; ++i) {
            d2[0] = ranges[i].lo;
            d2[1] = ranges[i].hi;
            expect_reject(ranges[i].name, clip, proj, d2, bary);
        }
    }

    /* Recovered h: (clip.z - A*clip.w)/B with the default range. */
    memcpy(c2, clip, sizeof c2);
    c2[0][2] = proj[2] * c2[0][3];
    expect_reject("h-zero", c2, proj, depth, bary);
    memcpy(c2, clip, sizeof c2);
    c2[0][2] = proj[2] * c2[0][3] + 1.0f;
    expect_reject("h-negative", c2, proj, depth, bary);
    memcpy(c2, clip, sizeof c2);
    c2[0][2] = -3.0e38f;
    expect_reject("h-overflow", c2, proj, depth, bary);
    build_inputs(&tiny, c2, p2, d2);
    expect_reject("h-tiny", c2, p2, d2, bary);

    /* h = (0.5, 1, 1): sum(beta/h) = 2 - bx - by exactly. */
    build_inputs(&skew, c2, p2, d2);
    b2[0] = 1.0f;
    b2[1] = 1.0f;
    expect_reject("denominator-zero", c2, p2, d2, b2);
    b2[1] = 1.5f;
    expect_reject("denominator-negative", c2, p2, d2, b2);
    {
        float buf[3] = { 1.0f, 1.0f, 42.0f };
        const float before[3] = { 1.0f, 1.0f, 42.0f };
        CHECK(call_weights(c2, p2, d2, buf, buf) == 0 && memcmp(buf, before, sizeof buf) == 0,
              "aliased failure modified buffer");
        ++g_invalid_cases;
    }

    /* beta2/h2 = 1e30/1e-19 overflows float. */
    build_inputs(&huge, c2, p2, d2);
    b2[0] = 0.0f;
    b2[1] = 1e30f;
    expect_reject("denominator-nonfinite", c2, p2, d2, b2);

    b2[0] = 3.0e38f;
    b2[1] = 3.0e38f;
    expect_reject("beta0-overflow", clip, proj, depth, b2);

    printf("  invalid %d cases\n", g_invalid_cases);
}

int main(void)
{
    int f0, c0;

    printf("homogeneous-compile: ok\n");

    f0 = g_failures;
    c0 = g_checks;
    test_reference_fixed();
    test_reference_random();
    test_edge_roundoff();
    test_aliasing();
    printf("homogeneous-reference: %s (%d checks, %d failures)\n",
           g_failures == f0 ? "ok" : "FAIL", g_checks - c0, g_failures - f0);

    f0 = g_failures;
    c0 = g_checks;
    test_invalid();
    printf("homogeneous-invalid: %s (%d checks, %d failures)\n", g_failures == f0 ? "ok" : "FAIL",
           g_checks - c0, g_failures - f0);

    printf("homogeneous-interpolation: %s (%d checks, %d failures)\n",
           g_failures ? "FAIL" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
