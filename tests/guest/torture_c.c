/* C-level pattern tests: make the COMPILER generate the control-flow and
 * libcall patterns that historically break lifters -- jump tables (dense,
 * sparse, nested), function-pointer dispatch through OPD, deep/mutual
 * recursion, varargs, 64-bit div/mod libcalls, string/mem builtins,
 * setjmp/longjmp, struct-by-value ABI, and (last, because it can hang)
 * the printf/dtoa machinery.
 *
 * Expected values are literals, verifiable by eye -- the point is whether
 * the DISPATCH reaches the right case and the ABI carries the right bits.
 */
#include "torture.h"
#include <stdarg.h>
#include <setjmp.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

typedef unsigned long long u64;
typedef long long i64;
typedef unsigned int u32;

/* volatile launder: keep gcc from constant-folding test inputs */
static volatile int vi;
static int L(int x) { vi = x; return vi; }

/* ---- switches: dense (jump table), sparse (compare tree), big ---------- */

static int NOINLINE sw_dense16(int x)
{
    switch (x) {
    case 0:  return 2;
    case 1:  return 3;
    case 2:  return 5;
    case 3:  return 7;
    case 4:  return 11;
    case 5:  return 13;
    case 6:  return 17;
    case 7:  return 19;
    case 8:  return 23;
    case 9:  return 29;
    case 10: return 31;
    case 11: return 37;
    case 12: return 41;
    case 13: return 43;
    case 14: return 47;
    case 15: return 53;
    default: return -1;
    }
}

static int NOINLINE sw_sparse(int x)
{
    switch (x) {
    case 1:      return 100;
    case 7:      return 200;
    case 100:    return 300;
    case 1000:   return 400;
    case 100000: return 500;
    default:     return -1;
    }
}

static int NOINLINE sw_dense33(int x)
{
    switch (x) {                       /* 33 cases: forces a table at -O2 */
    case 0: return 1000; case 1: return 1001; case 2: return 1002;
    case 3: return 1003; case 4: return 1004; case 5: return 1005;
    case 6: return 1006; case 7: return 1007; case 8: return 1008;
    case 9: return 1009; case 10: return 1010; case 11: return 1011;
    case 12: return 1012; case 13: return 1013; case 14: return 1014;
    case 15: return 1015; case 16: return 1016; case 17: return 1017;
    case 18: return 1018; case 19: return 1019; case 20: return 1020;
    case 21: return 1021; case 22: return 1022; case 23: return 1023;
    case 24: return 1024; case 25: return 1025; case 26: return 1026;
    case 27: return 1027; case 28: return 1028; case 29: return 1029;
    case 30: return 1030; case 31: return 1031; case 32: return 1032;
    default: return -1;
    }
}

static int NOINLINE sw_fallthrough(int x)
{
    int acc = 0;
    switch (x) {
    case 0: acc += 1;   /* fallthrough */
    case 1: acc += 10;  /* fallthrough */
    case 2: acc += 100; break;
    case 3: acc += 1000; break;
    default: acc = -1;
    }
    return acc;
}

static const int g_primes16[16] = { 2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31,
                                    37, 41, 43, 47, 53 };

static void switches_run(void)
{
    int i;
    t_section("switch");
    for (i = -1; i <= 16; i++)
        t_kat("sw_dense16", i + 1, (u64)(unsigned)sw_dense16(L(i)),
              (u64)(unsigned)((i >= 0 && i < 16) ? g_primes16[i] : -1));
    t_kat("sw_sparse", 0, (u64)(unsigned)sw_sparse(L(1)), 100);
    t_kat("sw_sparse", 1, (u64)(unsigned)sw_sparse(L(7)), 200);
    t_kat("sw_sparse", 2, (u64)(unsigned)sw_sparse(L(100)), 300);
    t_kat("sw_sparse", 3, (u64)(unsigned)sw_sparse(L(1000)), 400);
    t_kat("sw_sparse", 4, (u64)(unsigned)sw_sparse(L(100000)), 500);
    t_kat("sw_sparse", 5, (u64)(unsigned)sw_sparse(L(2)), 0xFFFFFFFFULL);
    for (i = 0; i <= 33; i++)
        t_kat("sw_dense33", i, (u64)(unsigned)sw_dense33(L(i)),
              (u64)(unsigned)(i < 33 ? 1000 + i : -1));
    t_kat("sw_fall", 0, (u64)(unsigned)sw_fallthrough(L(0)), 111);
    t_kat("sw_fall", 1, (u64)(unsigned)sw_fallthrough(L(1)), 110);
    t_kat("sw_fall", 2, (u64)(unsigned)sw_fallthrough(L(2)), 100);
    t_kat("sw_fall", 3, (u64)(unsigned)sw_fallthrough(L(3)), 1000);
}

