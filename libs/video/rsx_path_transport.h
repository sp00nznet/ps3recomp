#ifndef RSX_PATH_TRANSPORT_H
#define RSX_PATH_TRANSPORT_H

/*
 * Foundational multibounce diffuse path transport math.
 *
 * Pure C17, header-only, no allocations and no global state. The matching
 * GLSL include (rsx_path_transport.glsl) exposes the same function names with
 * out parameters. Every function returns false on invalid input or output and
 * then leaves all writable outputs zeroed (alive == false).
 */

#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct rsx_pt_vec3 {
    float x, y, z;
} rsx_pt_vec3;

#define RSX_PT_PI 3.14159265358979323846f
#define RSX_PT_PI_D 3.14159265358979323846
#define RSX_PT_RR_START_BOUNCE 3u
#define RSX_PT_RR_MIN_P 0.05f
#define RSX_PT_RR_MAX_P 0.95f

static inline rsx_pt_vec3 rsx_pt_internal_zero3(void)
{
    rsx_pt_vec3 z = {0.0f, 0.0f, 0.0f};
    return z;
}

static inline bool rsx_pt_internal_finite3(rsx_pt_vec3 v)
{
    return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
}

/* Normalizes in double so tiny or huge finite floats neither underflow nor overflow. */
static inline bool rsx_pt_internal_unit(rsx_pt_vec3 v, double out[3])
{
    out[0] = 0.0;
    out[1] = 0.0;
    out[2] = 0.0;
    if (!rsx_pt_internal_finite3(v))
        return false;
    double x = (double)v.x, y = (double)v.y, z = (double)v.z;
    double len2 = x * x + y * y + z * z;
    if (!isfinite(len2) || !(len2 > 0.0))
        return false;
    double inv = 1.0 / sqrt(len2);
    if (!isfinite(inv))
        return false;
    out[0] = x * inv;
    out[1] = y * inv;
    out[2] = z * inv;
    return true;
}

/* Range-checked double to float conversion (out-of-range casts are undefined in C). */
static inline bool rsx_pt_internal_to_float(double d, float* f)
{
    *f = 0.0f;
    if (!isfinite(d) || fabs(d) > (double)FLT_MAX)
        return false;
    *f = (float)d;
    return isfinite(*f);
}

/*
 * Cosine-weighted hemisphere sample around normal (any finite nonzero vector).
 * u1, u2 must lie in [0,1). Writes a unit direction and pdf = cos(theta)/pi.
 */
static inline bool rsx_pt_cosine_sample(rsx_pt_vec3 normal, float u1, float u2,
                                        rsx_pt_vec3* direction, float* pdf)
{
    if (direction)
        *direction = rsx_pt_internal_zero3();
    if (pdf)
        *pdf = 0.0f;
    if (!direction || !pdf)
        return false;
    if (!isfinite(u1) || !isfinite(u2))
        return false;
    if (u1 < 0.0f || u1 >= 1.0f || u2 < 0.0f || u2 >= 1.0f)
        return false;

    double n[3];
    if (!rsx_pt_internal_unit(normal, n))
        return false;

    /* Duff et al. 2017 branchless orthonormal basis; sign + n.z is never near zero. */
    double s = n[2] >= 0.0 ? 1.0 : -1.0;
    double a = -1.0 / (s + n[2]);
    double b = n[0] * n[1] * a;
    double t[3] = {1.0 + s * n[0] * n[0] * a, s * b, -s * n[0]};
    double bt[3] = {b, s + n[1] * n[1] * a, -n[1]};

    double r = sqrt((double)u1);
    double phi = 2.0 * RSX_PT_PI_D * (double)u2;
    double lx = r * cos(phi);
    double ly = r * sin(phi);
    double lz = sqrt(1.0 - (double)u1);

    double d[3];
    for (int i = 0; i < 3; ++i)
        d[i] = t[i] * lx + bt[i] * ly + n[i] * lz;
    double len = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!isfinite(len) || !(len > 0.0))
        return false;
    for (int i = 0; i < 3; ++i)
        d[i] /= len;

    double c = d[0] * n[0] + d[1] * n[1] + d[2] * n[2];
    rsx_pt_vec3 dir = {(float)d[0], (float)d[1], (float)d[2]};
    float p = (float)(c / RSX_PT_PI_D);
    if (!rsx_pt_internal_finite3(dir) || !isfinite(p) || !(p > 0.0f))
        return false;

    *direction = dir;
    *pdf = p;
    return true;
}

