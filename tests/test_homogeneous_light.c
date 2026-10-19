/*
 * test_homogeneous_light.c - standalone reference tests for
 * libs/video/rsx_homogeneous_light.h (homogeneous-light-reference).
 *
 * Source attribution: original work written for the Folklore RSX full path
 * tracing integration (base 1654139). No external code was used.
 * This is a mathematical reference check only; it is not gameplay validation.
 *
 * Build (homogeneous-light-compile):
 *   clang -std=c11 -Wall -Wextra -Werror tests/test_homogeneous_light.c -lm
 */
#include "../libs/video/rsx_homogeneous_light.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_checks;
static int g_failures;
static int g_case = -1;

static void check_at(int ok, const char *what, int line)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        if (g_case >= 0)
            fprintf(stderr, "FAIL %s:%d (case %d): %s\n", __FILE__, line, g_case, what);
        else
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, line, what);
    }
}

#define CHECK(cond) check_at((cond) ? 1 : 0, #cond, __LINE__)

static const float k_sentinel[4] = {12345.0f, -6789.5f, 42.25f, -0.125f};
static const float k_captured_h = 0.9993896484375f;

enum { ROT_IDENT, ROT_Z90, ROT_X90, ROT_Y90, ROT_Z180, ROT_CYCLIC, ROT_Z345, ROT_COUNT };