/* ---- function pointers (OPD indirection) -------------------------------- */

static int fp_add(int a, int b) { return a + b; }
static int fp_sub(int a, int b) { return a - b; }
static int fp_mul(int a, int b) { return a * b; }
static int fp_xor(int a, int b) { return a ^ b; }

typedef int (*binop)(int, int);
static binop g_ops[4] = { fp_add, fp_sub, fp_mul, fp_xor };

static int NOINLINE apply(binop f, int a, int b) { return f(a, b); }

static void fnptr_run(void)
{
    t_section("fnptr");
    t_kat("fnptr_tbl", 0, (u64)(unsigned)g_ops[L(0)](7, 5), 12);
    t_kat("fnptr_tbl", 1, (u64)(unsigned)g_ops[L(1)](7, 5), 2);
    t_kat("fnptr_tbl", 2, (u64)(unsigned)g_ops[L(2)](7, 5), 35);
    t_kat("fnptr_tbl", 3, (u64)(unsigned)g_ops[L(3)](7, 5), 2);
    t_kat("fnptr_arg", 0, (u64)(unsigned)apply(fp_mul, 6, 7), 42);
    t_kat("fnptr_arg", 1, (u64)(unsigned)apply(g_ops[L(1)], 100, 58), 42);
}

/* ---- recursion ---------------------------------------------------------- */

static int NOINLINE fib(int n)
{
    return n < 2 ? n : fib(n - 1) + fib(n - 2);
}

static int NOINLINE is_odd(int n);
static int NOINLINE is_even(int n) { return n == 0 ? 1 : is_odd(n - 1); }
static int NOINLINE is_odd(int n)  { return n == 0 ? 0 : is_even(n - 1); }

static void recursion_run(void)
{
    t_section("recursion");
    t_kat("fib20", 0, (u64)fib(L(20)), 6765);
    t_kat("mutual_even", 0, (u64)is_even(L(1000)), 1);
    t_kat("mutual_odd", 0, (u64)is_odd(L(999)), 1);
}

/* ---- varargs ------------------------------------------------------------ */

static int NOINLINE sum_ints(int n, ...)
{
    va_list ap;
    int s = 0, i;
    va_start(ap, n);
    for (i = 0; i < n; i++)
        s += va_arg(ap, int);
    va_end(ap);
    return s;
}

static i64 NOINLINE sum_mixed(int n, ...)
{
    va_list ap;
    i64 s = 0;
    int i;
    va_start(ap, n);
    for (i = 0; i < n; i++) {
        if (i & 1) s += va_arg(ap, i64);
        else       s += va_arg(ap, int);
    }
    va_end(ap);
    return s;
}

static double NOINLINE sum_doubles(int n, ...)
{
    va_list ap;
    double s = 0;
    int i;
    va_start(ap, n);
    for (i = 0; i < n; i++)
        s += va_arg(ap, double);
    va_end(ap);
    return s;
}

static void varargs_run(void)
{
    t_section("varargs");
    t_kat("va_ints", 0, (u64)sum_ints(L(5), 1, 2, 3, 4, 5), 15);
    t_kat("va_ints_many", 0,
          (u64)sum_ints(L(10), 1, 2, 3, 4, 5, 6, 7, 8, 9, 10), 55);
    t_kat("va_mixed", 0,
          (u64)sum_mixed(L(4), 1, 0x100000000LL, 2, 0x200000000LL),
          0x300000003ULL);
    /* exact-representable doubles through FPR vararg slots */
    t_kat("va_dbl", 0, (u64)(sum_doubles(L(3), 1.5, 2.25, 4.25) * 4.0), 32);
}

