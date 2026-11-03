/* vmx_fmaf (runtime/ppu/ppu_vmx.h) against libm fmaf, and vmaddfp / vnmsubfp against fmaf lane by lane.
 *
 * vmx_fmaf must be bit-identical to fmaf for every input (NaNs compare as NaN). The inputs are random words,
 * random words limited to a mid exponent range (so sums stay normal and the double path is the one taken), and
 * products made to nearly cancel the addend (the subnormal and rounding-midpoint edges). Built with -mno-fma on
 * x86-64 so the double-rounding path is the one tested.
 *
 *   test_vmx_fmaf [count]     default 20 million cases
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include "../runtime/ppu/ppu_vmx.h"

static uint64_t st = 88172645463325252ull;
static uint32_t rnd(void) { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return (uint32_t)st; }
static float f(uint32_t u) { float x; memcpy(&x, &u, 4); return x; }
static uint32_t U(float x) { uint32_t u; memcpy(&u, &x, 4); return u; }
static void put(uint8_t* v, int i, float x) { vmx_sw(v, i, U(x)); }

int main(int argc, char** argv)
{
    const long count = argc > 1 ? atol(argv[1]) : 20000000L;
    unsigned long long n = 0, bad = 0, lane_bad = 0;
    for (long i = 0; i < count; i++) {
        uint32_t a = rnd(), b = rnd(), c = rnd();
        if (i & 1) {
            a = (a & 0x80FFFFFF) | ((0x60 + (rnd() % 0x40)) << 23);
            b = (b & 0x80FFFFFF) | ((0x60 + (rnd() % 0x40)) << 23);
            c = (c & 0x80FFFFFF) | ((0x60 + (rnd() % 0x40)) << 23);
        }
        if ((i & 7) == 2) c = U(-f(a) * f(b)) ^ (rnd() & 0xF);   /* near cancellation */
        const float x = f(a), z = f(b), y = f(c);
        const uint32_t r1 = U(vmx_fmaf(x, z, y)), r2 = U(fmaf(x, z, y));
        n++;
        if (r1 != r2 && !(isnan(f(r1)) && isnan(f(r2)))) {
            if (bad < 5) printf("diff %08x %08x %08x: %08x vs %08x\n", a, b, c, r1, r2);
            bad++;
        }
        if ((i & 63) == 0) {
            /* vmaddfp: d = a * c + b; vnmsubfp: d = -(a * c - b). Four lanes, no flush, no NaNs. */
            uint8_t va[16], vb[16], vc[16], d1[16], d2[16];
            float xs[4], zs[4], ys[4];
            for (int l = 0; l < 4; l++) {
                xs[l] = f(rnd()); zs[l] = f(rnd()); ys[l] = f(rnd());
                if (!isfinite(xs[l]) || !isfinite(zs[l]) || !isfinite(ys[l])) { xs[l] = 1.5f; zs[l] = -2.25f; ys[l] = 0.75f; }
                put(va, l, xs[l]); put(vc, l, zs[l]); put(vb, l, ys[l]);
            }
            vmx_vmaddfp(d1, va, vb, vc, 0);
            vmx_vnmsubfp(d2, va, vb, vc, 0);
            for (int l = 0; l < 4; l++) {
                const float e1 = fmaf(xs[l], zs[l], ys[l]), e2 = -fmaf(xs[l], zs[l], -ys[l]);
                if (isnan(e1) || isnan(e2)) continue;
                if (vmx_w(d1, l) != U(e1) || vmx_w(d2, l) != U(e2)) lane_bad++;
            }
        }
    }
    printf("%llu cases, %llu differ; vmaddfp/vnmsubfp lanes: %llu differ\n", n, bad, lane_bad);
    return (bad || lane_bad) ? 1 : 0;
}
