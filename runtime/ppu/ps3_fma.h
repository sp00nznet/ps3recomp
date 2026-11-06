/* ps3_fma.h -- correctly rounded fused multiply-add for the PPU fmadd family, VMX vmaddfp and the SPU double ops.
 *
 * The guest's fused ops round once. With hardware FMA (built with -mfma / __FMA__, or arm64) fma() and fmaf() are the
 * instruction and round once. Without it they are C-runtime calls, and those differ: mingw-w64's own x86-64 fma()
 * (libmingwex, linked whenever the CRT is msvcrt, and kept by the GNU toolchain even with UCRT) is not correctly
 * rounded -- its comment admits a 1-ULP error; a bit test of 65,000 cases against an exact reference found 12,503
 * wrong results, 1,884 of them by more than 1 ULP. The UCRT's fma passed the same test under Wine, but which CRT a
 * build gets depends on the toolchain. So without hardware FMA this header uses its own integer-only implementation:
 * the same bits on every x86-64 CPU and with every CRT, and the same bits as the hardware path.
 *
 * ps3_fma_soft and ps3_fmaf_soft are adapted from musl libc src/math/fma.c and fmaf.c (MIT licence,
 * Copyright (c) 2005-2020 Rich Felker, et al.): type punning by memcpy, ps3_fma__clz64 for a_clz_64, the
 * FENV_ACCESS pragma dropped (the guest code never reads the host's exception flags). */
#ifndef PS3_FMA_H
#define PS3_FMA_H

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#if defined(__FMA__) || defined(__aarch64__) || defined(_M_ARM64)
#define PS3_FMA_HARDWARE 1
#endif

#ifdef _MSC_VER
#include <intrin.h>
static inline int ps3_fma__clz64(uint64_t x) { unsigned long i; _BitScanReverse64(&i, x); return 63 - (int)i; }
#else
static inline int ps3_fma__clz64(uint64_t x) { return __builtin_clzll(x); }   /* x != 0 at every call */
#endif

static inline uint64_t ps3_fma__u64(double x) { uint64_t i; memcpy(&i, &x, 8); return i; }
static inline double ps3_fma__f64(uint64_t i) { double x; memcpy(&x, &i, 8); return x; }

struct ps3_fma__num { uint64_t m; int e; int sign; };

/* normalize so top 10 bits and last bit are 0 */
static inline struct ps3_fma__num ps3_fma__normalize(double x)
{
    uint64_t ix = ps3_fma__u64(x);
    int e = (int)(ix >> 52);
    int sign = e & 0x800;
    e &= 0x7ff;
    if (!e) {
        ix = ps3_fma__u64(x * 0x1p63);
        e = (int)(ix >> 52 & 0x7ff);
        e = e ? e - 63 : 0x800;
    }
    ix &= (1ull << 52) - 1;
    ix |= 1ull << 52;
    ix <<= 1;
    e -= 0x3ff + 52 + 1;
    struct ps3_fma__num n = { ix, e, sign };
    return n;
}

static inline void ps3_fma__mul(uint64_t *hi, uint64_t *lo, uint64_t x, uint64_t y)
{
    uint64_t t1, t2, t3;
    uint64_t xlo = (uint32_t)x, xhi = x >> 32;
    uint64_t ylo = (uint32_t)y, yhi = y >> 32;
    t1 = xlo * ylo;
    t2 = xlo * yhi + xhi * ylo;
    t3 = xhi * yhi;
    *lo = t1 + (t2 << 32);
    *hi = t3 + (t2 >> 32) + (t1 > *lo);
}

#define PS3_FMA__ZEROINFNAN (0x7ff - 0x3ff - 52 - 1)

