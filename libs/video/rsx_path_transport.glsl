#ifndef RSX_PATH_TRANSPORT_GLSL
#define RSX_PATH_TRANSPORT_GLSL

// Foundational multibounce diffuse path transport math, mirroring
// rsx_path_transport.h. Include via GL_GOOGLE_include_directive. Every function
// returns false on invalid input/output and then leaves all outputs zeroed
// (alive == false). Finiteness uses isnan/isinf; avoid fast-math that folds them.

const float RSX_PT_PI = 3.14159265358979323846;
const uint RSX_PT_RR_START_BOUNCE = 3u;
const float RSX_PT_RR_MIN_P = 0.05;
const float RSX_PT_RR_MAX_P = 0.95;

bool rsx_pt_internal_finite(float v)
{
    return !(isnan(v) || isinf(v));
}

bool rsx_pt_internal_finite3(vec3 v)
{
    return !(any(isnan(v)) || any(isinf(v)));
}

// Prescales by the largest magnitude so dot() cannot overflow or underflow.
bool rsx_pt_internal_unit(vec3 v, out vec3 unit_v)
{
    unit_v = vec3(0.0);
    if (!rsx_pt_internal_finite3(v))
        return false;
    float m = max(abs(v.x), max(abs(v.y), abs(v.z)));
    if (!(m > 0.0))
        return false;
    vec3 s = v / m;
    float len = sqrt(dot(s, s));
    if (!rsx_pt_internal_finite(len) || !(len > 0.0))
        return false;
    vec3 r = s / len;
    if (!rsx_pt_internal_finite3(r))
        return false;
    unit_v = r;
    return true;
}

// Cosine-weighted hemisphere sample around normal; u1, u2 in [0,1).
// pdf = cos(theta) / pi.
bool rsx_pt_cosine_sample(vec3 normal, float u1, float u2, out vec3 direction, out float pdf)
{
    direction = vec3(0.0);
    pdf = 0.0;
    if (!rsx_pt_internal_finite(u1) || !rsx_pt_internal_finite(u2))
        return false;
    if (u1 < 0.0 || u1 >= 1.0 || u2 < 0.0 || u2 >= 1.0)
        return false;

    vec3 n;
    if (!rsx_pt_internal_unit(normal, n))
        return false;

    // Duff et al. 2017 branchless orthonormal basis.
    float s = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (s + n.z);
    float b = n.x * n.y * a;
    vec3 t = vec3(1.0 + s * n.x * n.x * a, s * b, -s * n.x);
    vec3 bt = vec3(b, s + n.y * n.y * a, -n.y);

    float r = sqrt(u1);
    float phi = 2.0 * RSX_PT_PI * u2;
    vec3 d = t * (r * cos(phi)) + bt * (r * sin(phi)) + n * sqrt(max(1.0 - u1, 0.0));
    float len = length(d);
    if (!rsx_pt_internal_finite(len) || !(len > 0.0))
        return false;
    d /= len;

    float p = dot(d, n) / RSX_PT_PI;
    if (!rsx_pt_internal_finite3(d) || !rsx_pt_internal_finite(p) || !(p > 0.0))
        return false;

    direction = d;
    pdf = p;
    return true;
}

// Lambertian BRDF (albedo/pi) * cos / pdf (cos/pi) == albedo.
bool rsx_pt_diffuse_step(vec3 throughput, vec3 albedo, out vec3 next)
{
    next = vec3(0.0);
    if (!rsx_pt_internal_finite3(throughput) || !rsx_pt_internal_finite3(albedo))
        return false;
    if (any(lessThan(throughput, vec3(0.0))))
        return false;
    if (any(lessThan(albedo, vec3(0.0))) || any(greaterThan(albedo, vec3(1.0))))
        return false;
    vec3 r = throughput * albedo;
    if (!rsx_pt_internal_finite3(r))
        return false;
    next = r;
    return true;
}

// Russian roulette from bounce 3 with p = clamp(maxComponent, 0.05, 0.95).
// Zero throughput is a valid terminated path (true, alive false).
bool rsx_pt_rr(vec3 throughput, uint bounce, float random, out vec3 next, out bool alive)
{
    next = vec3(0.0);
    alive = false;
    if (!rsx_pt_internal_finite3(throughput) || any(lessThan(throughput, vec3(0.0))))
        return false;
    if (!rsx_pt_internal_finite(random) || random < 0.0 || random >= 1.0)
        return false;

    float m = max(throughput.x, max(throughput.y, throughput.z));
    if (!(m > 0.0))
        return true;

    if (bounce < RSX_PT_RR_START_BOUNCE) {
        next = throughput;
        alive = true;
        return true;
    }

    float p = clamp(m, RSX_PT_RR_MIN_P, RSX_PT_RR_MAX_P);
    if (random >= p)
        return true;

    vec3 r = throughput / p;
    if (!rsx_pt_internal_finite3(r))
        return false;
    next = r;
    alive = true;
    return true;
}

// Ray origin offset: point + normalize(normal) * epsilon, epsilon finite and > 0.
bool rsx_pt_offset(vec3 point, vec3 normal, float epsilon, out vec3 origin)
{
    origin = vec3(0.0);
    if (!rsx_pt_internal_finite3(point))
        return false;
    if (!rsx_pt_internal_finite(epsilon) || !(epsilon > 0.0))
        return false;
    vec3 n;
    if (!rsx_pt_internal_unit(normal, n))
        return false;
    vec3 r = point + n * epsilon;
    if (!rsx_pt_internal_finite3(r))
        return false;
    origin = r;
    return true;
}

#endif // RSX_PATH_TRANSPORT_GLSL
