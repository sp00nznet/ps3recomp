#!/usr/bin/env python3
"""Compile actual PPU lifter emissions for the float cases of #823.

1. fsqrt/fsqrts/frsqrte of a negative number give the PPC default QNaN
   7FF8000000000000, not x86's negative one.
2. fmadds/fmsubs/fnmadds/fnmsubs round the exact result to single ONCE. The
   reference is exact rational arithmetic (fractions.Fraction), with cases
   built to land the double result exactly halfway between two singles, where
   rounding twice goes wrong.
3. stfs stores bits (Book I 4.6.3) instead of rounding.
"""
from fractions import Fraction
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess
import tempfile

from ppu_disasm import decode
from ppu_lifter import PPULifter, LiftedFunction, FENCE_PREAMBLE

ROOT = Path(__file__).resolve().parent.parent


def d2u(x):
    return struct.unpack('<Q', struct.pack('<d', x))[0]


def u2d(u):
    return struct.unpack('<d', struct.pack('<Q', u))[0]


def f32_bits(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def round_f32(x: Fraction, neg_zero=False):
    """Exact value -> IEEE single bits, round to nearest even."""
    if x == 0:
        return 0x80000000 if neg_zero else 0
    sign = 0x80000000 if x < 0 else 0
    a = abs(x)
    e = a.numerator.bit_length() - a.denominator.bit_length()
    if Fraction(2) ** e > a:
        e -= 1
    e = max(e, -126)
    ulp = Fraction(2) ** (e - 23)
    q = a / ulp
    n = q.numerator // q.denominator
    rem = q - n
    if rem > Fraction(1, 2) or (rem == Fraction(1, 2) and n & 1):
        n += 1
    v = n * ulp
    if v >= Fraction(2) ** 128:
        return sign | 0x7F800000
    return sign | f32_bits(float(v))


def trunc_f32(x: Fraction):
    """Exact value in single range -> single bits, truncated toward zero."""
    if x == 0:
        return 0
    sign = 0x80000000 if x < 0 else 0
    a = abs(x)
    e = a.numerator.bit_length() - a.denominator.bit_length()
    if Fraction(2) ** e > a:
        e -= 1
    ulp = Fraction(2) ** (max(e, -126) - 23)
    q = a / ulp
    return sign | f32_bits(float((q.numerator // q.denominator) * ulp))


FMA_FORMS = {'fmadds': (29, 0, 0), 'fmsubs': (28, 1, 0),
             'fnmadds': (31, 0, 1), 'fnmsubs': (30, 1, 1)}


def fma_ref(a, c, b, neg_b, neg_res):
    exact = Fraction(a) * Fraction(c) + (-Fraction(b) if neg_b else Fraction(b))
    if neg_res:
        exact = -exact
    neg_zero = exact == 0 and (neg_res ^ (d2u((a * c) + (-b if neg_b else b)) >> 63 == 1))
    return round_f32(exact, neg_zero)


def fma_cases(rng):
    singles = lambda lo, hi: struct.unpack('<f', struct.pack('<f', rng.uniform(lo, hi)))[0]
    cases = [(1.5, struct.unpack('<f', struct.pack('<f', 20000.6))[0], u2d(0x3810000000000000))]
    for _ in range(3000):                     # ordinary operands
        cases.append((singles(-1e4, 1e4), singles(-1e4, 1e4), singles(-1e8, 1e8)))
    for _ in range(6000):                     # double result exactly on a single midpoint
        a, c = singles(-100, 100), singles(-100, 100)
        if a * c == 0:
            continue
        f = abs(rng.uniform(1e-3, 1e6))
        e = Fraction(f).limit_denominator(1 << 60)
        mid_bits = f32_bits(float(e)) & 0x7FFFFFFF
        lo, hi = struct.unpack('<f', struct.pack('<I', mid_bits))[0], struct.unpack('<f', struct.pack('<I', mid_bits + 1))[0]
        m = (Fraction(lo) + Fraction(hi)) / 2
        nudge = rng.choice([0, 1, -1]) * Fraction(2) ** rng.randint(-120, -60)
        b = float(m + nudge - Fraction(a) * Fraction(c))
        cases += [(a, c, b), (a, c, -b)]       # -b: the same tie for fmsubs/fnmsubs
    for _ in range(500):                      # results in the single denormal range
        a, c = singles(-1, 1), singles(-1e-20, 1e-20)
        cases.append((a, c, rng.uniform(-1e-38, 1e-38)))
    big = struct.unpack('<f', struct.pack('<I', 0x7F7FFFFF))[0]   # FLT_MAX
    cases += [(big, 1.0, 2.0 ** 103), (big, 1.0, 2.0 ** 103 + 2.0 ** 60), (big, 1.0, 2.0 ** 103 - 2.0 ** 60),
              (1.0, 1.0, -1.0), (0.0, 5.0, -0.0), (3.0, 7.0, 0.0)]
    return cases


def stfs_cases(rng):
    cases = [u2d(0x3F80123456789ABC)]
    for _ in range(2000):
        cases.append(rng.uniform(-1e30, 1e30) * rng.choice([1, 1e-30, 1e-35]))
    for _ in range(500):                      # single denormal range: exponent 874..896
        cases.append(rng.choice([1, -1]) * rng.uniform(2.0 ** -149, 2.0 ** -126))
    return cases


def emitted_driver(rng):
    lifter = PPULifter()
    func = LiftedFunction(name="fp_test", start_addr=0x1000, end_addr=0x2000)
    emit = lambda word: lifter._translate_op(decode(word, 0x1000), func)
    pre = lifter._preamble_lines()
    pre[0] = pre[0].replace('#include "ppu_recomp.h"', '#include "ppu_context.h"\n' + FENCE_PREAMBLE)
    lines = list(pre) + ['#include <stdio.h>', '#include <inttypes.h>',
                         '#undef vm_write32', 'static uint32_t stored;',
                         'static void vm_write32(uint64_t, uint32_t v) { stored = v; }']
    # f1 = f2 * f3 + f4 (A-form: frt, fra, frb, frc)
    for mn, (xo, _, _) in FMA_FORMS.items():
        word = (59 << 26) | (1 << 21) | (2 << 16) | (4 << 11) | (3 << 6) | (xo << 1)
        lines.append(f'static void op_{mn}(ppu_context* ctx) {{ {emit(word)} }}')
    for mn, word in [('fsqrt', (63 << 26) | (1 << 21) | (4 << 11) | (22 << 1)),
                     ('fsqrts', (59 << 26) | (1 << 21) | (4 << 11) | (22 << 1)),
                     ('frsqrte', (63 << 26) | (1 << 21) | (4 << 11) | (26 << 1))]:
        lines.append(f'static void op_{mn}(ppu_context* ctx) {{ {emit(word)} }}')
    lines.append(f'static void op_stfs(ppu_context* ctx) {{ {emit((52 << 26) | (4 << 21) | (3 << 16) | 0x10)} }}')
    lines.append(f'static void op_stfsx(ppu_context* ctx) {{ {emit((31 << 26) | (4 << 21) | (3 << 16) | (5 << 11) | (663 << 1))} }}')

    rows = []
    for a, c, b in fma_cases(rng):
        for mn, (_, nb, nr) in FMA_FORMS.items():
            rows.append(f'{{op_{mn},"{mn}",0x{d2u(a):016X}ull,0x{d2u(c):016X}ull,0x{d2u(b):016X}ull,'
                        f'0x{fma_ref(a, c, b, nb, nr):08X}u}}')
    srows = []
    for d in stfs_cases(rng):
        srows.append(f'{{0x{d2u(d):016X}ull,0x{trunc_f32(Fraction(d)):08X}u}}')
    lines.append(r'''
struct FmaCase { void (*op)(ppu_context*); const char* mn; uint64_t a, c, b; uint32_t want; };
static const FmaCase fma_cases[] = {''' + ',\n'.join(rows) + r'''};
struct StfsCase { uint64_t d; uint32_t want; };
static const StfsCase stfs_cases[] = {''' + ',\n'.join(srows) + r'''};
static unsigned checks, failures;
static void check(const char* what, uint64_t in, uint64_t got, uint64_t want) {
    ++checks;
    if (got != want && ++failures <= 20)
        fprintf(stderr, "FAIL %s in=%016" PRIx64 " got=%" PRIx64 " want=%" PRIx64 "\n", what, in, got, want);
}
static uint64_t bits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static double dbl(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
static uint32_t single_bits(double d) { float f = (float)d; uint32_t u; memcpy(&u, &f, 4); return u; }
int main() {
    for (const FmaCase& t : fma_cases) {
        ppu_context ctx{}; ctx.fpr[2] = dbl(t.a); ctx.fpr[3] = dbl(t.c); ctx.fpr[4] = dbl(t.b);
        t.op(&ctx);
        check(t.mn, t.a, single_bits(ctx.fpr[1]), t.want);
        check("fmadds result is exactly a single", t.a, bits(ctx.fpr[1]), bits((double)(float)ctx.fpr[1]));
    }
    void (*roots[])(ppu_context*) = {op_fsqrt, op_fsqrts, op_frsqrte};
    const char* names[] = {"fsqrt", "fsqrts", "frsqrte"};
    for (int i = 0; i < 3; i++) {
        const double neg[] = {-1.0, -1e-300, -INFINITY, -2.5e10};
        for (double v : neg) {
            ppu_context ctx{}; ctx.fpr[4] = v; roots[i](&ctx);
            check(names[i], bits(v), bits(ctx.fpr[1]), 0x7FF8000000000000ull);
        }
        ppu_context q{}; q.fpr[4] = dbl(0xFFF4000000000001ull); roots[i](&q);   /* SNaN in: quieted, sign kept */
        check(names[i], 0xFFF4000000000001ull, bits(q.fpr[1]) >> 51, 0x1FFFull);
    }
    { ppu_context ctx{}; ctx.fpr[4] = 4.0; op_fsqrt(&ctx); check("fsqrt 4", 4, bits(ctx.fpr[1]), bits(2.0)); }
    { ppu_context ctx{}; ctx.fpr[4] = -0.0; op_fsqrt(&ctx); check("fsqrt -0", 0, bits(ctx.fpr[1]), bits(-0.0)); }
    for (const StfsCase& t : stfs_cases) {
        ppu_context ctx{}; ctx.fpr[4] = dbl(t.d); ctx.gpr[3] = 0x100; ctx.gpr[5] = 8;
        stored = 0; op_stfs(&ctx); check("stfs", t.d, stored, t.want);
        stored = 0; op_stfsx(&ctx); check("stfsx", t.d, stored, t.want);
    }
    const uint64_t specials[][2] = {{0x7FF0000000000000ull, 0x7F800000u}, {0xFFF0000000000000ull, 0xFF800000u},
        {0x7FF8000000000000ull, 0x7FC00000u}, {0x7FF4000000000000ull, 0x7FA00000u},   /* SNaN stays signalling */
        {0x8000000000000000ull, 0x80000000u}, {0x0000000000000000ull, 0u}};
    for (const auto& s : specials) {
        ppu_context ctx{}; ctx.fpr[4] = dbl(s[0]); ctx.gpr[3] = 0x100;
        stored = 0; op_stfs(&ctx); check("stfs special", s[0], stored, s[1]);
    }
    printf("PPU float (#823) checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
''')
    return '\n'.join(lines)


def main():
    rng = random.Random(823)
    with tempfile.TemporaryDirectory(prefix='ppu-float-823-') as temporary:
        root = Path(temporary); source = root/'fp823.cpp'
        executable = root/('fp823.exe' if os.name == 'nt' else 'fp823')
        source.write_text(emitted_driver(rng))
        if os.name == 'nt':
            from msvc_env import find_vcvars
            vcvars = find_vcvars()
            if not vcvars:
                raise SystemExit('Visual Studio environment is required; no silent skip.')
            batch = root/'run.bat'
            batch.write_text('@echo off\n'
                             + f'call "{vcvars}" >nul 2>nul\n'
                             + f'cd /d "{root}"\n'
                             + f'cl /nologo /std:c++17 /O1 /W3 /fp:precise /I "{ROOT / "runtime/ppu"}" /Fe:"{executable}" "{source}"\n'
                             + 'if errorlevel 1 exit /b 2\n'
                             + f'"{executable}"\n')
            return subprocess.run(['cmd', '/c', str(batch)]).returncode
        cxx = os.environ.get('CXX') or shutil.which('c++') or shutil.which('clang++')
        if not cxx:
            raise SystemExit('A C++ compiler is required; no silent skip.')
        subprocess.run([cxx, '-std=c++17', '-O2', '-ffp-contract=off', '-Wno-parentheses', '-I',
                        str(ROOT/'runtime/ppu'), str(source), '-o', str(executable)], check=True)
        return subprocess.run([str(executable)]).returncode


if __name__ == '__main__':
    raise SystemExit(main())