/* ---- 64-bit integer arithmetic (libcalls + inline sequences) ------------ */

static void int64_run(void)
{
    volatile i64 a, b;
    volatile u64 ua, ub;
    t_section("int64");
    a = 0x123456789ABCDEFLL; b = 0x1000;
    t_kat("i64_div", 0, (u64)(a / b), 0x123456789ABCULL);
    t_kat("i64_mod", 0, (u64)(a % b), 0xDEFULL);
    a = -1000000007LL; b = 3;
    t_kat("i64_div_neg", 0, (u64)(a / b), (u64)-333333335LL);
    t_kat("i64_mod_neg", 0, (u64)(a % b), (u64)-2LL);
    ua = 0xFFFFFFFFFFFFFFFFULL; ub = 10;
    t_kat("u64_div", 0, ua / ub, 0x1999999999999999ULL);
    t_kat("u64_mod", 0, ua % ub, 5);
    a = 0x100000000LL; b = 0x100000000LL;
    t_kat("i64_mul_hi", 0, (u64)(a * b), 0);           /* wraps to 0 */
    ua = 0xDEADBEEFCAFEBABEULL;
    t_kat("u64_shr_var", 0, ua >> L(17), 0x6F56DF77E57ULL);
    t_kat("u64_shl_var", 0, ua << L(17), 0x7DD95FD757C0000ULL);
}

/* ---- string/mem builtins ------------------------------------------------ */

static void strings_run(void)
{
    static char src[64], dst[64];
    int i, off;
    t_section("strings");
    for (i = 0; i < 64; i++) src[i] = (char)(i + 1);

    for (off = 0; off < 4; off++) {
        memset(dst, 0, sizeof dst);
        memcpy(dst + off, src + 1, 17);
        t_kat("memcpy_off", off,
              (u64)(unsigned char)dst[off] |
              ((u64)(unsigned char)dst[off + 16] << 8), 0x1202ULL);
    }
    memset(dst, 0x5A, 33);
    t_kat("memset", 0,
          (u64)(unsigned char)dst[0] | ((u64)(unsigned char)dst[32] << 8) |
          ((u64)(unsigned char)dst[33] << 16),
          0x5A5AULL | ((u64)(unsigned char)dst[33] << 16));

    {
        static const char* words[] = { "", "a", "ab", "abcdefg",
                                       "0123456789abcdef7" };
        static const int lens[] = { 0, 1, 2, 7, 17 };
        for (i = 0; i < 5; i++)
            t_kat("strlen", i, (u64)strlen(words[i]), (u64)lens[i]);
    }
    t_kat("strcmp_lt", 0, (u64)(strcmp("apple", "banana") < 0), 1);
    t_kat("strcmp_eq", 0, (u64)(strcmp("same", "same") == 0), 1);
    t_kat("strcmp_gt", 0, (u64)(strcmp("zeta", "alpha") > 0), 1);

    /* overlapping memmove both directions */
    for (i = 0; i < 32; i++) dst[i] = (char)i;
    memmove(dst + 4, dst, 16);
    t_kat("memmove_fwd", 0,
          (u64)(unsigned char)dst[4] | ((u64)(unsigned char)dst[19] << 8),
          0x0F00ULL);
    for (i = 0; i < 32; i++) dst[i] = (char)i;
    memmove(dst, dst + 4, 16);
    t_kat("memmove_back", 0,
          (u64)(unsigned char)dst[0] | ((u64)(unsigned char)dst[15] << 8),
          0x1304ULL);
}

/* ---- setjmp/longjmp ------------------------------------------------------ */

static jmp_buf g_jb;

static void NOINLINE do_longjmp(int v)
{
    longjmp(g_jb, v);
}