static float k_rot[ROT_COUNT][3][3] = {
    {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},
    {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
    {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},
    {{0, 0, 1}, {0, 1, 0}, {-1, 0, 0}},
    {{-1, 0, 0}, {0, -1, 0}, {0, 0, 1}},
    {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},
    {{0.6f, -0.8f, 0}, {0.8f, 0.6f, 0}, {0, 0, 1}},
};

static int xform(float m[3][4], const float l[4], float h, float out[4])
{
    return rsx_homogeneous_light_transform((const float (*)[4])m, l, h, out);
}

static void set_frame(float m[3][4], float r[3][3], float tx, float ty, float tz)
{
    int i, j;
    for (i = 0; i < 3; ++i)
        for (j = 0; j < 3; ++j)
            m[i][j] = r[i][j];
    m[0][3] = tx;
    m[1][3] = ty;
    m[2][3] = tz;
}

static int near(double a, double b, double rel)
{
    return fabs(a - b) <= rel * (1.0 + fabs(a) + fabs(b));
}

static int is_sentinel(const float out[4])
{
    return memcmp(out, k_sentinel, sizeof k_sentinel) == 0;
}

/* Independent algebra: E.xyz - X*E.w == R*Q/h with X and Q from the shader. */
static int identity_holds(float m[3][4], const float l[4], const float p[4], const float e[4])
{
    double h = p[3], q[3];
    int i, k;

    for (k = 0; k < 3; ++k)
        q[k] = (double)l[k] - (double)p[k] * l[3];
    for (i = 0; i < 3; ++i) {
        double x = ((double)m[i][0] * p[0] + (double)m[i][1] * p[1] + (double)m[i][2] * p[2] + (double)m[i][3] * h) / h;
        double rq = ((double)m[i][0] * q[0] + (double)m[i][1] * q[1] + (double)m[i][2] * q[2]) / h;
        double lhs = (double)e[i] - x * e[3];
        double mag = 1.0 + fabs((double)e[i]) + fabs(x * e[3]) + fabs(rq);
        if (!(fabs(lhs - rq) <= 1e-6 * mag))
            return 0;
    }
    return e[3] == l[3];
}

/* Point lights: apparent view point E.xyz/E.w must equal the game-space ray
 * endpoint Pxyz/h + Q/(h*Lw) mapped through M as a point. */
static int endpoint_holds(float m[3][4], const float l[4], const float p[4], const float e[4])
{
    double h = p[3], lw = l[3], g[3], gm[3];
    int i, k;

    for (k = 0; k < 3; ++k) {
        g[k] = (double)p[k] / h + ((double)l[k] - (double)p[k] * lw) / (h * lw);
        gm[k] = fabs((double)p[k] / h) + fabs(g[k]);
    }
    for (i = 0; i < 3; ++i) {
        double want = (double)m[i][0] * g[0] + (double)m[i][1] * g[1] + (double)m[i][2] * g[2] + m[i][3];
        double got = (double)e[i] / e[3];
        double mag = 1.0 + fabs(got) + fabs((double)m[i][3]);
        for (k = 0; k < 3; ++k)
            mag += fabs((double)m[i][k]) * gm[k];
        if (!(fabs(got - want) <= 1e-6 * mag))
            return 0;
    }
    return 1;
}

static void expect_values(int line, float m[3][4], const float l[4], float h, double x, double y, double z, double w)
{
    float out[4];
    int ok;

    memcpy(out, k_sentinel, sizeof out);
    ok = xform(m, l, h, out);
    check_at(ok == 1, "transform succeeds", line);
    check_at(near(out[0], x, 1e-6) && near(out[1], y, 1e-6) && near(out[2], z, 1e-6) && near(out[3], w, 0.0),
             "expected output", line);
}

static void expect_fail(int line, float m[3][4], const float l[4], float h, const char *what)
{
    float out[4];
    int ok;

    memcpy(out, k_sentinel, sizeof out);
    ok = xform(m, l, h, out);
    check_at(ok == 0, what, line);
    check_at(is_sentinel(out), "output unchanged on failure", line);
}

static void test_deterministic(void)
{
    float m[3][4];
    float l[4] = {1.0f, 2.0f, 3.0f, 1.0f};
    double hc = (double)k_captured_h;

    set_frame(m, k_rot[ROT_IDENT], 0.0f, 0.0f, 0.0f);
    expect_values(__LINE__, m, l, 1.0f, 1.0, 2.0, 3.0, 1.0);
    expect_values(__LINE__, m, l, 0.5f, 2.0, 4.0, 6.0, 1.0);
    expect_values(__LINE__, m, l, 2.0f, 0.5, 1.0, 1.5, 1.0);
    expect_values(__LINE__, m, l, 4.0f, 0.25, 0.5, 0.75, 1.0);

    set_frame(m, k_rot[ROT_IDENT], 0.25f, -0.5f, 1.0f);
    expect_values(__LINE__, m, l, k_captured_h, 1.0 / hc + 0.25, 2.0 / hc - 0.5, 3.0 / hc + 1.0, 1.0);
    {
        float a[4], b[4];
        CHECK(xform(m, l, k_captured_h, a) && xform(m, l, 1.0f, b));
        CHECK(fabs((double)a[2] - b[2]) > 1e-3);
    }

    set_frame(m, k_rot[ROT_IDENT], 10.0f, -20.0f, 30.0f);
    expect_values(__LINE__, m, l, 1.0f, 11.0, -18.0, 33.0, 1.0);
    expect_values(__LINE__, m, l, 2.0f, 10.5, -19.0, 31.5, 1.0);
    {
        float lq[4] = {1.0f, 2.0f, 3.0f, 0.25f};
        float l0[4] = {1.0f, 2.0f, 3.0f, 0.0f};
        float ln[4] = {1.0f, 2.0f, 3.0f, -1.0f};
        expect_values(__LINE__, m, lq, 2.0f, 3.0, -4.0, 9.0, 0.25);
        expect_values(__LINE__, m, l0, 2.0f, 0.5, 1.0, 1.5, 0.0);
        expect_values(__LINE__, m, ln, 2.0f, -9.5, 21.0, -28.5, -1.0);
    }

    set_frame(m, k_rot[ROT_Z90], 1.0f, 2.0f, 3.0f);
    expect_values(__LINE__, m, l, 0.5f, -3.0, 4.0, 9.0, 1.0);
    {
        float l0[4] = {1.0f, 2.0f, 3.0f, 0.0f};
        float lq[4] = {1.0f, 2.0f, 3.0f, 0.25f};
        float ln[4] = {1.0f, 2.0f, 3.0f, -1.0f};
        expect_values(__LINE__, m, l0, 0.5f, -4.0, 2.0, 6.0, 0.0);
        set_frame(m, k_rot[ROT_X90], 1.0f, 2.0f, 3.0f);
        expect_values(__LINE__, m, lq, 2.0f, 0.75, -1.0, 1.75, 0.25);
        set_frame(m, k_rot[ROT_Y90], 1.0f, 2.0f, 3.0f);
        expect_values(__LINE__, m, ln, 0.5f, 5.0, 2.0, -5.0, -1.0);
    }

    /* Directions ignore translation entirely. */
    {
        float l0[4] = {1.0f, 2.0f, 3.0f, 0.0f};
        float a[4], b[4];
        set_frame(m, k_rot[ROT_Z345], 0.0f, 0.0f, 0.0f);
        CHECK(xform(m, l0, 0.75f, a));
        set_frame(m, k_rot[ROT_Z345], 1000.0f, -2000.0f, 3000.0f);
        CHECK(xform(m, l0, 0.75f, b));
        CHECK(memcmp(a, b, sizeof a) == 0);
    }
}

static void test_rotations(void)
{
    static const float hs[3] = {0.5f, 0.9993896484375f, 3.0f};
    static const float lws[4] = {0.0f, 1.0f, 0.25f, -1.0f};
    int r, i, j, k, a, b;

    for (r = 0; r < ROT_COUNT; ++r) {
        for (i = 0; i < 3; ++i)
            for (j = 0; j < 3; ++j) {
                double d = 0.0;
                for (k = 0; k < 3; ++k)
                    d += (double)k_rot[r][i][k] * k_rot[r][j][k];
                CHECK(near(d, i == j ? 1.0 : 0.0, 1e-6));
            }
        for (a = 0; a < 3; ++a)
            for (b = 0; b < 4; ++b) {
                float m[3][4], e[4];
                float l[4] = {4.0f, -7.0f, 2.5f, lws[b]};
                float p[4];
                p[0] = 3.0f * hs[a];
                p[1] = -1.0f * hs[a];
                p[2] = 2.0f * hs[a];
                p[3] = hs[a];
                set_frame(m, k_rot[r], -6.0f, 1.5f, 8.0f);
                CHECK(xform(m, l, p[3], e));
                CHECK(identity_holds(m, l, p, e));
                if (l[3] > 0.0f)
                    CHECK(endpoint_holds(m, l, p, e));
            }
    }
}

static void test_aliasing(void)
{
    float m[3][4];
    float l[4] = {1.0f, -2.0f, 3.5f, 0.25f};
    float ref[4], buf[4];

    set_frame(m, k_rot[ROT_Z90], 3.0f, -4.0f, 5.0f);
    CHECK(xform(m, l, 0.5f, ref));
    memcpy(buf, l, sizeof buf);
    CHECK(xform(m, buf, 0.5f, buf));
    CHECK(memcmp(buf, ref, sizeof buf) == 0);

    {
        float big[4] = {3e38f, 0.0f, 0.0f, 0.0f};
        set_frame(m, k_rot[ROT_IDENT], 0.0f, 0.0f, 0.0f);
        memcpy(buf, big, sizeof buf);
        CHECK(xform(m, buf, 0.5f, buf) == 0);
        CHECK(memcmp(buf, big, sizeof buf) == 0);
    }
}

static void test_magnitudes(void)
{
    float m[3][4];
    const float c = 0.70710677f;

    set_frame(m, k_rot[ROT_IDENT], 0.0f, 0.0f, 0.0f);
    {
        float l[4] = {1e-30f, -2e-30f, 3e-30f, 0.0f};
        float h = 1e-3f;
        expect_values(__LINE__, m, l, h, (double)l[0] / h, (double)l[1] / h, (double)l[2] / h, 0.0);
    }
    {
        float l[4] = {1e-21f, 0.0f, 0.0f, 0.0f};
        float h = 2e-20f;
        expect_values(__LINE__, m, l, h, (double)l[0] / h, 0.0, 0.0, 0.0);
    }
    {
        float l[4] = {1e30f, -1e30f, 2e30f, 0.0f};
        float h = 1e-5f;
        expect_values(__LINE__, m, l, h, (double)l[0] / h, (double)l[1] / h, (double)l[2] / h, 0.0);
    }
    {
        float l[4] = {FLT_MAX, 0.0f, 0.0f, 0.0f};
        expect_values(__LINE__, m, l, 1.0f, (double)FLT_MAX, 0.0, 0.0, 0.0);
    }
    {
        /* t*Lw and R*L/h each exceed FLT_MAX but cancel exactly. */
        float l[4] = {3e38f, 0.0f, 0.0f, 2.0f};
        set_frame(m, k_rot[ROT_IDENT], -3e38f, 0.0f, 0.0f);
        expect_values(__LINE__, m, l, 0.5f, 0.0, 0.0, 0.0, 2.0);
    }
    {
        /* Float row sum would overflow; double sum then /h is finite. */
        float l[4] = {FLT_MAX, FLT_MAX, 0.0f, 0.0f};
        float r45[3][3] = {{c, -c, 0.0f}, {c, c, 0.0f}, {0.0f, 0.0f, 1.0f}};
        set_frame(m, r45, 0.0f, 0.0f, 0.0f);
        expect_values(__LINE__, m, l, 2.0f, 0.0, (double)c * FLT_MAX, 0.0, 0.0);
        expect_fail(__LINE__, m, l, 1.0f, "rotated FLT_MAX overflow rejected");
    }

    set_frame(m, k_rot[ROT_IDENT], 0.0f, 0.0f, 0.0f);
    {
        float l[4] = {3e38f, 0.0f, 0.0f, 0.0f};
        expect_fail(__LINE__, m, l, 0.5f, "light/h overflow rejected");
    }
    {
        float l[4] = {1e20f, 0.0f, 0.0f, 0.0f};
        expect_fail(__LINE__, m, l, 2e-20f, "tiny h overflow rejected");
    }
    {
        float l[4] = {0.0f, 0.0f, 0.0f, 2.0f};
        set_frame(m, k_rot[ROT_IDENT], 0.0f, 3e38f, 0.0f);
        expect_fail(__LINE__, m, l, 1.0f, "translation overflow rejected");
    }
}

static void test_rejections(void)
{
    volatile float zero = 0.0f;
    float qnan = zero / zero;
    float inf = 1.0f / zero;
    float bads[3];
    const float base[4] = {1.0f, 2.0f, 3.0f, 1.0f};
    int lane, v, i;
    char what[64];

    bads[0] = qnan;
    bads[1] = inf;
    bads[2] = -inf;

    for (lane = 0; lane < 17; ++lane)
        for (v = 0; v < 3; ++v) {
            float m[3][4], l[4], h = 0.5f;
            set_frame(m, k_rot[ROT_Z90], 1.0f, 2.0f, 3.0f);
            memcpy(l, base, sizeof l);
            if (lane < 12)
                m[lane / 4][lane % 4] = bads[v];
            else if (lane < 16)
                l[lane - 12] = bads[v];
            else
                h = bads[v];
            snprintf(what, sizeof what, "nonfinite lane %d kind %d rejected", lane, v);
            expect_fail(__LINE__, m, l, h, what);
        }

    {
        const float bad_h[6] = {0.0f, -0.0f, -1.0f, 1e-21f, 5e-21f, FLT_MIN};
        float m[3][4];
        set_frame(m, k_rot[ROT_IDENT], 0.0f, 0.0f, 0.0f);
        for (i = 0; i < 6; ++i) {
            snprintf(what, sizeof what, "bad h index %d rejected", i);
            expect_fail(__LINE__, m, base, bad_h[i], what);
        }
        expect_fail(__LINE__, m, base, -FLT_MAX, "negative h rejected");
    }

    {
        float m[3][4], out[4];
        set_frame(m, k_rot[ROT_IDENT], 0.0f, 0.0f, 0.0f);
        memcpy(out, k_sentinel, sizeof out);
        CHECK(rsx_homogeneous_light_transform(NULL, base, 1.0f, out) == 0);
        CHECK(is_sentinel(out));
        CHECK(rsx_homogeneous_light_transform((const float (*)[4])m, NULL, 1.0f, out) == 0);
        CHECK(is_sentinel(out));
        CHECK(rsx_homogeneous_light_transform((const float (*)[4])m, base, 1.0f, NULL) == 0);
    }
}

/* Wrong formula used by a naive port: (R*L + t*Lw) / div, w = Lw. */
static void make_naive(float m[3][4], const float l[4], double div, float out[4])
{
    int i;
    for (i = 0; i < 3; ++i)
        out[i] = (float)(((double)m[i][0] * l[0] + (double)m[i][1] * l[1] + (double)m[i][2] * l[2] + (double)m[i][3] * l[3]) / div);
    out[3] = l[3];
}

static void test_adversarial(void)
{
    float m[3][4], e[4], naive[4], scaled_all[4];
    float l[4] = {1.0f, 2.0f, 3.0f, 1.0f};
    float p[4] = {1.5f, -0.5f, 2.0f, 0.5f};
    double d2 = 0.0;
    int i;

    /* h != 1 with translation: both wrong variants are visibly wrong. */
    set_frame(m, k_rot[ROT_Z90], 5.0f, -3.0f, 2.0f);
    CHECK(xform(m, l, p[3], e));
    make_naive(m, l, 1.0, naive);
    make_naive(m, l, p[3], scaled_all);
    CHECK(identity_holds(m, l, p, e));
    CHECK(endpoint_holds(m, l, p, e));
    CHECK(!identity_holds(m, l, p, naive));
    CHECK(!endpoint_holds(m, l, p, naive));
    CHECK(!identity_holds(m, l, p, scaled_all));
    CHECK(!endpoint_holds(m, l, p, scaled_all));
    for (i = 0; i < 3; ++i)
        d2 += ((double)e[i] - naive[i]) * ((double)e[i] - naive[i]);
    CHECK(sqrt(d2) > 1.0);

    /* No translation: forgetting /h is still detected; scaling all by 1/h is not. */
    set_frame(m, k_rot[ROT_Z90], 0.0f, 0.0f, 0.0f);
    make_naive(m, l, 1.0, naive);
    make_naive(m, l, p[3], scaled_all);
    CHECK(!identity_holds(m, l, p, naive));
    CHECK(identity_holds(m, l, p, scaled_all));

    /* h == 1: all three coincide, so the mismatch is caused by h alone. */
    set_frame(m, k_rot[ROT_Z90], 5.0f, -3.0f, 2.0f);
    p[3] = 1.0f;
    CHECK(xform(m, l, p[3], e));
    make_naive(m, l, 1.0, naive);
    make_naive(m, l, 1.0, scaled_all);
    CHECK(identity_holds(m, l, p, e));
    CHECK(identity_holds(m, l, p, naive));
    CHECK(identity_holds(m, l, p, scaled_all));
}

static uint64_t g_rng = UINT64_C(0x243F6A8885A308D3);

static double rnd01(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (double)(g_rng >> 11) * (1.0 / 9007199254740992.0);
}

static double rnd_range(double lo, double hi)
{
    return lo + (hi - lo) * rnd01();
}

static void test_randomized(void)
{
    int n, k;

    for (n = 0; n < 2000; ++n) {
        float m[3][4], l[4], p[4], e[4];
        double q[4], norm, x, y, z, w, h;

        g_case = n;
        do {
            for (k = 0; k < 4; ++k)
                q[k] = rnd_range(-1.0, 1.0);
            norm = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        } while (norm < 0.1);
        x = q[0] / norm;
        y = q[1] / norm;
        z = q[2] / norm;
        w = q[3] / norm;
        m[0][0] = (float)(1.0 - 2.0 * (y * y + z * z));
        m[0][1] = (float)(2.0 * (x * y - z * w));
        m[0][2] = (float)(2.0 * (x * z + y * w));
        m[1][0] = (float)(2.0 * (x * y + z * w));
        m[1][1] = (float)(1.0 - 2.0 * (x * x + z * z));
        m[1][2] = (float)(2.0 * (y * z - x * w));
        m[2][0] = (float)(2.0 * (x * z - y * w));
        m[2][1] = (float)(2.0 * (y * z + x * w));
        m[2][2] = (float)(1.0 - 2.0 * (x * x + y * y));
        for (k = 0; k < 3; ++k)
            m[k][3] = (float)rnd_range(-100.0, 100.0);

        switch (n % 8) {
        case 0: p[3] = 1.0f; break;
        case 1: p[3] = k_captured_h; break;
        default: p[3] = (float)pow(2.0, rnd_range(-4.0, 4.0)); break;
        }
        h = p[3];
        for (k = 0; k < 3; ++k) {
            p[k] = (float)(rnd_range(-100.0, 100.0) * h);
            l[k] = (float)rnd_range(-100.0, 100.0);
        }
        switch (n % 5) {
        case 0: l[3] = 0.0f; break;
        case 1: l[3] = 1.0f; break;
        case 2: l[3] = 0.25f; break;
        case 3: l[3] = -1.0f; break;
        default: l[3] = (float)rnd_range(0.05, 4.0); break;
        }

        CHECK(xform(m, l, p[3], e));
        CHECK(identity_holds(m, l, p, e));
        if (l[3] > 0.0f)
            CHECK(endpoint_holds(m, l, p, e));
    }
    g_case = -1;
}

int main(void)
{
    test_deterministic();
    test_rotations();
    test_aliasing();
    test_magnitudes();
    test_rejections();
    test_adversarial();
    test_randomized();
    printf("homogeneous-light-reference: %s (%d checks, %d failures)\n", g_failures ? "FAIL" : "PASS", g_checks,
           g_failures);
    return g_failures ? 1 : 0;
}
