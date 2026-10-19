/* Independent CPU tests for rsx_light_space.h. */
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../libs/video/rsx_light_space.h"

/* C17 does not implicitly add const to nested array element types. */
#define CM(m) ((const float (*)[4])(m))

static int checks;
static int failures;

#define CHECK(cond)                                                                \
    do {                                                                           \
        ++checks;                                                                  \
        if (!(cond)) {                                                             \
            ++failures;                                                            \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                          \
    } while (0)

static void affine_identity(double m[3][4])
{
    int i, j;
    for (i = 0; i < 3; ++i)
        for (j = 0; j < 4; ++j)
            m[i][j] = i == j ? 1.0 : 0.0;
}

static void affine_mul(double a[3][4], double b[3][4], double o[3][4])
{
    double t[3][4];
    int i, j;
    for (i = 0; i < 3; ++i)
        for (j = 0; j < 4; ++j)
            t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] +
                      (j == 3 ? a[i][3] : 0.0);
    memcpy(o, t, sizeof t);
}

/* Left-multiplies m by a rotation about axis 0 (x) or 2 (z). */
static void rotate(double m[3][4], int axis, double angle)
{
    double r[3][4];
    int a = (axis + 1) % 3, b = (axis + 2) % 3;
    affine_identity(r);
    r[a][a] = cos(angle);
    r[a][b] = -sin(angle);
    r[b][a] = sin(angle);
    r[b][b] = cos(angle);
    affine_mul(r, m, m);
}

/* Left-multiplies m by diag(sx, sy, sz). */
static void scale(double m[3][4], double sx, double sy, double sz)
{
    int j;
    for (j = 0; j < 4; ++j) {
        m[0][j] *= sx;
        m[1][j] *= sy;
        m[2][j] *= sz;
    }
}

static void translate(double m[3][4], double x, double y, double z)
{
    m[0][3] += x;
    m[1][3] += y;
    m[2][3] += z;
}

static void rigid_inverse(double m[3][4], double o[3][4])
{
    double t[3][4];
    int i, j;
    for (i = 0; i < 3; ++i) {
        for (j = 0; j < 3; ++j)
            t[i][j] = m[j][i];
        t[i][3] = -(m[0][i] * m[0][3] + m[1][i] * m[1][3] + m[2][i] * m[2][3]);
    }
    memcpy(o, t, sizeof t);
}

static void to_float(double d[3][4], float f[3][4])
{
    int i, j;
    for (i = 0; i < 3; ++i)
        for (j = 0; j < 4; ++j)
            f[i][j] = (float)d[i][j];
}

static int near(double a, double b, double tol)
{
    return fabs(a - b) <= tol * (1.0 + fabs(b));
}

static int matrix_near(float m[3][4], double e[3][4], double rtol, double ttol)
{
    int i, j;
    for (i = 0; i < 3; ++i)
        for (j = 0; j < 4; ++j)
            if (!near(m[i][j], e[i][j], j == 3 ? ttol : rtol))
                return 0;
    return 1;
}

static void fill(float *v, int n, float value)
{
    int i;
    for (i = 0; i < n; ++i)
        v[i] = value;
}

static void world_to_light(double m[3][4])
{
    affine_identity(m);
    rotate(m, 0, 0.3);
    rotate(m, 2, -0.9);
    translate(m, 4.0, -5.0, 6.0);
}

/* Includes a reflection, so the expected quotient has determinant -1. */
static void world_to_view(double m[3][4])
{
    affine_identity(m);
    scale(m, 1.0, -1.0, 1.0);
    rotate(m, 2, 1.1);
    rotate(m, 0, 0.25);
    translate(m, -2.0, 0.5, -7.0);
}

static void expected_light_to_view(double e[3][4])
{
    double wl[3][4], wv[3][4];
    world_to_light(wl);
    world_to_view(wv);
    rigid_inverse(wl, wl);
    affine_mul(wv, wl, e);
}

static void model_pair(double model[3][4], float l[3][4], float v[3][4])
{
    double w[3][4], t[3][4];
    world_to_light(w);
    affine_mul(w, model, t);
    to_float(t, l);
    world_to_view(w);
    affine_mul(w, model, t);
    to_float(t, v);
}

static float identity[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};

static void test_identity(void)
{
    float out[3][4];
    int i, j, same = 1;
    CHECK(rsx_light_space_build(CM(identity), CM(identity), out));
    for (i = 0; i < 3; ++i)
        for (j = 0; j < 4; ++j)
            same &= out[i][j] == identity[i][j];
    CHECK(same);
}