static void setjmp_run(void)
{
    volatile int counter = 0;
    int rc;
    t_section("setjmp");
    rc = setjmp(g_jb);
    if (rc == 0) {
        counter = 1;
        do_longjmp(42);
        t_kat("setjmp_unreachable", 0, 1, 0);   /* must not get here */
    } else if (rc == 42 && counter == 1) {
        counter = 2;
        do_longjmp(7);
    } else if (rc == 7 && counter == 2) {
        t_kat("setjmp_chain", 0, 1, 1);
        return;
    }
    if (counter != 2)
        t_kat("setjmp_chain", 0, (u64)(unsigned)rc, 7);
}

/* ---- struct-by-value ABI ------------------------------------------------- */

typedef struct { int a; i64 b; char c; } SBig;
typedef struct { int x, y; } SSmall;

static SBig NOINLINE make_big(int a, i64 b, char c)
{
    SBig s; s.a = a; s.b = b; s.c = c; return s;
}

static i64 NOINLINE take_big(SBig s)
{
    return s.a + s.b + s.c;
}

static SSmall NOINLINE swap_small(SSmall s)
{
    SSmall r; r.x = s.y; r.y = s.x; return r;
}

static void structs_run(void)
{
    SBig b;
    SSmall s = { 3, 4 }, r;
    t_section("structs");
    b = make_big(L(100), 0x100000000LL, 7);
    t_kat("struct_ret", 0, (u64)take_big(b), 0x10000006BULL);
    r = swap_small(s);
    t_kat("struct_small", 0, ((u64)(unsigned)r.x << 8) | (unsigned)r.y,
          0x403ULL);
}

/* ---- float <-> int conversions in C -------------------------------------- */

static void fconv_run(void)
{
    volatile double d;
    volatile float f;
    t_section("fconv");
    d = 1.9;   t_kat("d2i_pos", 0, (u64)(unsigned)(int)d, 1);
    d = -1.9;  t_kat("d2i_neg", 0, (u64)(unsigned)(int)d, 0xFFFFFFFFULL);
    d = 4e9;   t_kat("d2u", 0, (u64)(unsigned)d, 4000000000ULL);
    d = 9.0e18; t_kat("d2ll", 0, (u64)(i64)d, 9000000000000000000ULL);
    f = 0.1f;
    {
        union { float f; u32 u; } v; v.f = f;
        t_kat("f_bits_0p1", 0, v.u, 0x3DCCCCCDULL);
    }
    d = 16777217.0;                       /* 2^24+1: not representable in f32 */
    f = (float)d;
    t_kat("d2f_round", 0, (u64)(f == 16777216.0f), 1);
    {
        volatile int i = 7;
        d = (double)i / 2.0;
        t_kat("i2d", 0, (u64)(d == 3.5), 1);
    }
}

/* ---- double bit-layout: the exact stfd/ld/lwz + exponent/k math newlib's
 * dtoa relies on (this is the machinery whose mis-lift caused the dtoa hang) - */

typedef union { double d; struct { u32 w0, w1; } w; unsigned long long u; } DBits;