static inline double ps3_fma_soft(double x, double y, double z)
{
    struct ps3_fma__num nx = ps3_fma__normalize(x), ny = ps3_fma__normalize(y), nz = ps3_fma__normalize(z);

    if (nx.e >= PS3_FMA__ZEROINFNAN || ny.e >= PS3_FMA__ZEROINFNAN)
        return x * y + z;
    if (nz.e >= PS3_FMA__ZEROINFNAN) {
        if (nz.e > PS3_FMA__ZEROINFNAN) /* z == 0 */
            return x * y;
        return z;
    }

    /* mul: r = x*y */
    uint64_t rhi, rlo, zhi, zlo;
    ps3_fma__mul(&rhi, &rlo, nx.m, ny.m);
    /* either top 20 or 21 bits of rhi and last 2 bits of rlo are 0 */

    /* align exponents */
    int e = nx.e + ny.e;
    int d = nz.e - e;
    /* shift bits z<<=kz, r>>=kr, so kz+kr == d, set e = e+kr (== ez-kz) */
    if (d > 0) {
        if (d < 64) {
            zlo = nz.m << d;
            zhi = nz.m >> (64 - d);
        } else {
            zlo = 0;
            zhi = nz.m;
            e = nz.e - 64;
            d -= 64;
            if (d == 0) {
            } else if (d < 64) {
                rlo = rhi << (64 - d) | rlo >> d | !!(rlo << (64 - d));
                rhi = rhi >> d;
            } else {
                rlo = 1;
                rhi = 0;
            }
        }
    } else {
        zhi = 0;
        d = -d;
        if (d == 0) {
            zlo = nz.m;
        } else if (d < 64) {
            zlo = nz.m >> d | !!(nz.m << (64 - d));
        } else {
            zlo = 1;
        }
    }

    /* add */
    int sign = nx.sign ^ ny.sign;
    int samesign = !(sign ^ nz.sign);
    int nonzero = 1;
    if (samesign) {
        /* r += z */
        rlo += zlo;
        rhi += zhi + (rlo < zlo);
    } else {
        /* r -= z */
        uint64_t t = rlo;
        rlo -= zlo;
        rhi = rhi - zhi - (t < rlo);
        if (rhi >> 63) {
            rlo = -rlo;
            rhi = -rhi - !!rlo;
            sign = !sign;
        }
        nonzero = !!rhi;
    }

    /* set rhi to top 63bit of the result (last bit is sticky) */
    if (nonzero) {
        e += 64;
        d = ps3_fma__clz64(rhi) - 1;
        /* note: d > 0 */
        rhi = rhi << d | rlo >> (64 - d) | !!(rlo << d);
    } else if (rlo) {
        d = ps3_fma__clz64(rlo) - 1;
        if (d < 0)
            rhi = rlo >> 1 | (rlo & 1);
        else
            rhi = rlo << d;
    } else {
        /* exact +-0 */
        return x * y + z;
    }
    e -= d;

    /* convert to double */
    int64_t i = (int64_t)rhi; /* i is in [1<<62,(1<<63)-1] */
    if (sign)
        i = -i;
    double r = (double)i; /* |r| is in [0x1p62,0x1p63] */

    if (e < -1022 - 62) {
        /* result is subnormal before rounding */
        if (e == -1022 - 63) {
            double c = 0x1p63;
            if (sign)
                c = -c;
            if (r == c) {
                /* min normal after rounding, underflow depends on arch behaviour which can be imitated by a double
                   to float conversion */
                float fltmin = 0x0.ffffff8p-63f * FLT_MIN * (float)r;
                return DBL_MIN / FLT_MIN * fltmin;
            }
            /* one bit is lost when scaled, add another top bit to only round once at conversion if it is inexact */
            if (rhi << 53) {
                i = (int64_t)(rhi >> 1 | (rhi & 1) | 1ull << 62);
                if (sign)
                    i = -i;
                r = (double)i;
                r = 2 * r - c; /* remove top bit */
            }
        } else {
            /* only round once when scaled */
            d = 10;
            i = (int64_t)((rhi >> d | !!(rhi << (64 - d))) << d);
            if (sign)
                i = -i;
            r = (double)i;
        }
    }
    return scalbn(r, e);
}

static inline float ps3_fmaf_soft(float x, float y, float z)
{
    double xy = (double)x * y;
    uint64_t ui = ps3_fma__u64(xy + z);
    int e = (int)(ui >> 52 & 0x7ff);
    /* covers |r| > 0x1p-126 halfway cases (may round incorrectly) */
    int halfway = (ui & 0x1fffffff) == 0x10000000;
    /* covers tiny inexact (may miss uflow) and tiny halfway cases */
    int tiny = e <= 0x3ff - 126 && e >= 0x3ff - 149;
    if (!halfway && !tiny)
        /* common case, optimization only */
        return (float)ps3_fma__f64(ui);
    if (e != 0x7ff) {
        /* r+t == x*y+z exactly in nearest rounding, otherwise rounding does not affect t!=0, t<0 and inexact flag. */
        double ur = ps3_fma__f64(ui);
        int s = (int)(ui >> 63);
        double t = s == (xy < z) ? xy - ur + z : z - ur + xy;
        if (t) {
            /* adjust r toward r+t if r%2==0 (round to odd). may be needed if r is halfway or exact float, wrong if r
               becomes halfway or exact float. */
            ui -= (uint64_t)(s ^ (t < 0));
            ui |= 1;
        }
    }
    return (float)ps3_fma__f64(ui);
}

#ifdef PS3_FMA_HARDWARE
static inline double ps3_fma(double x, double y, double z) { return fma(x, y, z); }
static inline float ps3_fmaf(float x, float y, float z) { return fmaf(x, y, z); }
#else
static inline double ps3_fma(double x, double y, double z) { return ps3_fma_soft(x, y, z); }
static inline float ps3_fmaf(float x, float y, float z) { return ps3_fmaf_soft(x, y, z); }
#endif

#endif /* PS3_FMA_H */
