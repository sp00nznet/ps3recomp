/* ps3_fma_soft / ps3_fmaf_soft (runtime/ppu/ps3_fma.h) against the CPU's FMA instruction, bit for bit.
 *
 * Built with -mno-fma, so ps3_fma/ps3_fmaf take the integer path; the reference functions below are compiled for FMA
 * with a target attribute and only called when the CPU has it (IEEE 754 fusedMultiplyAdd rounds once, so the
 * instruction is an exact reference). Cases: random operands, near-cancellation (z close to -x*y), z near half an ULP
 * of x*y (rounding midpoints), and tiny results in the subnormal range. Exit 0 = all equal (or skipped: no FMA).
 * `test_ps3_fma --control [N]` runs the same cases through the C runtime's fma()/fmaf() instead (the old path), as a
 * negative control: it fails where that runtime is not correctly rounded, as mingw-w64's x86-64 versions are not. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ps3_fma.h"

#if !(defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__)))
int main(void) { printf("test_ps3_fma: skipped (x86-64 GCC/Clang only)\n"); return 0; }
#else

__attribute__((target("fma"))) static double ref_fma(double x, double y, double z) { return __builtin_fma(x, y, z); }
__attribute__((target("fma"))) static float ref_fmaf(float x, float y, float z) { return __builtin_fmaf(x, y, z); }

static uint64_t s_rng = 0x9E3779B97F4A7C15ull;
static uint64_t next(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17; return s_rng; }
static double rnd_d(int span)
{
    uint64_t m = (next() & ((1ull << 52) - 1)) | (1ull << 52);
    int e = (int)(next() % (2u * span + 1)) - span;
    double v = ldexp((double)m, e - 52);
    return (next() & 1) ? -v : v;
}
static float rnd_f(int span) { return (float)rnd_d(span); }
static uint64_t bits_d(double x) { uint64_t u; memcpy(&u, &x, 8); return u; }
static uint32_t bits_f(float x) { uint32_t u; memcpy(&u, &x, 4); return u; }

static long bad_d, bad_f, n_d, n_f;
static int s_control;
static double (*volatile crt_fma)(double, double, double) = fma;
static float (*volatile crt_fmaf)(float, float, float) = fmaf;
static void check_d(double x, double y, double z)
{
    n_d++;
    double got = s_control ? crt_fma(x, y, z) : ps3_fma(x, y, z), want = ref_fma(x, y, z);
    if (bits_d(got) != bits_d(want) && !(got != got && want != want)) {
        if (bad_d++ < 8) printf("fma(%a, %a, %a) = %a, want %a\n", x, y, z, got, want);
    }
}
static void check_f(float x, float y, float z)
{
    n_f++;
    float got = s_control ? crt_fmaf(x, y, z) : ps3_fmaf(x, y, z), want = ref_fmaf(x, y, z);
    if (bits_f(got) != bits_f(want) && !(got != got && want != want)) {
        if (bad_f++ < 8) printf("fmaf(%a, %a, %a) = %a, want %a\n", (double)x, (double)y, (double)z, (double)got, (double)want);
    }
}

int main(int argc, char **argv)
{
    int a = 1;
    if (argc > 1 && strcmp(argv[1], "--control") == 0) { s_control = 1; a = 2; }
    long n = argc > a ? atol(argv[a]) : 200000;
    __builtin_cpu_init();
    if (!__builtin_cpu_supports("fma")) { printf("test_ps3_fma: skipped (CPU has no FMA)\n"); return 0; }
#ifdef PS3_FMA_HARDWARE
    printf("test_ps3_fma: note: built with hardware FMA, the integer path is not under test\n");
#endif
    for (long i = 0; i < n; i++) {
        double x = rnd_d(60), y = rnd_d(60), p = x * y;
        check_d(x, y, rnd_d(60));                                         /* random */
        check_d(x, y, (next() & 1) ? -p : -nextafter(p, INFINITY));      /* cancellation */
        check_d(x, y, ldexp(1.0, ilogb(p) - 53) * (double)(int)(next() % 5) * 0.5 * ((next() & 1) ? 1 : -1));  /* midpoints */
        check_d(ldexp(1.0 + (double)(next() % 1000) / 1000, -540), ldexp(1.0 + (double)(next() % 1000) / 1000, -540),
                ldexp((double)(next() % 1000) / 1000, -1034) * ((next() & 1) ? 1 : -1));                /* subnormal range */
        float a = rnd_f(40), b = rnd_f(40), q = a * b;
        check_f(a, b, rnd_f(40));
        check_f(a, b, -q);
        check_f(a, b, ldexpf(1.0f, ilogbf(q) - 24) * (float)(int)(next() % 5) * 0.5f * ((next() & 1) ? 1 : -1));
        check_f(ldexpf(1.0f + (float)(next() % 1000) / 1000, -70), ldexpf(1.0f + (float)(next() % 1000) / 1000, -70),
                ldexpf((float)(next() % 1000) / 1000, -130) * ((next() & 1) ? 1 : -1));
    }
    /* special values: zeros, infinities, NaN, overflow */
    const double sd[] = { 0.0, -0.0, 1.0, -1.0, INFINITY, -INFINITY, NAN, 0x1p1023, 0x1p-1074, 0x1.fffffffffffffp1023 };
    for (unsigned i = 0; i < 10; i++) for (unsigned j = 0; j < 10; j++) for (unsigned k = 0; k < 10; k++) {
        check_d(sd[i], sd[j], sd[k]);
        check_f((float)sd[i], (float)sd[j], (float)sd[k]);
    }
    printf("test_ps3_fma%s: fma %ld cases, %ld wrong; fmaf %ld cases, %ld wrong\n", s_control ? " (control: C runtime fma/fmaf)" : "",
           n_d, bad_d, n_f, bad_f);
    return (bad_d || bad_f) ? 1 : 0;
}
#endif
