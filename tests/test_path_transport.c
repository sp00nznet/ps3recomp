/*
 * Standalone tests for libs/video/rsx_path_transport.h.
 * Build: cc -std=c17 -Wall -Wextra -Werror tests/test_path_transport.c -lm
 */

#include "../libs/video/rsx_path_transport.h"

#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define TEST_PI 3.14159265358979323846

static int g_checks;
static int g_failures;

static void check(bool cond, const char* name)
{
    ++g_checks;
    if (cond) {
        printf("PASS %s\n", name);
    } else {
        ++g_failures;
        printf("FAIL %s\n", name);
    }
}

static bool near(double a, double b, double tol)
{
    return fabs(a - b) <= tol;
}

static rsx_pt_vec3 v3(float x, float y, float z)
{
    rsx_pt_vec3 v = {x, y, z};
    return v;
}

static bool is_zero3(rsx_pt_vec3 v)
{
    return v.x == 0.0f && v.y == 0.0f && v.z == 0.0f;
}

static bool near3(rsx_pt_vec3 a, rsx_pt_vec3 b, double tol)
{
    return near(a.x, b.x, tol) && near(a.y, b.y, tol) && near(a.z, b.z, tol);
}

static double len3(rsx_pt_vec3 v)
{
    return sqrt((double)v.x * v.x + (double)v.y * v.y + (double)v.z * v.z);
}

/* Unit normal in double for reference comparisons. */
static void unit_ref(rsx_pt_vec3 v, double n[3])
{
    double x = v.x, y = v.y, z = v.z;
    double l = sqrt(x * x + y * y + z * z);
    n[0] = x / l;
    n[1] = y / l;
    n[2] = z / l;
}

static double dot_ref(rsx_pt_vec3 d, const double n[3])
{
    return (double)d.x * n[0] + (double)d.y * n[1] + (double)d.z * n[2];
}