/*
 * Lambertian bounce with cosine sampling: (albedo/pi) * cos / (cos/pi) == albedo,
 * so throughput is updated componentwise by albedo. Throughput must be finite and
 * >= 0, albedo in [0,1].
 */
static inline bool rsx_pt_diffuse_step(rsx_pt_vec3 throughput, rsx_pt_vec3 albedo,
                                       rsx_pt_vec3* next)
{
    if (!next)
        return false;
    *next = rsx_pt_internal_zero3();
    if (!rsx_pt_internal_finite3(throughput) || !rsx_pt_internal_finite3(albedo))
        return false;
    if (throughput.x < 0.0f || throughput.y < 0.0f || throughput.z < 0.0f)
        return false;
    if (albedo.x < 0.0f || albedo.y < 0.0f || albedo.z < 0.0f)
        return false;
    if (albedo.x > 1.0f || albedo.y > 1.0f || albedo.z > 1.0f)
        return false;

    rsx_pt_vec3 r = {throughput.x * albedo.x, throughput.y * albedo.y,
                     throughput.z * albedo.z};
    if (!rsx_pt_internal_finite3(r))
        return false;
    *next = r;
    return true;
}

/*
 * Russian roulette. Before RSX_PT_RR_START_BOUNCE the throughput is preserved.
 * Afterwards the survival probability is clamp(maxComponent, 0.05, 0.95); the path
 * dies when random >= p and survivors are divided by p (unbiased). Zero throughput
 * is a valid terminated path (returns true, alive false). random must be in [0,1).
 */
static inline bool rsx_pt_rr(rsx_pt_vec3 throughput, unsigned bounce, float random,
                             rsx_pt_vec3* next, bool* alive)
{
    if (next)
        *next = rsx_pt_internal_zero3();
    if (alive)
        *alive = false;
    if (!next || !alive)
        return false;
    if (!rsx_pt_internal_finite3(throughput))
        return false;
    if (throughput.x < 0.0f || throughput.y < 0.0f || throughput.z < 0.0f)
        return false;
    if (!isfinite(random) || random < 0.0f || random >= 1.0f)
        return false;

    float m = fmaxf(throughput.x, fmaxf(throughput.y, throughput.z));
    if (!(m > 0.0f))
        return true;

    if (bounce < RSX_PT_RR_START_BOUNCE) {
        *next = throughput;
        *alive = true;
        return true;
    }

    float p = fminf(fmaxf(m, RSX_PT_RR_MIN_P), RSX_PT_RR_MAX_P);
    if (random >= p)
        return true;

    double q = (double)p;
    rsx_pt_vec3 r;
    if (!rsx_pt_internal_to_float((double)throughput.x / q, &r.x) ||
        !rsx_pt_internal_to_float((double)throughput.y / q, &r.y) ||
        !rsx_pt_internal_to_float((double)throughput.z / q, &r.z))
        return false;
    *next = r;
    *alive = true;
    return true;
}

/* Ray origin offset: point + normalize(normal) * epsilon, epsilon finite and > 0. */
static inline bool rsx_pt_offset(rsx_pt_vec3 point, rsx_pt_vec3 normal, float epsilon,
                                 rsx_pt_vec3* origin)
{
    if (!origin)
        return false;
    *origin = rsx_pt_internal_zero3();
    if (!rsx_pt_internal_finite3(point))
        return false;
    if (!isfinite(epsilon) || !(epsilon > 0.0f))
        return false;
    double n[3];
    if (!rsx_pt_internal_unit(normal, n))
        return false;

    double e = (double)epsilon;
    rsx_pt_vec3 r;
    if (!rsx_pt_internal_to_float((double)point.x + n[0] * e, &r.x) ||
        !rsx_pt_internal_to_float((double)point.y + n[1] * e, &r.y) ||
        !rsx_pt_internal_to_float((double)point.z + n[2] * e, &r.z))
        return false;
    *origin = r;
    return true;
}

#endif /* RSX_PATH_TRANSPORT_H */
