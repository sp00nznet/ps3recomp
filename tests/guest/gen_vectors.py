#!/usr/bin/env python3
"""
Known-answer-test (KAT) generator for the ps3recomp lifter torture suite.

Implements PPC64 integer + FPU instruction semantics in Python (an
INDEPENDENT reference model -- not derived from the lifter) and emits
tests/guest/torture_kats.c: thousands of inline-asm test blocks that execute
the real instruction on the "guest CPU" (i.e. through ppu_lifter.py's C
emission) and compare against the model's expected value, CA, and CR bits.

A disagreement means the lifter or this model is wrong -- both are readable,
so triage is fast. The model is validated at generation time against
Python's own IEEE-754 doubles and a set of hand-checked identities.

Semantics references: PowerPC Architecture Book I (PEM), Cell BE Handbook.
Conventions used below:
  * all register values are Python ints masked to 64 bits (M64)
  * BE bit numbering for masks: bit 0 = MSB = 1<<63
  * CA/OV/CR computed per Book I; "undefined" result fields are masked out
    of the comparison (e.g. mulhw's high 32 bits) via a per-test mask.

Usage:  python gen_vectors.py [-o torture_kats.c]
"""

import argparse
import ctypes
import random
import struct
from fractions import Fraction

M64 = (1 << 64) - 1
M32 = (1 << 32) - 1


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def sext(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def sext32(v):
    return sext(v, 32)


def sext64(v):
    return sext(v, 64)


def rotl64(v, n):
    n &= 63
    return ((v << n) | (v >> (64 - n))) & M64 if n else v & M64


def rotl32dup(v, n):
    """rlw* rotation: rotate the LOW 32 bits, result duplicated into both
    halves of the 64-bit rotate output (Book I: r <- ROTL32(RS[32:63], n))."""
    lo = v & M32
    n &= 31
    r32 = ((lo << n) | (lo >> (32 - n))) & M32 if n else lo
    return (r32 << 32) | r32


def mask64(mb, me):
    """MASK(mb, me) with BE bit numbering (0 = MSB). Wraps when mb > me."""
    def bit(i):
        return 1 << (63 - i)
    if mb <= me:
        m = 0
        for i in range(mb, me + 1):
            m |= bit(i)
        return m
    # wrap: mb..63 plus 0..me
    m = 0
    for i in range(mb, 64):
        m |= bit(i)
    for i in range(0, me + 1):
        m |= bit(i)
    return m


def cr_signed(res64):
    """CR field from a signed-64 compare of res against 0 (record forms).
    LT=8, GT=4, EQ=2; SO passthrough is 0 in our tests."""
    s = sext64(res64)
    return 8 if s < 0 else (4 if s > 0 else 2)


# ---------------------------------------------------------------------------
# integer model: each op returns (result, ca) or (result,) -- ca=None if the
# op doesn't touch CA
# ---------------------------------------------------------------------------

def add_carry(a, b, cin=0):
    s = a + b + cin
    return s & M64, 1 if s > M64 else 0


def op_add(a, b, ca):        r, _ = add_carry(a, b);          return r, None
def op_addc(a, b, ca):       return add_carry(a, b)
def op_adde(a, b, ca):       return add_carry(a, b, ca)
def op_subf(a, b, ca):       r, _ = add_carry((~a) & M64, b, 1); return r, None
def op_subfc(a, b, ca):      return add_carry((~a) & M64, b, 1)
def op_subfe(a, b, ca):      return add_carry((~a) & M64, b, ca)
def op_and(a, b, ca):        return a & b, None
def op_or(a, b, ca):         return a | b, None
def op_xor(a, b, ca):        return a ^ b, None
def op_nand(a, b, ca):       return (~(a & b)) & M64, None
def op_nor(a, b, ca):        return (~(a | b)) & M64, None
def op_eqv(a, b, ca):        return (~(a ^ b)) & M64, None
def op_andc(a, b, ca):       return a & ((~b) & M64), None
def op_orc(a, b, ca):        return a | ((~b) & M64), None


def op_mullw(a, b, ca):
    return (sext32(a) * sext32(b)) & M64, None


def op_mulhw(a, b, ca):      # high 32 defined, low... RT[32:63]=prod[0:31], RT[0:31] undefined
    p = (sext32(a) * sext32(b)) & M64
    return (p >> 32) & M32, None    # compare under mask 0xFFFFFFFF


def op_mulhwu(a, b, ca):
    p = ((a & M32) * (b & M32)) & M64
    return (p >> 32) & M32, None


def op_mulld(a, b, ca):
    return (sext64(a) * sext64(b)) & M64, None


def op_mulhd(a, b, ca):
    return ((sext64(a) * sext64(b)) >> 64) & M64, None


def op_mulhdu(a, b, ca):
    return ((a * b) >> 64) & M64, None


def _trunc_div(n, d):
    q = abs(n) // abs(d)
    return -q if (n < 0) != (d < 0) else q


def op_divw(a, b, ca):
    n, d = sext32(a), sext32(b)
    if d == 0 or (n == -0x80000000 and d == -1):
        return None, None            # undefined result -> nocrash-only test
    return _trunc_div(n, d) & M32, None   # RT[0:31] undefined -> mask low32


def op_divwu(a, b, ca):
    n, d = a & M32, b & M32
    if d == 0:
        return None, None
    return (n // d) & M32, None


def op_divd(a, b, ca):
    n, d = sext64(a), sext64(b)
    if d == 0 or (n == -(1 << 63) and d == -1):
        return None, None
    return _trunc_div(n, d) & M64, None


def op_divdu(a, b, ca):
    n, d = a, b
    if d == 0:
        return None, None
    return (n // d) & M64, None


def op_slw(a, b, ca):
    n = b & 0x3F
    return ((a & M32) << n) & M32 if n < 32 else 0, None


def op_srw(a, b, ca):
    n = b & 0x3F
    return (a & M32) >> n if n < 32 else 0, None


def op_sld(a, b, ca):
    n = b & 0x7F
    return (a << n) & M64 if n < 64 else 0, None


def op_srd(a, b, ca):
    n = b & 0x7F
    return a >> n if n < 64 else 0, None


def _sra(val, sh, width):
    """Arithmetic shift right of `width`-bit value; returns (res64, ca)."""
    s = sext(val, width)
    if sh >= width:
        res = -1 if s < 0 else 0
        ca = 1 if (s < 0 and (val & ((1 << width) - 1)) != 0) else 0
    else:
        res = s >> sh
        lost = val & ((1 << sh) - 1) if sh else 0
        ca = 1 if (s < 0 and lost != 0) else 0
    return res & M64, ca


def op_sraw(a, b, ca):
    return _sra(a & M32, b & 0x3F, 32)


def op_srad(a, b, ca):
    return _sra(a, b & 0x7F, 64)


RR_OPS = {
    # name: (fn, has_ca_in, result_mask, has_ca_out)
    "add":    (op_add,    False, M64, False),
    "subf":   (op_subf,   False, M64, False),
    "addc":   (op_addc,   False, M64, True),
    "subfc":  (op_subfc,  False, M64, True),
    "adde":   (op_adde,   True,  M64, True),
    "subfe":  (op_subfe,  True,  M64, True),
    "and":    (op_and,    False, M64, False),
    "or":     (op_or,     False, M64, False),
    "xor":    (op_xor,    False, M64, False),
    "nand":   (op_nand,   False, M64, False),
    "nor":    (op_nor,    False, M64, False),
    "eqv":    (op_eqv,    False, M64, False),
    "andc":   (op_andc,   False, M64, False),
    "orc":    (op_orc,    False, M64, False),
    "mullw":  (op_mullw,  False, M64, False),
    "mulhw":  (op_mulhw,  False, M32, False),
    "mulhwu": (op_mulhwu, False, M32, False),
    "mulld":  (op_mulld,  False, M64, False),
    "mulhd":  (op_mulhd,  False, M64, False),
    "mulhdu": (op_mulhdu, False, M64, False),
    "slw":    (op_slw,    False, M64, False),
    "srw":    (op_srw,    False, M64, False),
    "sld":    (op_sld,    False, M64, False),
    "srd":    (op_srd,    False, M64, False),
    "sraw":   (op_sraw,   False, M64, True),
    "srad":   (op_srad,   False, M64, True),
    "divw":   (op_divw,   False, M32, False),
    "divwu":  (op_divwu,  False, M32, False),
    "divd":   (op_divd,   False, M64, False),
    "divdu":  (op_divdu,  False, M64, False),
}

# record ('.') forms worth testing: cr0 must reflect the signed-64 result
DOT_OPS = ["add", "subf", "and", "or", "xor", "adde", "sraw"]

# single-source ops
def op_neg(a, ca):     r, _ = add_carry((~a) & M64, 0, 1); return r, None
def op_addme(a, ca):   return add_carry(a, M64, ca)
def op_addze(a, ca):   return add_carry(a, 0, ca)
def op_subfme(a, ca):  return add_carry((~a) & M64, M64, ca)
def op_subfze(a, ca):  return add_carry((~a) & M64, 0, ca)
def op_extsb(a, ca):   return sext(a, 8) & M64, None
def op_extsh(a, ca):   return sext(a, 16) & M64, None
def op_extsw(a, ca):   return sext(a, 32) & M64, None


def op_cntlzw(a, ca):
    v = a & M32
    return (32 - v.bit_length()) if v else 32, None


def op_cntlzd(a, ca):
    return (64 - a.bit_length()) if a else 64, None


R1_OPS = {
    "neg":    (op_neg,    False, False),
    "addme":  (op_addme,  True,  True),
    "addze":  (op_addze,  True,  True),
    "subfme": (op_subfme, True,  True),
    "subfze": (op_subfze, True,  True),
    "extsb":  (op_extsb,  False, False),
    "extsh":  (op_extsh,  False, False),
    "extsw":  (op_extsw,  False, False),
    "cntlzw": (op_cntlzw, False, False),
    "cntlzd": (op_cntlzd, False, False),
}


# ---------------------------------------------------------------------------
# FPU model. Python floats ARE IEEE-754 doubles with round-to-nearest-even,
# so ordinary arithmetic is bit-exact vs PPC default mode; only specials
# (NaN propagation/generation, div-by-zero) need explicit handling.
# PPC generated QNaN = 0x7FF8000000000000. NaN propagation for 2-op: frA's
# NaN wins, else frB's (quieted).
# ---------------------------------------------------------------------------

QNAN = 0x7FF8000000000000


def d2b(d):
    return struct.unpack(">Q", struct.pack(">d", d))[0]


def b2d(b):
    return struct.unpack(">d", struct.pack(">Q", b))[0]


def is_nan(bits):
    return (bits & 0x7FF0000000000000) == 0x7FF0000000000000 and (bits & 0xFFFFFFFFFFFFF) != 0


def quiet(bits):
    return bits | 0x0008000000000000


def is_inf(bits):
    return (bits & 0x7FFFFFFFFFFFFFFF) == 0x7FF0000000000000


def is_zero(bits):
    return (bits & 0x7FFFFFFFFFFFFFFF) == 0


def nan2(a, b):
    """2-operand NaN propagation: frA then frB, quieted."""
    if is_nan(a):
        return quiet(a)
    if is_nan(b):
        return quiet(b)
    return None


def fp_add(a, b):
    n = nan2(a, b)
    if n is not None:
        return n
    x, y = b2d(a), b2d(b)
    if is_inf(a) and is_inf(b) and (a ^ b) >> 63:
        return QNAN                       # inf + -inf
    return d2b(x + y)


def fp_sub(a, b):
    n = nan2(a, b)
    if n is not None:
        return n
    if is_inf(a) and is_inf(b) and not ((a ^ b) >> 63):
        return QNAN                       # inf - inf
    return d2b(b2d(a) - b2d(b))


def fp_mul(a, b):
    n = nan2(a, b)
    if n is not None:
        return n
    if (is_inf(a) and is_zero(b)) or (is_zero(a) and is_inf(b)):
        return QNAN
    return d2b(b2d(a) * b2d(b))


def fp_div(a, b):
    n = nan2(a, b)
    if n is not None:
        return n
    if is_inf(a) and is_inf(b):
        return QNAN
    if is_zero(b):
        if is_zero(a):
            return QNAN                   # 0/0
        return ((a ^ b) & (1 << 63)) | 0x7FF0000000000000  # x/0 = +-inf
    return d2b(b2d(a) / b2d(b))


def frac_to_bits(fr):
    """Round an exact Fraction to the nearest IEEE-754 double (RN-even).
    Used for fused multiply-add expected values."""
    if fr == 0:
        return 0
    sign = 1 << 63 if fr < 0 else 0
    fr = abs(fr)
    # exponent e: 2^e <= fr < 2^(e+1)
    e = fr.numerator.bit_length() - fr.denominator.bit_length()
    if Fraction(2) ** e > fr:
        e -= 1
    while Fraction(2) ** (e + 1) <= fr:
        e += 1
    mant_exp = e - 52
    if e < -1022:                          # subnormal: fixed rounding position
        mant_exp = -1074
    # m = fr / 2^mant_exp, rounded half-to-even
    scaled = fr / Fraction(2) ** mant_exp
    m, frac = divmod(scaled.numerator, scaled.denominator)
    rem2 = 2 * frac
    if rem2 > scaled.denominator or (rem2 == scaled.denominator and (m & 1)):
        m += 1
    if m >= (1 << 53):                     # rounded up into next binade
        m >>= 1
        mant_exp += 1
    if m == 0:
        return sign
    if m < (1 << 52):                      # subnormal
        return sign | m
    biased = mant_exp + 52 + 1023
    if biased >= 2047:
        return sign | 0x7FF0000000000000   # overflow -> inf
    return sign | (biased << 52) | (m & ((1 << 52) - 1))


def fp_madd(a, c, b):
    """fmadd frT,frA,frC,frB = frA*frC + frB, SINGLE rounding (fused)."""
    for x in (a, c, b):
        if is_nan(x):
            return quiet(x)
    if (is_inf(a) and is_zero(c)) or (is_zero(a) and is_inf(c)):
        return QNAN
    prod_inf = is_inf(a) or is_inf(c)
    if prod_inf and is_inf(b):
        psign = (a ^ c) >> 63
        if psign != (b >> 63):
            return QNAN                   # inf - inf
    if prod_inf:
        return ((a ^ c) & (1 << 63)) | 0x7FF0000000000000
    if is_inf(b):
        return b
    fr = Fraction(b2d(a)) * Fraction(b2d(c)) + Fraction(b2d(b))
    if fr == 0:
        # exact zero: sign = product sign AND addend sign match else +0 (RN)
        psign = ((a ^ c) >> 63) & 1
        bsign = (b >> 63) & 1
        return (1 << 63) if (psign and bsign) else 0
    return frac_to_bits(fr)


def fp_frsp(a):
    if is_nan(a):
        return quiet(a)
    f = ctypes.c_float(b2d(a)).value
    return d2b(f)


def fp_fctiwz(a):
    if is_nan(a):
        return 0x80000000
    d = b2d(a)
    if is_inf(a):
        return 0x7FFFFFFF if d > 0 else 0x80000000
    t = int(d)                             # trunc toward zero
    if t > 0x7FFFFFFF:
        return 0x7FFFFFFF
    if t < -0x80000000:
        return 0x80000000
    return t & M32


def fp_fcfid(bits):
    return d2b(float(sext64(bits)))


def fp_fneg(a):
    return a ^ (1 << 63)


def fp_fabs(a):
    return a & ~(1 << 63)


def fp_fnabs(a):
    return a | (1 << 63)


def fp_fsel(a, c, b):
    """fsel frT,frA,frC,frB: frT = (frA >= 0.0) ? frC : frB. NaN frA -> frB."""
    if is_nan(a):
        return b
    return c if b2d(a) >= 0.0 else b


def fp_fcmpu(a, b):
    """CR field bits: LT=8 GT=4 EQ=2 UN=1."""
    if is_nan(a) or is_nan(b):
        return 1
    x, y = b2d(a), b2d(b)
    return 8 if x < y else (4 if x > y else 2)


# validate the Fraction rounding path against Python's own doubles
def _selfcheck():
    rnd = random.Random(99)
    for _ in range(4000):
        x = rnd.uniform(-1e300, 1e300) * (10.0 ** rnd.randint(-300, 300))
        if x != x or x in (float("inf"), float("-inf")):
            continue
        want = d2b(x)
        got = frac_to_bits(Fraction(x))
        assert got == want, f"frac_to_bits mismatch for {x}: {got:016X} != {want:016X}"
    assert frac_to_bits(Fraction(1, 10)) == d2b(0.1)
    assert frac_to_bits(Fraction(1, 1 << 1074)) == 1          # min subnormal
    assert frac_to_bits(Fraction(1, 1 << 1075)) == 0          # rounds to zero (half-even)
    assert frac_to_bits(Fraction(3, 1 << 1075)) == 2          # rounds up
    # fused-vs-unfused detector: (1+e)(1-e)-1 = -e^2 exactly
    e = 2.0 ** -52
    fused = fp_madd(d2b(1 + e), d2b(1 - e), d2b(-1.0))
    assert fused == d2b(-(2.0 ** -104)), hex(fused)
    # integer identities
    assert mask64(32, 63) == 0x00000000FFFFFFFF
    assert mask64(0, 0) == 1 << 63
    assert mask64(63, 0) == (1 << 63) | 1                     # wrap
    assert rotl32dup(0x12345678, 8) == 0x3456781234567812
    assert op_sraw(0x80000000, 1, 0)[0] == 0xFFFFFFFFC0000000
    assert op_sraw(0x80000001, 1, 0) == (0xFFFFFFFFC0000000, 1)
    assert op_sraw(0x80000000, 40, 0) == (M64, 1)             # n>=32, negative
    assert op_srad(1 << 63, 100, 0) == (M64, 1)
    assert op_cntlzw(0, 0)[0] == 32
    assert op_divw(0x80000000, M64, 0)[0] is None             # overflow case
    assert op_subfc(1, 1, 0) == (0, 1)                        # 1-1: no borrow -> CA=1
    assert op_subfc(2, 1, 0) == (M64, 0)                      # 1-2: borrow -> CA=0
    assert op_mulhd(M64, M64, 0)[0] == 0                      # (-1)*(-1) high = 0
    assert op_mulhdu(M64, M64, 0)[0] == M64 - 1


# ---------------------------------------------------------------------------
# emission
# ---------------------------------------------------------------------------

CORE8 = [0, 1, 0x7FFFFFFF, 0x80000000, (1 << 63) - 1, 1 << 63, M64,
         0xAAAA5555DEADBEEF]
EDGE1 = [0, 1, 2, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF,
         0x80000000, 0xFFFFFFFF, (1 << 63) - 1, 1 << 63, M64, 0x123456789ABCDEF0]

SETCA = {0: "li %1,0\\n\\taddic %1,%1,0",      # 0 + 0 -> CA=0
         1: "li %1,-1\\n\\taddic %1,%1,1"}     # 0xFF..F + 1 -> CA=1


class Emitter:
    def __init__(self):
        self.funcs = []          # (name, [lines])
        self.cur = None
        self.cur_name = None
        self.blocks = 0
        self.total = 0

    def begin(self, name):
        self.cur_name = name
        self.cur = []
        self.blocks = 0

    def end(self):
        if self.cur is not None:
            self.funcs.append((self.cur_name, self.cur))
            self.cur = None

    def block(self, lines):
        # split functions every 64 asm blocks to keep lifted funcs a sane size
        if self.blocks and self.blocks % 64 == 0:
            n = self.cur_name
            self.end()
            self.begin(f"{n}_{self.blocks // 64}")
        self.cur.extend(lines)
        self.blocks += 1
        self.total += 1


def emit_rr(em, rnd):
    pairs = [(a, b) for a in CORE8 for b in CORE8]
    pairs += [(rnd.getrandbits(64), rnd.getrandbits(64)) for _ in range(8)]
    for op, (fn, ca_in, mask, ca_out) in RR_OPS.items():
        em.begin(f"kat_{op}")
        cas = (0, 1) if ca_in else (0,)
        idx = 0
        for cain in cas:
            for a, b in pairs:
                want, wca = fn(a, b, cain)
                if want is None:           # undefined-result: no-crash only
                    em.block([
                        f'  {{ unsigned long long got, x;',
                        f'    __asm__ __volatile__("{op} %0,%2,%3\\n\\tmfxer %1"',
                        f'      : "=&r"(got), "=&r"(x) : "r"(0x{a:X}ULL), "r"(0x{b:X}ULL));',
                        f'    t_nocrash("{op}", {idx}); }}'])
                    idx += 1
                    continue
                setca = SETCA[cain] + "\\n\\t" if ca_in else ""
                asm = f'{setca}{op} %0,%2,%3\\n\\tmfxer %1'
                if ca_out:
                    em.block([
                        f'  {{ unsigned long long got, x;',
                        f'    __asm__ __volatile__("{asm}"',
                        f'      : "=&r"(got), "=&r"(x) : "r"(0x{a:X}ULL), "r"(0x{b:X}ULL));',
                        f'    t_kat_ca("{op}", {idx}, got & 0x{mask:X}ULL, 0x{want & mask:X}ULL, (x>>29)&1, {wca}); }}'])
                else:
                    em.block([
                        f'  {{ unsigned long long got, x;',
                        f'    __asm__ __volatile__("{asm}"',
                        f'      : "=&r"(got), "=&r"(x) : "r"(0x{a:X}ULL), "r"(0x{b:X}ULL));',
                        f'    t_kat("{op}", {idx}, got & 0x{mask:X}ULL, 0x{want & mask:X}ULL); }}'])
                idx += 1
        em.end()


def emit_dot(em, rnd):
    """Record forms: value + CR0 (lifter must derive cr0 from the 64-bit
    signed result)."""
    vals = [(0, 0), (1, M64), (M64, 1), (1 << 63, 0), ((1 << 63) - 1, 1),
            (M64, M64), (0x123456789ABCDEF0, 0xFEDCBA9876543210)]
    for op in DOT_OPS:
        fn, ca_in, mask, _ = RR_OPS[op]
        em.begin(f"kat_{op}_rec")
        idx = 0
        for a, b in vals:
            for cain in ((0, 1) if ca_in else (0,)):
                want, _ = fn(a, b, cain)
                if want is None:
                    continue
                cr0 = cr_signed(want)
                setca = SETCA[cain] + "\\n\\t" if ca_in else ""
                em.block([
                    f'  {{ unsigned long long got, c;',
                    f'    __asm__ __volatile__("{setca}{op}. %0,%2,%3\\n\\tmfcr %1"',
                    f'      : "=&r"(got), "=&r"(c) : "r"(0x{a:X}ULL), "r"(0x{b:X}ULL) : "cc");',
                    f'    t_kat_ca("{op}.", {idx}, got & 0x{mask:X}ULL, 0x{want & mask:X}ULL, (c>>28)&0xE, {cr0}); }}'])
                idx += 1
        em.end()


def emit_r1(em, rnd):
    for op, (fn, ca_in, ca_out) in R1_OPS.items():
        em.begin(f"kat_{op}")
        idx = 0
        for cain in ((0, 1) if ca_in else (0,)):
            for a in EDGE1:
                want, wca = fn(a, cain)
                setca = SETCA[cain] + "\\n\\t" if ca_in else ""
                asm = f'{setca}{op} %0,%2\\n\\tmfxer %1'
                if ca_out:
                    em.block([
                        f'  {{ unsigned long long got, x;',
                        f'    __asm__ __volatile__("{asm}"',
                        f'      : "=&r"(got), "=&r"(x) : "r"(0x{a:X}ULL));',
                        f'    t_kat_ca("{op}", {idx}, got, 0x{want:X}ULL, (x>>29)&1, {wca}); }}'])
                else:
                    em.block([
                        f'  {{ unsigned long long got, x;',
                        f'    __asm__ __volatile__("{asm}"',
                        f'      : "=&r"(got), "=&r"(x) : "r"(0x{a:X}ULL));',
                        f'    t_kat("{op}", {idx}, got, 0x{want:X}ULL); }}'])
                idx += 1
        em.end()


def emit_ri(em, rnd):
    """Immediate forms (baked into the asm string)."""
    imms = [0, 1, -1, 0x7FFF, -0x8000, 0x1234]
    uimms = [0, 1, 0xFFFF, 0x8000, 0x1234]
    args = [0, 1, M64, 0x7FFFFFFF, 0x80000000, 1 << 63, 0x123456789ABCDEF0]

    def block_ri(op, idx, a, imm, want, wca, check_ca, dot_cr=None):
        asmop = f"{op} %0,%2,{imm}"
        if dot_cr is not None:
            em.block([
                f'  {{ unsigned long long got, c;',
                f'    __asm__ __volatile__("{asmop}\\n\\tmfcr %1"',
                f'      : "=&r"(got), "=&r"(c) : "r"(0x{a:X}ULL) : "cc");',
                f'    t_kat_ca("{op}", {idx}, got, 0x{want:X}ULL, (c>>28)&0xE, {dot_cr}); }}'])
        elif check_ca:
            em.block([
                f'  {{ unsigned long long got, x;',
                f'    __asm__ __volatile__("{asmop}\\n\\tmfxer %1"',
                f'      : "=&r"(got), "=&r"(x) : "r"(0x{a:X}ULL));',
                f'    t_kat_ca("{op}", {idx}, got, 0x{want:X}ULL, (x>>29)&1, {wca}); }}'])
        else:
            em.block([
                f'  {{ unsigned long long got, x;',
                f'    __asm__ __volatile__("{asmop}\\n\\tmfxer %1"',
                f'      : "=&r"(got), "=&r"(x) : "r"(0x{a:X}ULL));',
                f'    t_kat("{op}", {idx}, got, 0x{want:X}ULL); }}'])

    em.begin("kat_addic")
    idx = 0
    for a in args:
        for imm in imms:
            want, wca = add_carry(a, sext(imm, 16) & M64)
            block_ri("addic", idx, a, imm, want, wca, True)
            idx += 1
    em.end()

    em.begin("kat_subfic")
    idx = 0
    for a in args:
        for imm in imms:
            want, wca = add_carry((~a) & M64, sext(imm, 16) & M64, 1)
            block_ri("subfic", idx, a, imm, want, wca, True)
            idx += 1
    em.end()

    em.begin("kat_mulli")
    idx = 0
    for a in args:
        for imm in imms:
            want = (sext64(a) * imm) & M64
            block_ri("mulli", idx, a, imm, want, None, False)
            idx += 1
    em.end()

    em.begin("kat_andi_rec")
    idx = 0
    for a in args:
        for imm in uimms:
            want = a & imm
            block_ri("andi.", idx, a, imm, want, None, False, dot_cr=cr_signed(want))
            idx += 1
    em.end()

    for op in ("ori", "xori"):
        em.begin(f"kat_{op}")
        idx = 0
        for a in args:
            for imm in uimms:
                want = (a | imm) if op == "ori" else (a ^ imm)
                block_ri(op, idx, a, imm, want, None, False)
                idx += 1
        em.end()

    # andis./oris/xoris: shifted immediates
    for op, f in (("oris", lambda a, i: a | (i << 16)),
                  ("xoris", lambda a, i: a ^ (i << 16))):
        em.begin(f"kat_{op}")
        idx = 0
        for a in args:
            for imm in uimms:
                block_ri(op, idx, a, imm, f(a, imm) & M64, None, False)
                idx += 1
        em.end()


def emit_rot(em, rnd):
    rotargs = [0x123456789ABCDEF0, M64, 1 << 63, 0x00000000FFFFFFFF,
               0xF0F0F0F00F0F0F0F, 1]

    # rlwinm: incl. wrap masks (MB > ME) that compilers emit for bit-clears
    em.begin("kat_rlwinm")
    idx = 0
    grid = [(0, 0, 31), (8, 0, 31), (31, 0, 31), (0, 24, 31), (0, 0, 7),
            (5, 5, 20), (0, 30, 2), (1, 31, 0), (17, 12, 25), (16, 16, 31)]
    for a in rotargs:
        for sh, mb, me in grid:
            want = rotl32dup(a, sh) & mask64(mb + 32, me + 32) \
                if mb <= me else rotl32dup(a, sh) & (mask64(mb + 32, 63) | mask64(32, me + 32))
            em.block([
                f'  {{ unsigned long long got;',
                f'    __asm__ __volatile__("rlwinm %0,%1,{sh},{mb},{me}"',
                f'      : "=r"(got) : "r"(0x{a:X}ULL));',
                f'    t_kat("rlwinm", {idx}, got, 0x{want:X}ULL); }}'])
            idx += 1
    em.end()

    # rlwimi: read-modify-write insert (also wrap-mask forms)
    em.begin("kat_rlwimi")
    idx = 0
    for a in rotargs[:4]:
        for old in (0, M64, 0xCCCCCCCCCCCCCCCC):
            for sh, mb, me in [(0, 8, 15), (8, 0, 7), (16, 16, 31), (4, 28, 3)]:
                m = mask64(mb + 32, me + 32) if mb <= me \
                    else (mask64(mb + 32, 63) | mask64(32, me + 32))
                want = (rotl32dup(a, sh) & m) | (old & ~m & M64)
                em.block([
                    f'  {{ unsigned long long got = 0x{old:X}ULL;',
                    f'    __asm__ __volatile__("rlwimi %0,%1,{sh},{mb},{me}"',
                    f'      : "+r"(got) : "r"(0x{a:X}ULL));',
                    f'    t_kat("rlwimi", {idx}, got, 0x{want:X}ULL); }}'])
                idx += 1
    em.end()

    # rld* family
    def rld_block(op, idx, a, args_str, want):
        em.block([
            f'  {{ unsigned long long got;',
            f'    __asm__ __volatile__("{op} %0,%1,{args_str}"',
            f'      : "=r"(got) : "r"(0x{a:X}ULL));',
            f'    t_kat("{op}", {idx}, got, 0x{want:X}ULL); }}'])

    em.begin("kat_rldicl")
    idx = 0
    for a in rotargs:
        for sh, mb in [(0, 0), (0, 32), (32, 32), (1, 63), (63, 0), (8, 56), (40, 24)]:
            rld_block("rldicl", idx, a, f"{sh},{mb}", rotl64(a, sh) & mask64(mb, 63))
            idx += 1
    em.end()

    em.begin("kat_rldicr")
    idx = 0
    for a in rotargs:
        for sh, me in [(0, 63), (0, 31), (32, 31), (1, 0), (63, 62), (16, 47)]:
            rld_block("rldicr", idx, a, f"{sh},{me}", rotl64(a, sh) & mask64(0, me))
            idx += 1
    em.end()

    em.begin("kat_rldic")
    idx = 0
    for a in rotargs:
        for sh, mb in [(0, 0), (8, 8), (32, 16), (1, 62), (56, 4)]:
            rld_block("rldic", idx, a, f"{sh},{mb}", rotl64(a, sh) & mask64(mb, 63 - sh))
            idx += 1
    em.end()

    em.begin("kat_rldimi")
    idx = 0
    for a in rotargs[:4]:
        for old in (0, 0x5A5A5A5A5A5A5A5A):
            for sh, mb in [(0, 32), (16, 16), (32, 0), (8, 40)]:
                m = mask64(mb, 63 - sh)
                want = (rotl64(a, sh) & m) | (old & ~m & M64)
                em.block([
                    f'  {{ unsigned long long got = 0x{old:X}ULL;',
                    f'    __asm__ __volatile__("rldimi %0,%1,{sh},{mb}"',
                    f'      : "+r"(got) : "r"(0x{a:X}ULL));',
                    f'    t_kat("rldimi", {idx}, got, 0x{want:X}ULL); }}'])
                idx += 1
    em.end()

    # variable-count rotates
    em.begin("kat_rlwnm")
    idx = 0
    for a in rotargs[:4]:
        for n in (0, 1, 17, 31, 32, 63):
            for mb, me in [(0, 31), (8, 23)]:
                want = rotl32dup(a, n & 31) & mask64(mb + 32, me + 32)
                em.block([
                    f'  {{ unsigned long long got;',
                    f'    __asm__ __volatile__("rlwnm %0,%1,%2,{mb},{me}"',
                    f'      : "=r"(got) : "r"(0x{a:X}ULL), "r"({n}ULL));',
                    f'    t_kat("rlwnm", {idx}, got, 0x{want:X}ULL); }}'])
                idx += 1
    em.end()

    # shift-immediate forms
    em.begin("kat_srawi")
    idx = 0
    for a in rotargs + [0x80000000, 0x7FFFFFFF]:
        for sh in (0, 1, 8, 31):
            want, wca = _sra(a & M32, sh, 32)
            em.block([
                f'  {{ unsigned long long got, x;',
                f'    __asm__ __volatile__("srawi %0,%2,{sh}\\n\\tmfxer %1"',
                f'      : "=&r"(got), "=&r"(x) : "r"(0x{a:X}ULL));',
                f'    t_kat_ca("srawi", {idx}, got, 0x{want:X}ULL, (x>>29)&1, {wca}); }}'])
            idx += 1
    em.end()

    em.begin("kat_sradi")
    idx = 0
    for a in rotargs + [1 << 63]:
        for sh in (0, 1, 32, 63):
            want, wca = _sra(a, sh, 64)
            em.block([
                f'  {{ unsigned long long got, x;',
                f'    __asm__ __volatile__("sradi %0,%2,{sh}\\n\\tmfxer %1"',
                f'      : "=&r"(got), "=&r"(x) : "r"(0x{a:X}ULL));',
                f'    t_kat_ca("sradi", {idx}, got, 0x{want:X}ULL, (x>>29)&1, {wca}); }}'])
            idx += 1
    em.end()


def emit_cmp(em, rnd):
    """cmp/cmpl into a non-cr0 field (tests CR field indexing) + cr-logical."""
    vals = [(0, 0), (1, 2), (2, 1), (M64, 0), (0, M64), (1 << 63, (1 << 63) - 1),
            (0x80000000, 0x7FFFFFFF), (M64, M64)]

    em.begin("kat_cmp")
    idx = 0
    for a, b in vals:
        # cmpd cr5
        sa, sb = sext64(a), sext64(b)
        f = 8 if sa < sb else (4 if sa > sb else 2)
        em.block([
            f'  {{ unsigned long long c;',
            f'    __asm__ __volatile__("cmpd cr5,%1,%2\\n\\tmfcr %0"',
            f'      : "=r"(c) : "r"(0x{a:X}ULL), "r"(0x{b:X}ULL) : "cr5");',
            f'    t_kat("cmpd_cr5", {idx}, (c>>8)&0xE, {f}); }}'])
        # cmpld cr6
        f = 8 if a < b else (4 if a > b else 2)
        em.block([
            f'  {{ unsigned long long c;',
            f'    __asm__ __volatile__("cmpld cr6,%1,%2\\n\\tmfcr %0"',
            f'      : "=r"(c) : "r"(0x{a:X}ULL), "r"(0x{b:X}ULL) : "cr6");',
            f'    t_kat("cmpld_cr6", {idx}, (c>>4)&0xE, {f}); }}'])
        # cmpw cr1 (32-bit signed views)
        sa, sb = sext32(a), sext32(b)
        f = 8 if sa < sb else (4 if sa > sb else 2)
        em.block([
            f'  {{ unsigned long long c;',
            f'    __asm__ __volatile__("cmpw cr1,%1,%2\\n\\tmfcr %0"',
            f'      : "=r"(c) : "r"(0x{a:X}ULL), "r"(0x{b:X}ULL) : "cr1");',
            f'    t_kat("cmpw_cr1", {idx}, (c>>24)&0xE, {f}); }}'])
        idx += 1
    em.end()

    # CR-logical ops: set cr0/cr1 via compares, combine into cr5, read back
    em.begin("kat_crlogic")
    idx = 0
    for crop, pyop in [("crand", lambda x, y: x & y), ("cror", lambda x, y: x | y),
                       ("crxor", lambda x, y: x ^ y), ("crnand", lambda x, y: 1 - (x & y)),
                       ("crnor", lambda x, y: 1 - (x | y)), ("creqv", lambda x, y: 1 - (x ^ y)),
                       ("crandc", lambda x, y: x & (1 - y)), ("crorc", lambda x, y: x | (1 - y))]:
        for e0, e1 in [(0, 0), (0, 1), (1, 0), (1, 1)]:
            # cr0.eq = e0 (cmpdi a,42), cr1.eq = e1
            a0 = 42 if e0 else 41
            a1 = 42 if e1 else 41
            want = pyop(e0, e1)
            # crop cr5.eq(4*5+2=22), cr0.eq(2), cr1.eq(6)
            em.block([
                f'  {{ unsigned long long c;',
                f'    __asm__ __volatile__("cmpdi cr0,%1,42\\n\\tcmpdi cr1,%2,42\\n\\t'
                f'{crop} 22,2,6\\n\\tmfcr %0"',
                f'      : "=r"(c) : "r"({a0}ULL), "r"({a1}ULL) : "cr0", "cr1", "cr5");',
                f'    t_kat("{crop}", {idx}, (c>>9)&1, {want}); }}'])
            idx += 1
    em.end()


def emit_fpu(em, rnd):
    # curated vectors: exact-representable arithmetic + specials
    D = d2b
    finite = [D(0.0), D(-0.0), D(1.0), D(-1.0), D(1.5), D(2.0), D(0.5),
              D(3.0), D(1e300), D(1e-300), D(2.0 ** -1074), D(1.7976931348623157e308)]
    inf, ninf = 0x7FF0000000000000, 0xFFF0000000000000
    nanA = 0x7FF8000000000001                    # distinct payloads: catches
    nanB = 0x7FF8000000000002                    # operand-order swaps
    specials = [inf, ninf, nanA]

    def fblock(name, idx, asmop, ins, want, mask=M64):
        """ins: list of (constraint_letter_value) as u64 bit patterns."""
        n = len(ins)
        decls = ", ".join(f"a{i} = t_b2d(0x{v:X}ULL)" for i, v in enumerate(ins))
        srcs = ", ".join(f'"f"(a{i})' for i in range(n))
        ops = ", ".join(f"%{i + 1}" for i in range(n))
        em.block([
            f'  {{ double r, {decls};',
            f'    __asm__ __volatile__("{asmop} %0,{ops}" : "=f"(r) : {srcs});',
            f'    t_kat("{name}", {idx}, t_d2b(r) & 0x{mask:X}ULL, 0x{want & mask:X}ULL); }}'])

    for name, model in [("fadd", fp_add), ("fsub", fp_sub), ("fmul", fp_mul),
                        ("fdiv", fp_div)]:
        em.begin(f"kat_{name}")
        idx = 0
        for a in finite:
            for b in finite[:8]:
                fblock(name, idx, name, [a, b], model(a, b))
                idx += 1
        for a, b in [(inf, D(1.0)), (D(1.0), inf), (inf, ninf), (inf, inf),
                     (nanA, D(1.0)), (D(1.0), nanB), (nanA, nanB),
                     (D(0.0), D(-0.0)), (D(-0.0), D(-0.0))]:
            fblock(name, idx, name, [a, b], model(a, b))
            idx += 1
        em.end()

    # fmadd: FUSED single-rounding semantics (frT = frA*frC + frB;
    # asm operand order fmadd frT,frA,frC,frB)
    em.begin("kat_fmadd")
    idx = 0
    e = 2.0 ** -52
    madd_vecs = [
        (D(1 + e), D(1 - e), D(-1.0)),          # fused: -2^-104, unfused: 0
        (D(2.0), D(3.0), D(1.0)),
        (D(1e300), D(1e300), ninf),             # product overflows: inf + -inf
        (D(1.5), D(-2.0), D(0.25)),
        (D(2.0 ** 53), D(2.0), D(1.0)),         # rounding at the ulp boundary
        (nanA, D(1.0), D(1.0)),
        (D(0.0), inf, D(1.0)),                  # 0*inf -> QNaN
    ]
    for a, c, b in madd_vecs:
        want = fp_madd(a, c, b)
        em.block([
            f'  {{ double r, fa = t_b2d(0x{a:X}ULL), fc = t_b2d(0x{c:X}ULL), fb = t_b2d(0x{b:X}ULL);',
            f'    __asm__ __volatile__("fmadd %0,%1,%2,%3" : "=f"(r) : "f"(fa), "f"(fc), "f"(fb));',
            f'    t_kat("fmadd", {idx}, t_d2b(r), 0x{want:X}ULL); }}'])
        idx += 1
    em.end()

    # sign-bit ops (must work on NaN without arithmetic)
    em.begin("kat_fsign")
    idx = 0
    for name, model in [("fneg", fp_fneg), ("fabs", fp_fabs), ("fnabs", fp_fnabs)]:
        for a in [D(1.5), D(-1.5), D(0.0), D(-0.0), inf, ninf, nanA]:
            fblock(name, idx, name, [a], model(a))
            idx += 1
    em.end()

    # frsp: double -> single rounding
    em.begin("kat_frsp")
    idx = 0
    for a in [D(1.0), D(1.5), D(0.1), D(1e300), D(-1e300), D(1e-45),
              D(2.0 ** -150), D(3.4028235677973366e38), inf, nanA]:
        fblock("frsp", idx, "frsp", [a], fp_frsp(a))
        idx += 1
    em.end()

    # fctiwz: trunc to i32, result in low 32 of the FPR store
    em.begin("kat_fctiwz")
    idx = 0
    for a in [D(0.0), D(0.9), D(-0.9), D(1.5), D(-1.5), D(2147483647.0),
              D(2147483648.0), D(-2147483648.0), D(-2147483649.0),
              D(1e300), D(-1e300), inf, ninf, nanA]:
        want = fp_fctiwz(a)
        em.block([
            f'  {{ double fa = t_b2d(0x{a:X}ULL); unsigned long long got;',
            f'    __asm__ __volatile__("fctiwz %0,%1" : "=f"(fa) : "f"(fa));',
            f'    got = t_d2b(fa);',
            f'    t_kat("fctiwz", {idx}, got & 0xFFFFFFFFULL, 0x{want:X}ULL); }}'])
        idx += 1
    em.end()

    # fcfid: i64 -> double
    em.begin("kat_fcfid")
    idx = 0
    for v in [0, 1, M64, 1 << 62, (1 << 53) + 1, 1 << 63, 0x7FFFFFFFFFFFFFFF,
              0xFFFFFFFF00000000]:
        want = fp_fcfid(v)
        em.block([
            f'  {{ double r, fv = t_b2d(0x{v:X}ULL);',
            f'    __asm__ __volatile__("fcfid %0,%1" : "=f"(r) : "f"(fv));',
            f'    t_kat("fcfid", {idx}, t_d2b(r), 0x{want:X}ULL); }}'])
        idx += 1
    em.end()

    # fcmpu into cr3 (+ NaN -> unordered)
    em.begin("kat_fcmpu")
    idx = 0
    for a, b in [(D(1.0), D(2.0)), (D(2.0), D(1.0)), (D(1.0), D(1.0)),
                 (D(0.0), D(-0.0)), (nanA, D(1.0)), (D(1.0), nanA),
                 (nanA, nanB), (inf, D(1e300)), (ninf, inf)]:
        f = fp_fcmpu(a, b)
        em.block([
            f'  {{ unsigned long long c; double fa = t_b2d(0x{a:X}ULL), fb = t_b2d(0x{b:X}ULL);',
            f'    __asm__ __volatile__("fcmpu cr3,%1,%2\\n\\tmfcr %0" : "=r"(c) : "f"(fa), "f"(fb) : "cr3");',
            f'    t_kat("fcmpu_cr3", {idx}, (c>>16)&0xF, {f}); }}'])
        idx += 1
    em.end()

    # fsel: frA >= 0 ? frC : frB  (-0.0 counts as >= 0; NaN -> frB)
    em.begin("kat_fsel")
    idx = 0
    C, B = D(111.0), D(222.0)
    for a in [D(1.0), D(-1.0), D(0.0), D(-0.0), inf, ninf, nanA]:
        want = fp_fsel(a, C, B)
        em.block([
            f'  {{ double r, fa = t_b2d(0x{a:X}ULL), fc = t_b2d(0x{C:X}ULL), fb = t_b2d(0x{B:X}ULL);',
            f'    __asm__ __volatile__("fsel %0,%1,%2,%3" : "=f"(r) : "f"(fa), "f"(fc), "f"(fb));',
            f'    t_kat("fsel", {idx}, t_d2b(r), 0x{want:X}ULL); }}'])
        idx += 1
    em.end()


HEADER = """\
/* GENERATED by tests/guest/gen_vectors.py -- DO NOT EDIT.
 * Known-answer tests: each block executes a real PPC instruction via inline
 * asm (through the lifter) and compares against the Python reference model.
 */
#include "torture.h"

static double t_b2d(unsigned long long b) { union { unsigned long long u; double d; } v; v.u = b; return v.d; }
static unsigned long long t_d2b(double d)  { union { unsigned long long u; double d; } v; v.d = d; return v.u; }

"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--output", default="torture_kats.c")
    args = ap.parse_args()

    _selfcheck()
    rnd = random.Random(0x1337)

    em = Emitter()
    emit_rr(em, rnd)
    emit_dot(em, rnd)
    emit_r1(em, rnd)
    emit_ri(em, rnd)
    emit_rot(em, rnd)
    emit_cmp(em, rnd)
    emit_fpu(em, rnd)

    with open(args.output, "w", newline="\n") as f:
        f.write(HEADER)
        for name, lines in em.funcs:
            f.write(f"static void NOINLINE {name}(void)\n{{\n")
            f.write("\n".join(lines))
            f.write("\n}\n\n")
        f.write("void kat_run_all(void)\n{\n")
        for name, _ in em.funcs:
            f.write(f'  t_section("{name}");\n  {name}();\n')
        f.write("}\n")

    print(f"wrote {args.output}: {em.total} KATs in {len(em.funcs)} functions")


if __name__ == "__main__":
    main()