static uint32_t rng_next(uint32_t* s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

/* Uniform in [0,1) with 24-bit resolution. */
static float rng_float(uint32_t* s)
{
    return (float)(rng_next(s) >> 8) * (1.0f / 16777216.0f);
}

static void test_cosine_analytic(void)
{
    rsx_pt_vec3 d;
    float pdf;

    bool ok = rsx_pt_cosine_sample(v3(0, 0, 1), 0.0f, 0.0f, &d, &pdf);
    check(ok && near3(d, v3(0, 0, 1), 1e-6) && near(pdf, 1.0 / TEST_PI, 1e-6),
          "cosine: u=(0,0) returns the normal with pdf 1/pi");

    ok = rsx_pt_cosine_sample(v3(0, 0, 1), 0.75f, 0.0f, &d, &pdf);
    check(ok && near3(d, v3(0.8660254f, 0.0f, 0.5f), 1e-6) && near(pdf, 0.5 / TEST_PI, 1e-6),
          "cosine: +z normal u=(0.75,0) gives (sqrt(3)/2,0,1/2)");

    ok = rsx_pt_cosine_sample(v3(0, 0, 1), 0.75f, 0.25f, &d, &pdf);
    check(ok && near3(d, v3(0.0f, 0.8660254f, 0.5f), 1e-6),
          "cosine: +z normal u=(0.75,0.25) rotates azimuth by pi/2");

    ok = rsx_pt_cosine_sample(v3(0, 0, -1), 0.75f, 0.0f, &d, &pdf);
    check(ok && near(d.z, -0.5, 1e-6) && near(len3(d), 1.0, 1e-6) && near(pdf, 0.5 / TEST_PI, 1e-6),
          "cosine: -z normal samples the lower hemisphere");

    ok = rsx_pt_cosine_sample(v3(0, 0, 5), 0.0f, 0.5f, &d, &pdf);
    check(ok && near3(d, v3(0, 0, 1), 1e-6), "cosine: unnormalized normal is normalized");

    /* cos(theta) must equal sqrt(1-u1) for every normal and every sample. */
    rsx_pt_vec3 normals[] = {v3(1, 0, 0), v3(0, -1, 0), v3(0.3f, -0.7f, 0.2f), v3(-2, 5, -9)};
    uint32_t seed = 0x1234567u;
    double worst = 0.0;
    bool all_ok = true;
    for (unsigned k = 0; k < sizeof(normals) / sizeof(normals[0]); ++k) {
        double n[3];
        unit_ref(normals[k], n);
        for (int i = 0; i < 1000; ++i) {
            float u1 = rng_float(&seed), u2 = rng_float(&seed);
            if (!rsx_pt_cosine_sample(normals[k], u1, u2, &d, &pdf)) {
                all_ok = false;
                continue;
            }
            double e = fabs(dot_ref(d, n) - sqrt(1.0 - (double)u1));
            if (e > worst)
                worst = e;
        }
    }
    check(all_ok && worst < 1e-5, "cosine: cos(theta) == sqrt(1-u1) for arbitrary normals");
}

static void test_cosine_moments(void)
{
    rsx_pt_vec3 normals[] = {v3(0, 0, 1), v3(0, 0, -1), v3(1, 1, 1), v3(-0.2f, 0.9f, -0.4f),
                             v3(1e-6f, 0, -1)};
    const int count = 100000;
    uint32_t seed = 0xC0FFEEu;
    char name[128];

    for (unsigned k = 0; k < sizeof(normals) / sizeof(normals[0]); ++k) {
        double n[3];
        unit_ref(normals[k], n);
        double sum_c = 0.0, sum_c2 = 0.0, tan_sum[3] = {0.0, 0.0, 0.0};
        double worst_len = 0.0, worst_pdf = 0.0, min_c = 1.0;
        bool all_ok = true;
        for (int i = 0; i < count; ++i) {
            rsx_pt_vec3 d;
            float pdf;
            float u1 = rng_float(&seed), u2 = rng_float(&seed);
            if (!rsx_pt_cosine_sample(normals[k], u1, u2, &d, &pdf)) {
                all_ok = false;
                continue;
            }
            double c = dot_ref(d, n);
            sum_c += c;
            sum_c2 += c * c;
            tan_sum[0] += d.x - c * n[0];
            tan_sum[1] += d.y - c * n[1];
            tan_sum[2] += d.z - c * n[2];
            double le = fabs(len3(d) - 1.0);
            if (le > worst_len)
                worst_len = le;
            double pe = fabs((double)pdf - c / TEST_PI);
            if (pe > worst_pdf)
                worst_pdf = pe;
            if (c < min_c)
                min_c = c;
        }
        double mean_c = sum_c / count, mean_c2 = sum_c2 / count;
        double tan_mean = sqrt(tan_sum[0] * tan_sum[0] + tan_sum[1] * tan_sum[1] +
                               tan_sum[2] * tan_sum[2]) / count;

        snprintf(name, sizeof(name), "cosine MC normal %u: all samples succeed", k);
        check(all_ok, name);
        snprintf(name, sizeof(name), "cosine MC normal %u: E[cos]=%.5f ~ 2/3", k, mean_c);
        check(near(mean_c, 2.0 / 3.0, 5e-3), name);
        snprintf(name, sizeof(name), "cosine MC normal %u: E[cos^2]=%.5f ~ 1/2", k, mean_c2);
        check(near(mean_c2, 0.5, 5e-3), name);
        snprintf(name, sizeof(name), "cosine MC normal %u: tangent mean %.5f ~ 0", k, tan_mean);
        check(tan_mean < 1e-2, name);
        snprintf(name, sizeof(name), "cosine MC normal %u: unit length, upper hemisphere, pdf=cos/pi", k);
        check(worst_len < 1e-5 && min_c > 0.0 && worst_pdf < 1e-6, name);
    }
}

static void test_cosine_orientation(void)
{
    rsx_pt_vec3 normals[] = {
        v3(1, 0, 0),           v3(0, 1, 0),          v3(0, 0, -1),
        v3(-1, -1, -1),        v3(3, -4, 12),        v3(1e-20f, -2e-20f, 3e-20f),
        v3(1e30f, -1e30f, 1e30f), v3(FLT_MAX, 0, 0), v3(0, -0.0f, -1e-38f),
        v3(1e-6f, 0, -1),      v3(-1e-7f, 1e-7f, -1)};
    const float us[][2] = {{0.0f, 0.0f}, {0.5f, 0.5f}, {0.9f, 0.1f}, {0.99f, 0.77f}};
    bool ok = true;
    for (unsigned k = 0; k < sizeof(normals) / sizeof(normals[0]); ++k) {
        double n[3];
        unit_ref(normals[k], n);
        for (unsigned j = 0; j < sizeof(us) / sizeof(us[0]); ++j) {
            rsx_pt_vec3 d;
            float pdf;
            if (!rsx_pt_cosine_sample(normals[k], us[j][0], us[j][1], &d, &pdf)) {
                ok = false;
                continue;
            }
            double c = dot_ref(d, n);
            if (!near(len3(d), 1.0, 1e-5) || !(c > 0.0) || !near(pdf, c / TEST_PI, 1e-6))
                ok = false;
        }
    }
    check(ok, "cosine: arbitrary/extreme orientations give unit directions in the hemisphere");

    rsx_pt_vec3 d;
    float pdf;
    float top = nextafterf(1.0f, 0.0f);
    bool lim = rsx_pt_cosine_sample(v3(0, 1, 0), top, top, &d, &pdf);
    check(lim && pdf > 0.0f && d.y > 0.0f && near(len3(d), 1.0, 1e-5),
          "cosine: largest u1,u2 below 1 still valid with positive pdf");
}

static bool cosine_fails_reset(rsx_pt_vec3 n, float u1, float u2)
{
    rsx_pt_vec3 d = v3(9, 9, 9);
    float pdf = 9.0f;
    bool ok = rsx_pt_cosine_sample(n, u1, u2, &d, &pdf);
    return !ok && is_zero3(d) && pdf == 0.0f;
}

static void test_cosine_invalid(void)
{
    check(cosine_fails_reset(v3(NAN, 0, 1), 0.5f, 0.5f), "cosine invalid: NaN normal");
    check(cosine_fails_reset(v3(0, INFINITY, 0), 0.5f, 0.5f), "cosine invalid: Inf normal");
    check(cosine_fails_reset(v3(0, 0, -INFINITY), 0.5f, 0.5f), "cosine invalid: -Inf normal");
    check(cosine_fails_reset(v3(0, 0, 0), 0.5f, 0.5f), "cosine invalid: zero normal");
    check(cosine_fails_reset(v3(-0.0f, 0, -0.0f), 0.5f, 0.5f), "cosine invalid: negative-zero normal");
    check(cosine_fails_reset(v3(0, 0, 1), 1.0f, 0.5f), "cosine invalid: u1 == 1");
    check(cosine_fails_reset(v3(0, 0, 1), -1e-7f, 0.5f), "cosine invalid: u1 < 0");
    check(cosine_fails_reset(v3(0, 0, 1), NAN, 0.5f), "cosine invalid: u1 NaN");
    check(cosine_fails_reset(v3(0, 0, 1), INFINITY, 0.5f), "cosine invalid: u1 Inf");
    check(cosine_fails_reset(v3(0, 0, 1), 0.5f, 1.0f), "cosine invalid: u2 == 1");
    check(cosine_fails_reset(v3(0, 0, 1), 0.5f, -0.5f), "cosine invalid: u2 < 0");
    check(cosine_fails_reset(v3(0, 0, 1), 0.5f, NAN), "cosine invalid: u2 NaN");

    rsx_pt_vec3 d = v3(9, 9, 9);
    bool ok = rsx_pt_cosine_sample(v3(0, 0, 1), 0.5f, 0.5f, &d, NULL);
    check(!ok && is_zero3(d), "cosine invalid: NULL pdf fails and zeroes direction");
    float pdf = 9.0f;
    ok = rsx_pt_cosine_sample(v3(0, 0, 1), 0.5f, 0.5f, NULL, &pdf);
    check(!ok && pdf == 0.0f, "cosine invalid: NULL direction fails and zeroes pdf");
    check(!rsx_pt_cosine_sample(v3(0, 0, 1), 0.5f, 0.5f, NULL, NULL),
          "cosine invalid: both outputs NULL");

    d = v3(9, 9, 9);
    pdf = 9.0f;
    ok = rsx_pt_cosine_sample(v3(0, 0, 1), 0.5f, 0.5f, &d, &pdf);
    ok = ok && !rsx_pt_cosine_sample(v3(NAN, 0, 0), 0.5f, 0.5f, &d, &pdf);
    check(ok && is_zero3(d) && pdf == 0.0f, "cosine: failure after success resets outputs");
}

static bool diffuse_fails_reset(rsx_pt_vec3 t, rsx_pt_vec3 a)
{
    rsx_pt_vec3 n = v3(9, 9, 9);
    return !rsx_pt_diffuse_step(t, a, &n) && is_zero3(n);
}

static void test_diffuse(void)
{
    rsx_pt_vec3 n;
    bool ok = rsx_pt_diffuse_step(v3(1, 0.5f, 0.25f), v3(0.5f, 1, 0), &n);
    check(ok && near3(n, v3(0.5f, 0.5f, 0.0f), 0.0), "diffuse: componentwise product");

    ok = rsx_pt_diffuse_step(v3(0, 0, 0), v3(1, 1, 1), &n);
    check(ok && is_zero3(n), "diffuse: zero throughput valid");

    ok = rsx_pt_diffuse_step(v3(1e30f, 2, 3), v3(1, 1, 1), &n);
    check(ok && near3(n, v3(1e30f, 2, 3), 0.0), "diffuse: unit albedo preserves throughput");

    /* BRDF/pdf cancellation: (albedo/pi)*cos/pdf == albedo for a real sample. */
    rsx_pt_vec3 d;
    float pdf;
    rsx_pt_vec3 t = v3(0.8f, 0.6f, 0.4f), a = v3(0.7f, 0.3f, 0.9f), nrm = v3(1, -2, 0.5f);
    double un[3];
    unit_ref(nrm, un);
    ok = rsx_pt_cosine_sample(nrm, 0.37f, 0.81f, &d, &pdf) && rsx_pt_diffuse_step(t, a, &n);
    double w = dot_ref(d, un) / TEST_PI / (double)pdf;
    check(ok && near(n.x, t.x * a.x * w, 1e-5) && near(n.y, t.y * a.y * w, 1e-5) &&
              near(n.z, t.z * a.z * w, 1e-5),
          "diffuse: matches explicit Lambertian BRDF*cos/pdf estimate");

    check(diffuse_fails_reset(v3(1, 1, 1), v3(1.1f, 0, 0)), "diffuse invalid: albedo > 1");
    check(diffuse_fails_reset(v3(1, 1, 1), v3(0, -0.1f, 0)), "diffuse invalid: albedo < 0");
    check(diffuse_fails_reset(v3(1, 1, 1), v3(0, 0, NAN)), "diffuse invalid: albedo NaN");
    check(diffuse_fails_reset(v3(1, 1, 1), v3(INFINITY, 0, 0)), "diffuse invalid: albedo Inf");
    check(diffuse_fails_reset(v3(-1, 1, 1), v3(0.5f, 0.5f, 0.5f)), "diffuse invalid: negative throughput");
    check(diffuse_fails_reset(v3(1, INFINITY, 1), v3(0.5f, 0.5f, 0.5f)), "diffuse invalid: Inf throughput");
    check(diffuse_fails_reset(v3(1, 1, NAN), v3(0.5f, 0.5f, 0.5f)), "diffuse invalid: NaN throughput");
    check(!rsx_pt_diffuse_step(v3(1, 1, 1), v3(1, 1, 1), NULL), "diffuse invalid: NULL output");
}

static bool rr_fails_reset(rsx_pt_vec3 t, unsigned bounce, float r)
{
    rsx_pt_vec3 n = v3(9, 9, 9);
    bool alive = true;
    return !rsx_pt_rr(t, bounce, r, &n, &alive) && is_zero3(n) && !alive;
}

static void test_rr_analytic(void)
{
    rsx_pt_vec3 n;
    bool alive;
    bool ok = true;
    for (unsigned b = 0; b < RSX_PT_RR_START_BOUNCE; ++b) {
        alive = false;
        if (!rsx_pt_rr(v3(0.01f, 0.02f, 0.03f), b, 0.999f, &n, &alive) || !alive ||
            !near3(n, v3(0.01f, 0.02f, 0.03f), 0.0))
            ok = false;
    }
    check(ok, "rr: bounces 0..2 preserve throughput and stay alive");

    ok = rsx_pt_rr(v3(FLT_MAX, 1, 1), 2, 0.5f, &n, &alive);
    check(ok && alive && n.x == FLT_MAX, "rr: huge throughput preserved before bounce 3");

    ok = rsx_pt_rr(v3(0.5f, 0.2f, 0.1f), 3, 0.49f, &n, &alive);
    check(ok && alive && near3(n, v3(1.0f, 0.4f, 0.2f), 1e-6), "rr: p=0.5 survivor divided by p");

    n = v3(9, 9, 9);
    ok = rsx_pt_rr(v3(0.5f, 0.2f, 0.1f), 3, 0.5f, &n, &alive);
    check(ok && !alive && is_zero3(n), "rr: random == p dies");

    ok = rsx_pt_rr(v3(2, 2, 2), 10, 0.94f, &n, &alive);
    check(ok && alive && near(n.x, 2.0 / 0.95, 1e-5), "rr: probability clamps to 0.95 (survive)");
    ok = rsx_pt_rr(v3(2, 2, 2), 10, 0.96f, &n, &alive);
    check(ok && !alive && is_zero3(n), "rr: probability clamps to 0.95 (die)");

    ok = rsx_pt_rr(v3(0.01f, 0, 0), 4, 0.04f, &n, &alive);
    check(ok && alive && near(n.x, 0.01 / 0.05, 1e-5) && n.y == 0.0f, "rr: probability clamps to 0.05 (survive)");
    ok = rsx_pt_rr(v3(0.01f, 0, 0), 4, 0.06f, &n, &alive);
    check(ok && !alive && is_zero3(n), "rr: probability clamps to 0.05 (die)");

    alive = true;
    n = v3(9, 9, 9);
    ok = rsx_pt_rr(v3(0, 0, 0), 0, 0.0f, &n, &alive);
    check(ok && !alive && is_zero3(n), "rr: zero throughput dead before bounce 3");
    alive = true;
    ok = rsx_pt_rr(v3(-0.0f, 0, 0), 5, 0.0f, &n, &alive);
    check(ok && !alive && is_zero3(n), "rr: zero throughput dead after bounce 3");
}

static void test_rr_energy(void)
{
    struct {
        rsx_pt_vec3 t;
        unsigned bounce;
    } cases[] = {{{0.5f, 0.2f, 0.1f}, 3}, {{0.01f, 0.03f, 0.02f}, 7}, {{3, 1, 0.5f}, 4},
                 {{0.3f, 0.3f, 0.3f}, 100}};
    const int count = 200000;
    uint32_t seed = 0xBADC0DEu;
    char name[160];

    for (unsigned k = 0; k < sizeof(cases) / sizeof(cases[0]); ++k) {
        rsx_pt_vec3 t = cases[k].t;
        float m = fmaxf(t.x, fmaxf(t.y, t.z));
        double p = fmin(fmax(m, 0.05), 0.95);
        double sum[3] = {0.0, 0.0, 0.0};
        int survived = 0;
        bool consistent = true;
        for (int i = 0; i < count; ++i) {
            rsx_pt_vec3 n;
            bool alive;
            if (!rsx_pt_rr(t, cases[k].bounce, rng_float(&seed), &n, &alive)) {
                consistent = false;
                continue;
            }
            if (alive) {
                ++survived;
                if (!near(n.x, t.x / p, 1e-5 * (1.0 + t.x / p)))
                    consistent = false;
            } else if (!is_zero3(n)) {
                consistent = false;
            }
            sum[0] += n.x;
            sum[1] += n.y;
            sum[2] += n.z;
        }
        double frac = (double)survived / count;
        double frac_tol = 6.0 * sqrt(p * (1.0 - p) / count);
        const float tc[3] = {t.x, t.y, t.z};
        bool energy = true;
        for (int c = 0; c < 3; ++c) {
            double mean = sum[c] / count;
            double sd = tc[c] * sqrt(1.0 / p - 1.0);
            if (!near(mean, tc[c], 6.0 * sd / sqrt((double)count) + 1e-7))
                energy = false;
        }
        snprintf(name, sizeof(name), "rr MC case %u: samples consistent (dead zero, alive t/p)", k);
        check(consistent, name);
        snprintf(name, sizeof(name), "rr MC case %u: survival %.4f ~ p %.4f", k, frac, p);
        check(near(frac, p, frac_tol), name);
        snprintf(name, sizeof(name), "rr MC case %u: E[next] matches throughput (unbiased)", k);
        check(energy, name);
    }
}

static void test_rr_invalid(void)
{
    check(rr_fails_reset(v3(NAN, 0, 0), 3, 0.1f), "rr invalid: NaN throughput");
    check(rr_fails_reset(v3(0, -INFINITY, 0), 0, 0.1f), "rr invalid: -Inf throughput");
    check(rr_fails_reset(v3(0, 0, INFINITY), 5, 0.1f), "rr invalid: Inf throughput");
    check(rr_fails_reset(v3(0.5f, -0.1f, 0), 3, 0.1f), "rr invalid: negative throughput");
    check(rr_fails_reset(v3(0.5f, 0.5f, 0.5f), 3, NAN), "rr invalid: random NaN");
    check(rr_fails_reset(v3(0.5f, 0.5f, 0.5f), 3, 1.0f), "rr invalid: random == 1");
    check(rr_fails_reset(v3(0.5f, 0.5f, 0.5f), 0, -0.01f), "rr invalid: random < 0");
    check(rr_fails_reset(v3(0.5f, 0.5f, 0.5f), 3, INFINITY), "rr invalid: random Inf");
    check(rr_fails_reset(v3(FLT_MAX, 0, 0), 3, 0.0f), "rr invalid: survivor overflow");

    bool alive = true;
    check(!rsx_pt_rr(v3(1, 1, 1), 0, 0.5f, NULL, &alive) && !alive,
          "rr invalid: NULL next fails and clears alive");
    rsx_pt_vec3 n = v3(9, 9, 9);
    check(!rsx_pt_rr(v3(1, 1, 1), 0, 0.5f, &n, NULL) && is_zero3(n),
          "rr invalid: NULL alive fails and zeroes next");
}

static bool offset_fails_reset(rsx_pt_vec3 p, rsx_pt_vec3 n, float e)
{
    rsx_pt_vec3 o = v3(9, 9, 9);
    return !rsx_pt_offset(p, n, e, &o) && is_zero3(o);
}

static void test_offset(void)
{
    rsx_pt_vec3 o;
    bool ok = rsx_pt_offset(v3(1, 2, 3), v3(0, 0, 2), 0.5f, &o);
    check(ok && near3(o, v3(1, 2, 3.5f), 1e-6), "offset: axis normal");

    ok = rsx_pt_offset(v3(0, 0, 0), v3(3, 4, 0), 1.0f, &o);
    check(ok && near3(o, v3(0.6f, 0.8f, 0), 1e-6), "offset: normalizes arbitrary normal");

    ok = rsx_pt_offset(v3(-1, 1, 0), v3(1e-30f, -1e-30f, 0), 0.001f, &o);
    check(ok && near3(o, v3(-1.0f + 0.000707107f, 1.0f - 0.000707107f, 0), 1e-6),
          "offset: tiny normal still normalized");

    ok = rsx_pt_offset(v3(5, 5, 5), v3(1e30f, 1e30f, 1e30f), 0.1f, &o);
    check(ok && near(len3(v3(o.x - 5, o.y - 5, o.z - 5)), 0.1, 1e-5), "offset: huge normal normalized");

    check(offset_fails_reset(v3(0, 0, 0), v3(0, 0, 1), 0.0f), "offset invalid: epsilon zero");
    check(offset_fails_reset(v3(0, 0, 0), v3(0, 0, 1), -1.0f), "offset invalid: negative epsilon");
    check(offset_fails_reset(v3(0, 0, 0), v3(0, 0, 1), NAN), "offset invalid: epsilon NaN");
    check(offset_fails_reset(v3(0, 0, 0), v3(0, 0, 1), INFINITY), "offset invalid: epsilon Inf");
    check(offset_fails_reset(v3(0, 0, 0), v3(0, 0, 0), 1.0f), "offset invalid: zero normal");
    check(offset_fails_reset(v3(0, 0, 0), v3(NAN, 0, 1), 1.0f), "offset invalid: NaN normal");
    check(offset_fails_reset(v3(0, 0, 0), v3(INFINITY, 0, 1), 1.0f), "offset invalid: Inf normal");
    check(offset_fails_reset(v3(INFINITY, 0, 0), v3(0, 0, 1), 1.0f), "offset invalid: Inf point");
    check(offset_fails_reset(v3(0, NAN, 0), v3(0, 0, 1), 1.0f), "offset invalid: NaN point");
    check(offset_fails_reset(v3(FLT_MAX, 0, 0), v3(1, 0, 0), FLT_MAX), "offset invalid: result overflow");
    check(!rsx_pt_offset(v3(0, 0, 0), v3(0, 0, 1), 1.0f, NULL), "offset invalid: NULL output");
}

int main(void)
{
    test_cosine_analytic();
    test_cosine_moments();
    test_cosine_orientation();
    test_cosine_invalid();
    test_diffuse();
    test_rr_analytic();
    test_rr_energy();
    test_rr_invalid();
    test_offset();

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