static void test_known_world_to_view(void)
{
    double rz[3][4] = {{0, -1, 0, 1}, {1, 0, 0, 2}, {0, 0, 1, 3}};
    double refl[3][4] = {{1, 0, 0, -4}, {0, -1, 0, 5}, {0, 0, 1, 0.5}};
    double wl[3][4], wv[3][4], e[3][4];
    float l[3][4], v[3][4], out[3][4];

    to_float(rz, v);
    CHECK(rsx_light_space_build(CM(identity), CM(v), out));
    CHECK(matrix_near(out, rz, 1e-7, 1e-7));

    to_float(refl, v);
    CHECK(rsx_light_space_build(CM(identity), CM(v), out));
    CHECK(matrix_near(out, refl, 1e-7, 1e-7));

    world_to_light(wl);
    world_to_view(wv);
    expected_light_to_view(e);
    to_float(wl, l);
    to_float(wv, v);
    CHECK(rsx_light_space_build(CM(l), CM(v), out));
    CHECK(matrix_near(out, e, 1e-5, 1e-5));
}

static void test_common_model(void)
{
    double m[3][4], e[3][4];
    float l[3][4], v[3][4], out[3][4];

    affine_identity(m);
    scale(m, 0.5, 3.0, 7.0);
    rotate(m, 0, -0.4);
    rotate(m, 2, 0.7);
    translate(m, 10.0, -20.0, 5.0);
    model_pair(m, l, v);
    expected_light_to_view(e);
    CHECK(rsx_light_space_build(CM(l), CM(v), out));
    CHECK(matrix_near(out, e, 1e-5, 1e-5));
}

static void test_matched_scales(void)
{
    static const double scales[] = {1e-4, 1e4};
    double m[3][4], e[3][4];
    float l[3][4], v[3][4], out[3][4];
    int k;

    expected_light_to_view(e);
    for (k = 0; k < 2; ++k) {
        double s = scales[k];
        affine_identity(m);
        scale(m, s, s, s);
        rotate(m, 2, 0.7);
        translate(m, 3.0 * s, -2.0 * s, s);
        model_pair(m, l, v);
        CHECK(rsx_light_space_build(CM(l), CM(v), out));
        CHECK(matrix_near(out, e, 1e-5, 1e-6 * (100.0 + 10.0 * s)));

        affine_identity(m);
        scale(m, s, 2.0 * s, 0.5 * s);
        rotate(m, 0, 1.3);
        model_pair(m, l, v);
        CHECK(rsx_light_space_build(CM(l), CM(v), out));
        CHECK(matrix_near(out, e, 1e-5, 1e-6 * (100.0 + 10.0 * s)));
    }
}

static void test_singular(void)
{
    float zero_row[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 0, 0}};
    float rank2[3][4] = {{1, 2, 3, 0}, {2, 4, 6, 0}, {0, 0, 1, 0}};
    float thin[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1e-12f, 0}};
    float out[3][4], keep[3][4];

    fill(&out[0][0], 12, 42.0f);
    memcpy(keep, out, sizeof out);
    CHECK(!rsx_light_space_build(CM(zero_row), CM(identity), out));
    CHECK(!rsx_light_space_build(CM(rank2), CM(identity), out));
    /* Quotient would be the identity, but the inverse exceeds the bound. */
    CHECK(!rsx_light_space_build(CM(thin), CM(thin), out));
    CHECK(memcmp(out, keep, sizeof out) == 0);
}