static void NOINLINE fbits_run(void)
{
    static const struct { double d; unsigned long long bits; u32 w0hi; int i_exp; int k; }
    C[] = {
        /* d,      full IEEE bits,        word0,       binary exp, decimal-k */
        { 1.0,    0x3FF0000000000000ULL, 0x3FF00000,   0,   0 },
        { 2.0,    0x4000000000000000ULL, 0x40000000,   1,   0 },
        { 0.5,    0x3FE0000000000000ULL, 0x3FE00000,  -1,  -1 },
        { 100.0,  0x4059000000000000ULL, 0x40590000,   6,   2 },
        { 1e10,   0x4202A05F20000000ULL, 0x4202A05F,  33,  10 },
        { 1e-5,   0x3EE4F8B588E368F1ULL, 0x3EE4F8B5, -17,  -5 },
    };
    int n;
    t_section("fbits");
    for (n = 0; n < (int)(sizeof C / sizeof C[0]); n++) {
        volatile double dv = C[n].d;
        DBits u; u.d = dv;
        /* 64-bit raw bits (stfd + ld) */
        t_kat("fbits_u64", n, u.u, C[n].bits);
        /* high word (word0) via the 32-bit struct member (stfd + lwz@0 on BE) */
        t_kat("fbits_w0", n, u.w.w0, C[n].w0hi);
        /* binary exponent, exactly newlib's ((word0>>20 & 0x7FF) - 1023) */
        {
            int i = (int)((u.w.w0 >> 20) & 0x7FFu) - 1023;
            t_kat("fbits_iexp", n, (u64)(unsigned)i, (u64)(unsigned)C[n].i_exp);
        }
        /* renormalize mantissa to [1,2): word0 = (word0 & 0xFFFFF) | 0x3FF00000 */
        {
            DBits d2; d2.d = dv;
            d2.w.w0 = (d2.w.w0 & 0x000FFFFFu) | 0x3FF00000u;
            int i = (int)((u.w.w0 >> 20) & 0x7FFu) - 1023;
            /* newlib's estimate (Book: 0.3010299957 = log10(2)) */
            double ds = (d2.d - 1.5) * 0.289529654602168
                      + 0.1760912590558
                      + (double)i * 0.301029995663981;
            int k = (int)ds;
            if (ds < 0.0 && (double)k != ds) k--;
            t_kat("fbits_k", n, (u64)(unsigned)k, (u64)(unsigned)C[n].k);
        }
    }
}

/* ---- printf/snprintf: integer formats only (dtoa comes later) ------------ */

static void printf_int_run(void)
{
    char buf[128];
    t_section("printf_int");
    snprintf(buf, sizeof buf, "%d|%u|%x|%08X", -42, 42u, 0xbeef, 0xCAFE);
    t_check_str("snpf_int", 0, buf, "-42|42|beef|0000CAFE");
    snprintf(buf, sizeof buf, "%lld|%llu|%llx",
             -1234567890123456789LL, 18446744073709551615ULL,
             0xDEADBEEFCAFEBABEULL);
    t_check_str("snpf_ll", 0, buf, "-1234567890123456789|18446744073709551615|deadbeefcafebabe");
    snprintf(buf, sizeof buf, "[%-6s][%6s][%.3s]", "ab", "cd", "abcdef");
    t_check_str("snpf_str", 0, buf, "[ab    ][    cd][abc]");
    snprintf(buf, sizeof buf, "%c%c%c%%", 'x', 'y', 'z');
    t_check_str("snpf_char", 0, buf, "xyz%");
}

void torture_c_run(void)
{
    switches_run();
    fnptr_run();
    recursion_run();
    varargs_run();
    int64_run();
    strings_run();
    setjmp_run();
    structs_run();
    fconv_run();
    fbits_run();
    printf_int_run();
}

/* ---- dtoa / strtod: LAST -- known lifter-hang territory ------------------ */

void torture_dtoa_run(void)
{
    char buf[64];
    t_section("dtoa");
    snprintf(buf, sizeof buf, "%f", 1.5);
    t_check_str("dtoa_f", 0, buf, "1.500000");
    snprintf(buf, sizeof buf, "%g", 0.1);
    t_check_str("dtoa_g", 0, buf, "0.1");
    snprintf(buf, sizeof buf, "%e", 12345.6789);
    t_check_str("dtoa_e", 0, buf, "1.234568e+04");
    snprintf(buf, sizeof buf, "%.17g", 0.1);
    t_check_str("dtoa_17g", 0, buf, "0.10000000000000001");
    snprintf(buf, sizeof buf, "%g", 1e300);
    t_check_str("dtoa_1e300", 0, buf, "1e+300");
    {
        union { double d; u64 u; } v;
        v.d = strtod("0.1", 0);
        t_kat("strtod_0p1", 0, v.u, 0x3FB999999999999AULL);
        v.d = strtod("1e300", 0);
        t_kat("strtod_1e300", 0, v.u, 0x7E37E43C8800759CULL);
        v.d = strtod("-2.5", 0);
        t_kat("strtod_neg", 0, v.u, 0xC004000000000000ULL);
    }
    t_puts("dtoa section survived\n");
}