static void test_quotient_rejection(void)
{
    float twice[3][4] = {{2, 0, 0, 0}, {0, 2, 0, 0}, {0, 0, 2, 0}};
    float shear[3][4] = {{1, 0.1f, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    float stretch[3][4] = {{1.001f, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    float slight[3][4] = {{1.000001f, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    float out[3][4], keep[3][4];

    fill(&out[0][0], 12, 42.0f);
    memcpy(keep, out, sizeof out);
    CHECK(!rsx_light_space_build(CM(identity), CM(twice), out));
    CHECK(!rsx_light_space_build(CM(twice), CM(identity), out));
    CHECK(!rsx_light_space_build(CM(identity), CM(shear), out));
    CHECK(!rsx_light_space_build(CM(identity), CM(stretch), out));
    CHECK(memcmp(out, keep, sizeof out) == 0);
    CHECK(rsx_light_space_build(CM(identity), CM(slight), out));
}

static void test_build_invalid(void)
{
    float bad[3][4], l[3][4], v[3][4], out[3][4], keep[3][4];

    fill(&out[0][0], 12, 42.0f);
    memcpy(keep, out, sizeof out);
    memcpy(bad, identity, sizeof bad);
    bad[1][2] = NAN;
    CHECK(!rsx_light_space_build(CM(bad), CM(identity), out));
    CHECK(!rsx_light_space_build(CM(identity), CM(bad), out));
    bad[1][2] = INFINITY;
    CHECK(!rsx_light_space_build(CM(bad), CM(identity), out));
    CHECK(!rsx_light_space_build(CM(identity), CM(bad), out));
    CHECK(!rsx_light_space_build(NULL, CM(identity), out));
    CHECK(!rsx_light_space_build(CM(identity), NULL, out));
    CHECK(!rsx_light_space_build(CM(identity), CM(identity), NULL));

    memcpy(l, identity, sizeof l);
    memcpy(v, identity, sizeof v);
    l[0][3] = -3e38f;
    v[0][3] = 3e38f;
    CHECK(!rsx_light_space_build(CM(l), CM(v), out));
    CHECK(memcmp(out, keep, sizeof out) == 0);
}

static void test_build_alias(void)
{
    double wl[3][4], wv[3][4];
    float l[3][4], v[3][4], ref[3][4], a[3][4], keep[3][4];
    float sing[3][4] = {{1, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 1, 0}};

    world_to_light(wl);
    world_to_view(wv);
    to_float(wl, l);
    to_float(wv, v);
    CHECK(rsx_light_space_build(CM(l), CM(v), ref));

    memcpy(a, v, sizeof a);
    CHECK(rsx_light_space_build(CM(l), CM(a), a));
    CHECK(memcmp(a, ref, sizeof a) == 0);
    memcpy(a, l, sizeof a);
    CHECK(rsx_light_space_build(CM(a), CM(v), a));
    CHECK(memcmp(a, ref, sizeof a) == 0);

    memcpy(keep, sing, sizeof keep);
    CHECK(!rsx_light_space_build(CM(sing), CM(v), sing));
    CHECK(memcmp(sing, keep, sizeof sing) == 0);
}

static void test_homogeneous_basic(void)
{
    float m[3][4] = {{0, -1, 0, 1}, {1, 0, 0, 2}, {0, 0, 1, 3}};
    float p[4] = {1, 0, 0, 1}, d[4] = {1, 0, 0, 0}, h[4] = {1, 0, 0, 2.5f};
    float n[4] = {0, 0, 1, -1}, z[4] = {1, 0, 0, -0.0f}, out[4];

    CHECK(rsx_light_space_homogeneous(CM(m), p, out));
    CHECK(out[0] == 1 && out[1] == 3 && out[2] == 3 && out[3] == 1);
    CHECK(rsx_light_space_homogeneous(CM(m), d, out));
    CHECK(out[0] == 0 && out[1] == 1 && out[2] == 0 && out[3] == 0);
    CHECK(rsx_light_space_homogeneous(CM(m), h, out));
    CHECK(out[0] == 2.5f && out[1] == 6 && out[2] == 7.5f && out[3] == 2.5f);
    CHECK(rsx_light_space_homogeneous(CM(m), n, out));
    CHECK(out[0] == -1 && out[1] == -2 && out[2] == -2 && out[3] == -1);
    CHECK(rsx_light_space_homogeneous(CM(m), z, out));
    CHECK(out[3] == 0 && signbit(out[3]));
}

/* Source vector h.xyz - P*w must commute with the transform. */
static void test_homogeneous_field(void)
{
    float lights[3][4] = {{1.5f, -2, 3, 1}, {0.3f, 0.4f, -0.5f, 0}, {2, 4, -6, 2.5f}};
    float points[3][3] = {{0, 0, 0}, {1, -2, 0.5f}, {-3, 7, 2}};
    double m[3][4];
    float l[3][4], v[3][4], ltv[3][4];
    int a, b, i;

    affine_identity(m);
    scale(m, 0.5, 3.0, 7.0);
    rotate(m, 2, 0.7);
    translate(m, 10.0, -20.0, 5.0);
    model_pair(m, l, v);
    CHECK(rsx_light_space_build(CM(l), CM(v), ltv));

    for (a = 0; a < 3; ++a)
        for (b = 0; b < 3; ++b) {
            const float *h = lights[a], *p = points[b];
            float pl[4] = {p[0], p[1], p[2], 1}, sl[4], hv[4], pv[4], sv[4];
            int ok = 1;
            for (i = 0; i < 3; ++i)
                sl[i] = (float)(h[i] - (double)p[i] * h[3]);
            sl[3] = 0;
            CHECK(rsx_light_space_homogeneous(CM(ltv), h, hv));
            CHECK(rsx_light_space_homogeneous(CM(ltv), pl, pv));
            CHECK(rsx_light_space_homogeneous(CM(ltv), sl, sv));
            for (i = 0; i < 3; ++i) {
                double pw = (double)pv[i] * hv[3];
                ok &= fabs((hv[i] - pw) - sv[i]) <= 1e-5 * (1.0 + fabs(hv[i]) + fabs(pw));
            }
            CHECK(ok);
            CHECK(hv[3] == h[3] && sv[3] == 0 && pv[3] == 1);
        }
}

static void test_homogeneous_invalid(void)
{
    float twice[3][4] = {{2, 0, 0, 0}, {0, 2, 0, 0}, {0, 0, 2, 0}};
    float shear[3][4] = {{1, 0.1f, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    float big[3][4] = {{1, 0, 0, 3e38f}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    float m[3][4] = {{0, -1, 0, 1}, {1, 0, 0, 2}, {0, 0, 1, 3}};
    float nanm[3][4], src[4] = {1, 2, 3, 1}, nans[4] = {1, NAN, 3, 1};
    float infs[4] = {1, 2, 3, INFINITY}, bigs[4] = {3e38f, 0, 0, 1};
    float out[4], keep[4], buf[4], buf_keep[4];

    memcpy(nanm, m, sizeof nanm);
    nanm[2][3] = NAN;
    fill(out, 4, 42.0f);
    memcpy(keep, out, sizeof out);
    CHECK(!rsx_light_space_homogeneous(CM(twice), src, out));
    CHECK(!rsx_light_space_homogeneous(CM(shear), src, out));
    CHECK(!rsx_light_space_homogeneous(CM(nanm), src, out));
    CHECK(!rsx_light_space_homogeneous(CM(m), nans, out));
    CHECK(!rsx_light_space_homogeneous(CM(m), infs, out));
    CHECK(!rsx_light_space_homogeneous(CM(big), bigs, out));
    CHECK(!rsx_light_space_homogeneous(NULL, src, out));
    CHECK(!rsx_light_space_homogeneous(CM(m), NULL, out));
    CHECK(!rsx_light_space_homogeneous(CM(m), src, NULL));
    CHECK(memcmp(out, keep, sizeof out) == 0);

    buf[0] = 1;
    buf[1] = 0;
    buf[2] = 0;
    buf[3] = 2.5f;
    CHECK(rsx_light_space_homogeneous(CM(m), buf, buf));
    CHECK(buf[0] == 2.5f && buf[1] == 6 && buf[2] == 7.5f && buf[3] == 2.5f);
    CHECK(rsx_light_space_homogeneous(CM(m), src, m[0]));
    CHECK(m[0][0] == -1 && m[0][1] == 3 && m[0][2] == 6 && m[0][3] == 1);

    memcpy(buf, nans, sizeof buf);
    memcpy(buf_keep, buf, sizeof buf);
    CHECK(!rsx_light_space_homogeneous(CM(twice), buf, buf));
    CHECK(memcmp(buf, buf_keep, sizeof buf) == 0);
}

static void test_ray(void)
{
    float h[4] = {2, 4, 6, 2}, view[3] = {1, 2, 0}, dir[3], dist;
    float dl[4] = {3, 0, -4, 0}, g[4] = {1, -1, 2, 0.5f}, gv[3] = {-1, 2, 4};
    float large[4] = {3e38f, -3e38f, 0, 0}, origin[3] = {0, 0, 0};
    float tiny[4] = {2e-30f, 4e-30f, 6e-30f, 2e-30f}, buf[4];
    double r14 = sqrt(14.0);

    CHECK(rsx_light_space_ray(h, view, dir, &dist));
    CHECK(dir[0] == 0 && dir[1] == 0 && dir[2] == 1 && dist == 3);
    CHECK(rsx_light_space_ray(dl, view, dir, &dist));
    CHECK(near(dir[0], 0.6, 1e-7) && dir[1] == 0 && near(dir[2], -0.8, 1e-7));
    CHECK(isinf(dist) && dist > 0);
    CHECK(rsx_light_space_ray(g, gv, dir, &dist));
    CHECK(near(dir[0], 0.6, 1e-7) && near(dir[1], -0.8, 1e-7) && dir[2] == 0 && dist == 5);
    CHECK(rsx_light_space_ray(large, origin, dir, &dist));
    CHECK(near(dir[0], sqrt(0.5), 1e-7) && near(dir[1], -sqrt(0.5), 1e-7) && isinf(dist));
    CHECK(rsx_light_space_ray(tiny, origin, dir, &dist));
    CHECK(near(dist, r14, 1e-6) && near(dir[0], 1.0 / r14, 1e-6) &&
          near(dir[1], 2.0 / r14, 1e-6) && near(dir[2], 3.0 / r14, 1e-6));

    memcpy(buf, h, sizeof buf);
    CHECK(rsx_light_space_ray(buf, view, buf, &buf[3]));
    CHECK(buf[0] == 0 && buf[1] == 0 && buf[2] == 1 && buf[3] == 3);
    memcpy(buf, g, sizeof buf);
    CHECK(rsx_light_space_ray(buf, gv, gv, &buf[3]));
    CHECK(near(gv[0], 0.6, 1e-7) && near(gv[1], -0.8, 1e-7) && gv[2] == 0 && buf[3] == 5);
}

static void test_ray_invalid(void)
{
    float neg[4] = {1, 0, 0, -1}, pt[4] = {2, 4, 6, 2}, at[3] = {1, 2, 3};
    float zero[4] = {0, 0, 0, 0}, nzero[4] = {-0.0f, 0, 0, 0}, nanh[4] = {NAN, 0, 0, 1};
    float infh[4] = {INFINITY, 0, 0, 0}, view[3] = {0, 0, 0}, nanv[3] = {0, NAN, 0};
    float infv[3] = {0, 0, -INFINITY}, sub[4] = {1, 0, 0, FLT_TRUE_MIN};
    float far_pt[4] = {3e38f, 0, 0, 1}, far_view[3] = {-3e38f, 0, 0};
    float half_w[4] = {3e38f, 0, 0, 0.5f};
    float dir[3] = {7, 7, 7}, dist = 7, buf[4], keep[4];

    CHECK(!rsx_light_space_ray(neg, view, dir, &dist));
    CHECK(!rsx_light_space_ray(pt, at, dir, &dist));
    CHECK(!rsx_light_space_ray(zero, view, dir, &dist));
    CHECK(!rsx_light_space_ray(nzero, view, dir, &dist));
    CHECK(!rsx_light_space_ray(nanh, view, dir, &dist));
    CHECK(!rsx_light_space_ray(infh, view, dir, &dist));
    CHECK(!rsx_light_space_ray(pt, nanv, dir, &dist));
    CHECK(!rsx_light_space_ray(pt, infv, dir, &dist));
    CHECK(!rsx_light_space_ray(sub, view, dir, &dist));
    CHECK(!rsx_light_space_ray(far_pt, far_view, dir, &dist));
    CHECK(!rsx_light_space_ray(half_w, view, dir, &dist));
    CHECK(!rsx_light_space_ray(NULL, view, dir, &dist));
    CHECK(!rsx_light_space_ray(pt, NULL, dir, &dist));
    CHECK(!rsx_light_space_ray(pt, view, NULL, &dist));
    CHECK(!rsx_light_space_ray(pt, view, dir, NULL));
    CHECK(dir[0] == 7 && dir[1] == 7 && dir[2] == 7 && dist == 7);

    memcpy(buf, neg, sizeof buf);
    memcpy(keep, buf, sizeof buf);
    CHECK(!rsx_light_space_ray(buf, view, buf, &buf[3]));
    CHECK(memcmp(buf, keep, sizeof buf) == 0);
}

int main(void)
{
    test_identity();
    test_known_world_to_view();
    test_common_model();
    test_matched_scales();
    test_singular();
    test_quotient_rejection();
    test_build_invalid();
    test_build_alias();
    test_homogeneous_basic();
    test_homogeneous_field();
    test_homogeneous_invalid();
    test_ray();
    test_ray_invalid();
    printf("light_space: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
