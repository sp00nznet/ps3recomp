#!/usr/bin/env python3
"""PPU lifter conformance suite.

Tests the lifter's ACTUAL EMISSIONS (not a parallel implementation): each case
encodes a real instruction word, round-trips it through ppu_disasm.decode
(verifying the encoding), asks PPULifter for the C statement it would emit for
that instruction, and wraps all statements in a generated C driver that runs
them against edge-case register inputs and compares results with expectations
computed here in Python straight from the PowerISA definitions.

Why this exists: two multi-session silent-miscompile hunts (spu_disasm il
double sign-extension; cntlzw(0) = raw __builtin_clz UB that dropped every
SPURS-queue wakeup signal) were single-instruction semantics bugs that a suite
like this catches in milliseconds. Sweep the class, don't spot-fix.

Usage:
    py -3 tools\\test_ppu_lift.py            # generate + compile + run
    py -3 tools\\test_ppu_lift.py --emit     # just write the C file
The compile step needs cl on PATH (the script self-launches vcvars64 like
scratch\\dobuild2.bat when cl is absent).

Scope (tranche 1): integer ALU/rotate/shift/carry/compare classes -- the
proven silent-miscompile territory. Memory ops, FP, and VMX are follow-on
tranches (VMX semantics already have manual-verified handling; loads/stores
need a vm stub harness). A minimal vm stub now backs the FP D-form
load/store-with-update RA writeback cases below (CASES gained optional
in_mem/exp_mem/in_fpr/exp_fpr fields); it is self-contained on purpose and
can grow with later memory-op tranches.
"""

import argparse
import os
import random
import shutil
import struct
import subprocess
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

import ppu_disasm                    # noqa: E402
from ppu_lifter import (PPULifter, LiftedFunction,          # noqa: E402
                        FENCE_PREAMBLE, HEADER_PREAMBLE)

MASK64 = (1 << 64) - 1
MASK32 = (1 << 32) - 1

# ---------------------------------------------------------------------------
# Instruction encoders (big-endian words). Every encoded word is round-tripped
# through ppu_disasm.decode and the mnemonic checked, so an encoding mistake
# here is reported as ENCODING, never as a false lifter failure.
# ---------------------------------------------------------------------------

def xo_form(xo, rt, ra, rb, oe=0, rc=0):
    return (31 << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (oe << 10) | (xo << 1) | rc

def x_logic(xo, rs, ra, rb, rc=0):   # and/or/... rs sits in the rt slot
    return (31 << 26) | (rs << 21) | (ra << 16) | (rb << 11) | (xo << 1) | rc

def d_form(op, rt, ra, imm):
    return (op << 26) | (rt << 21) | (ra << 16) | (imm & 0xFFFF)

def m_form(op, rs, ra, sh, mb, me, rc=0):
    return (op << 26) | (rs << 21) | (ra << 16) | (sh << 11) | (mb << 6) | (me << 1) | rc

def md_form(xo3, rs, ra, sh, mbe, rc=0):   # rldicl/rldicr/rldic/rldimi
    return ((30 << 26) | (rs << 21) | (ra << 16) | ((sh & 31) << 11)
            | ((mbe & 31) << 6) | ((mbe >> 5) << 5) | (xo3 << 2) | ((sh >> 5) << 1) | rc)

def xs_sradi(rs, ra, sh, rc=0):
    return ((31 << 26) | (rs << 21) | (ra << 16) | ((sh & 31) << 11)
            | (413 << 2) | ((sh >> 5) << 1) | rc)

def cmp_form(xo, bf, l, ra, rb):
    return (31 << 26) | (bf << 23) | (l << 21) | (ra << 16) | (rb << 11) | (xo << 1)

def cmpi_form(op, bf, l, ra, imm):
    return (op << 26) | (bf << 23) | (l << 21) | (ra << 16) | (imm & 0xFFFF)

def vx_form(xo, vd, va, vb):
    return (4 << 26) | (vd << 21) | (va << 16) | (vb << 11) | xo

def fp_x_form(xo, bt, rc=0):   # opcd-63 X-form FPSCR bit ops (mtfsb0/mtfsb1/mtfsfi); RA/RB unused
    return (63 << 26) | (bt << 21) | (xo << 1) | rc

def spr_form(xo, rt, spr_num):   # mfspr/mtspr; the 10-bit spr field is two 5-bit halves swapped
    spr_raw = ((spr_num & 0x1F) << 5) | ((spr_num >> 5) & 0x1F)
    return (31 << 26) | (rt << 21) | (spr_raw << 11) | (xo << 1)

def sync_form(lfield):   # sync/lwsync/ptesync differ only by the 2-bit L field (bits 9..10)
    return (31 << 26) | ((lfield & 3) << 21) | (598 << 1)

EIEIO_WORD = (31 << 26) | (854 << 1)
ISYNC_WORD = (19 << 26) | (150 << 1)

# ---------------------------------------------------------------------------
# PowerISA reference semantics (independent of the lifter). 64-bit registers;
# CA per the 64-bit operation (PowerISA v2.03, the manual in the repo root).
# ---------------------------------------------------------------------------

def s64(v): v &= MASK64; return v - (1 << 64) if v >> 63 else v
def s32(v): v &= MASK32; return v - (1 << 32) if v >> 31 else v
def sxw(v): return s32(v) & MASK64          # sign-extend low word to 64

def ref_add(a, b):        return (a + b) & MASK64, None
def ref_addc(a, b):       s = a + b; return s & MASK64, s >> 64
def ref_adde(a, b, ca):   s = a + b + ca; return s & MASK64, s >> 64
def ref_addme(a, ca):     s = a + MASK64 + ca; return s & MASK64, s >> 64
def ref_addze(a, ca):     s = a + ca; return s & MASK64, s >> 64
def ref_subf(a, b):       return (b - a) & MASK64, None
def ref_subfc(a, b):      s = ((~a) & MASK64) + b + 1; return s & MASK64, s >> 64
def ref_subfe(a, b, ca):  s = ((~a) & MASK64) + b + ca; return s & MASK64, s >> 64
def ref_subfme(a, ca):    s = ((~a) & MASK64) + MASK64 + ca; return s & MASK64, s >> 64
def ref_subfze(a, ca):    s = ((~a) & MASK64) + ca; return s & MASK64, s >> 64
def ref_neg(a):           return (-a) & MASK64, None
def ref_cntlzw(a):        w = a & MASK32; return (32 if w == 0 else 31 - w.bit_length() + 1), None
def ref_cntlzd(a):        return (64 if a == 0 else 64 - a.bit_length()), None
def ref_extsb(a):         v = a & 0xFF; return (v - 0x100 if v >> 7 else v) & MASK64, None
def ref_extsh(a):         v = a & 0xFFFF; return (v - 0x10000 if v >> 15 else v) & MASK64, None
def ref_extsw(a):         return sxw(a), None
def ref_mullw(a, b):      return (s32(a) * s32(b)) & MASK64, None
def ref_mulhw(a, b):      return ((s32(a) * s32(b)) >> 32) & MASK32, None   # low32 defined; high undefined -> mask32 compare
def ref_mulhwu(a, b):     return (((a & MASK32) * (b & MASK32)) >> 32) & MASK32, None
def ref_mulld(a, b):      return (s64(a) * s64(b)) & MASK64, None
def ref_mulhd(a, b):      return ((s64(a) * s64(b)) >> 64) & MASK64, None
def ref_mulhdu(a, b):     return ((a * b) >> 64) & MASK64, None
def ref_mulli(a, imm):    return (s64(a) * imm) & MASK64, None

def rotl32(v, n): v &= MASK32; n &= 31; return ((v << n) | (v >> (32 - n))) & MASK32 if n else v
def rotl64(v, n): v &= MASK64; n &= 63; return ((v << n) | (v >> (64 - n))) & MASK64 if n else v

def mask32(mb, me):
    if mb <= me:
        return (MASK32 >> mb) & (MASK32 << (31 - me)) & MASK32
    return ((MASK32 >> mb) | (MASK32 << (31 - me))) & MASK32

def mask64(mb, me):
    if mb <= me:
        m = MASK64 if mb == 0 else (MASK64 >> mb)
        return m & ((MASK64 << (63 - me)) & MASK64)
    return ((MASK64 >> mb) | ((MASK64 << (63 - me)) & MASK64)) & MASK64

def ref_rlwinm(a, sh, mb, me):   return rotl32(a, sh) & mask32(mb, me), None
def ref_rlwimi(ra, rs, sh, mb, me):
    m = mask32(mb, me)
    return ((rotl32(rs, sh) & m) | (ra & MASK32 & ~m)) & MASK32, None   # low32 compare
def ref_rldicl(a, sh, mb): return rotl64(a, sh) & mask64(mb, 63), None
def ref_rldicr(a, sh, me): return rotl64(a, sh) & mask64(0, me), None
def ref_rldimi(ra, rs, sh, mb):
    m = mask64(mb, 63 - sh)
    return ((rotl64(rs, sh) & m) | (ra & ~m)) & MASK64, None

def ref_slw(a, b):
    n = b & 0x3F
    return 0 if n & 0x20 else ((a & MASK32) << n) & MASK32, None
def ref_srw(a, b):
    n = b & 0x3F
    return 0 if n & 0x20 else (a & MASK32) >> n, None
def ref_sld(a, b):
    n = b & 0x7F
    return 0 if n & 0x40 else (a << n) & MASK64, None
def ref_srd(a, b):
    n = b & 0x7F
    return 0 if n & 0x40 else a >> n, None

def ref_srawi(a, sh):
    rs = s32(a)
    if sh == 0:
        return sxw(a), 0
    out = (a & MASK32) & ((1 << sh) - 1)
    return (rs >> sh) & MASK64, 1 if (rs < 0 and out) else 0
def ref_sraw(a, b):
    n = b & 0x3F
    rs = s32(a)
    if n == 0:
        return sxw(a), 0
    if n >= 32:
        return (rs >> 31) & MASK64, 1 if (rs < 0) else 0
    out = (a & MASK32) & ((1 << n) - 1)
    return (rs >> n) & MASK64, 1 if (rs < 0 and out) else 0
def ref_sradi(a, sh):
    rs = s64(a)
    if sh == 0:
        return a & MASK64, 0
    out = a & ((1 << sh) - 1)
    return (rs >> sh) & MASK64, 1 if (rs < 0 and out) else 0

def ref_logic(op, a, b):
    f = {"and": a & b, "or": a | b, "xor": a ^ b, "nand": ~(a & b),
         "nor": ~(a | b), "andc": a & ~b, "orc": a | ~b, "eqv": ~(a ^ b)}[op]
    return f & MASK64, None

def ref_addi(a, imm):  return (a + imm) & MASK64, None
def ref_addic(a, imm): s = a + (imm & MASK64); return s & MASK64, s >> 64
def ref_subfic(a, imm):
    s = ((~a) & MASK64) + (imm & MASK64) + 1
    return s & MASK64, s >> 64

def ref_divw(a, b):
    d, n = s32(a), s32(b)
    if n == 0 or (d == -(1 << 31) and n == -1):
        return None, None                        # UNDEFINED: assert no-crash only
    return (int(d / n) if (d < 0) != (n < 0) else d // n) & MASK32, None
def ref_divwu(a, b):
    d, n = a & MASK32, b & MASK32
    if n == 0:
        return None, None
    return (d // n) & MASK32, None
def ref_divd(a, b):
    d, n = s64(a), s64(b)
    if n == 0 or (d == -(1 << 63) and n == -1):
        return None, None
    q = abs(d) // abs(n)
    return (q if (d < 0) == (n < 0) else -q) & MASK64, None
def ref_divdu(a, b):
    if b == 0:
        return None, None
    return (a // b) & MASK64, None

# --- vupk*: AltiVec sign-extend unpacks. Element SELECTION (which half of
# the 16 input bytes feeds the op) follows the AltiVec PEM (6-172..6-176):
# high = storage bytes 0-7, low = storage bytes 8-15. ppu_lifter.py's vupk*
# handlers now route every multi-byte lane through the vrh/vsth (halfword
# source of vupkhsh/vupklsh) and vsth (halfword result of vupkhsb/vupklsb)
# big-endian accessors, matching every other multi-byte-lane VMX handler
# (ppu: vmx element wise ops read big endian lanes fix). These references
# therefore use '>' (true big-endian) struct formats to predict the lifter's
# corrected output -- this used to be '<' (host LE), which modeled the
# pre-fix byte-reversed behavior as "correct"; it wasn't, it was the same
# class of bug this change fixes everywhere else.
def ref_vupkhsh(vb16):
    halfs = struct.unpack(">4h", vb16[0:8])          # high 4 halfwords, big-endian
    return struct.pack(">4i", *halfs)
def ref_vupklsh(vb16):
    halfs = struct.unpack(">4h", vb16[8:16])         # low 4 halfwords, big-endian
    return struct.pack(">4i", *halfs)
def ref_vupkhsb(vb16):
    bytes8 = struct.unpack(">8b", vb16[0:8])         # high 8 bytes (byte lanes: order-agnostic)
    return struct.pack(">8h", *bytes8)
def ref_vupklsb(vb16):
    bytes8 = struct.unpack(">8b", vb16[8:16])        # low 8 bytes (byte lanes: order-agnostic)
    return struct.pack(">8h", *bytes8)

def cr_nibble_signed(a, b):
    if s64(a) < s64(b): return 8
    if s64(a) > s64(b): return 4
    return 2
def cr_nibble_signed32(a, b):
    if s32(a) < s32(b): return 8
    if s32(a) > s32(b): return 4
    return 2
def cr_nibble_unsigned(a, b, bits):
    m = (1 << bits) - 1
    if (a & m) < (b & m): return 8
    if (a & m) > (b & m): return 4
    return 2

# ---------------------------------------------------------------------------
# Test-case construction
# ---------------------------------------------------------------------------

E64 = [0, 1, 2, MASK64, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000,
       0xFFFFFFFF, 0x80000000, 0x7FFFFFFF, 0x100000000,
       0x123456789ABCDEF0, 0xFEDCBA9876543210]
rng = random.Random(0x59414B5A)   # deterministic
E64 += [rng.getrandbits(64) for _ in range(4)]

CASES = []   # dicts: name, word, in_regs {idx:val}, in_ca, expects [(reg, val, mask)], exp_ca, exp_cr(nibble,pos), may_trap
             # in_mem/exp_mem {addr: bytes} and in_fpr {freg: raw64 bits} /
             # exp_fpr [(freg, raw64 bits)] back the FP update-form cases below
             # (ctx->fpr is a double[32]; bits are compared as the raw
             # IEEE-754 pattern via memcpy, not value).

def case(name, word, in_regs, expects, in_ca=None, exp_ca=None, exp_cr=None, may_trap=False,
         in_mem=None, exp_mem=None, in_fpr=None, exp_fpr=None):
    CASES.append(dict(name=name, word=word, in_regs=in_regs, expects=expects,
                      in_ca=in_ca, exp_ca=exp_ca, exp_cr=exp_cr, may_trap=may_trap,
                      in_mem=in_mem or {}, exp_mem=exp_mem or {},
                      in_fpr=in_fpr or {}, exp_fpr=exp_fpr or []))

# VMX/vector cases: unlike the GPR CASES above, a vupk-family op reads/writes
# ctx->vr[] directly (no memory load/store instructions needed to exercise
# it), so a case here just seeds one input vector register's 16 raw bytes
# and checks one output vector register's 16 raw bytes. The byte contents
# are opaque here (chosen only to span sign/lane patterns); the ref_vupk*
# functions above are what predict the lifter's actual output for a given
# input, including its big-endian element read/write convention.
#
# va_reg/va_bytes and vc_reg/vc_bytes are optional: the original harness only
# covered unary vupk* (one input, vB); binary ops (vcmpgtsw, vadduwm, ...)
# also seed vA, and the VA-form vmaddfp also seeds vC. Kept at the end of the
# signature so the existing unary vcase(...) call sites above are untouched.
VCASES = []   # dicts: name, word, vb_reg, vb_bytes, vd_reg, exp_bytes, va_reg, va_bytes, vc_reg, vc_bytes

def vcase(name, word, vb_reg, vb_bytes, vd_reg, exp_bytes,
          va_reg=None, va_bytes=None, vc_reg=None, vc_bytes=None):
    VCASES.append(dict(name=name, word=word, vb_reg=vb_reg, vb_bytes=vb_bytes,
                       vd_reg=vd_reg, exp_bytes=exp_bytes,
                       va_reg=va_reg, va_bytes=va_bytes,
                       vc_reg=vc_reg, vc_bytes=vc_bytes))

def be_w(vals):    # N x uint32 (big-endian lane values) -> 4N raw bytes
    return struct.pack(f">{len(vals)}I", *(v & 0xFFFFFFFF for v in vals))
def be_h(vals):    # N x uint16 (big-endian lane values) -> 2N raw bytes
    return struct.pack(f">{len(vals)}H", *(v & 0xFFFF for v in vals))
def be_f(vals):    # N x float (big-endian lane values) -> 4N raw bytes
    return struct.pack(f">{len(vals)}f", *vals)

def va_form(xo6, vd, va, vb, vc):   # VA-form (vmaddfp/vnmsubfp/vsel/vperm/vsldoi/...)
    return (4 << 26) | (vd << 21) | (va << 16) | (vb << 11) | (vc << 6) | xo6

def pairs(n=6):
    ps = []
    for i in range(n):
        ps.append((E64[i * 3 % len(E64)], E64[(i * 5 + 2) % len(E64)]))
    ps += [(0, 0), (MASK64, 1), (0x7FFFFFFFFFFFFFFF, 1), (0x8000000000000000, MASK64),
           (0xFFFFFFFF, 1), (0x80000000, 0x80000000)]
    return ps

def build_cases():
    R = 3, 4, 5   # rt, ra, rb

    # --- XO-form arithmetic + carry family -------------------------------
    xo_ops = [
        ("add",    266, lambda a, b, ca: ref_add(a, b)),
        ("subf",    40, lambda a, b, ca: ref_subf(a, b)),
        ("addc",    10, lambda a, b, ca: ref_addc(a, b)),
        ("subfc",    8, lambda a, b, ca: ref_subfc(a, b)),
        ("adde",   138, lambda a, b, ca: ref_adde(a, b, ca)),
        ("subfe",  136, lambda a, b, ca: ref_subfe(a, b, ca)),
        ("mullw",  235, lambda a, b, ca: ref_mullw(a, b)),
        ("mulld",  233, lambda a, b, ca: ref_mulld(a, b)),
        ("mulhw",   75, lambda a, b, ca: ref_mulhw(a, b)),
        ("mulhwu",  11, lambda a, b, ca: ref_mulhwu(a, b)),
        ("mulhd",   73, lambda a, b, ca: ref_mulhd(a, b)),
        ("mulhdu",   9, lambda a, b, ca: ref_mulhdu(a, b)),
    ]
    for name, xo, ref in xo_ops:
        uses_ca = name in ("adde", "subfe")
        sets_ca = name in ("addc", "subfc", "adde", "subfe")
        for a, b in pairs():
            for ca in ((0, 1) if uses_ca else (None,)):
                v, cout = ref(a, b, ca or 0)
                mask = MASK32 if name in ("mulhw", "mulhwu") else MASK64
                case(f"{name} a={a:#x} b={b:#x} ca={ca}",
                     xo_form(xo, R[0], R[1], R[2]),
                     {R[1]: a, R[2]: b}, [(R[0], v, mask)],
                     in_ca=ca, exp_ca=(cout if sets_ca else None))

    # --- OE-form (oe=1) encodings of adde/subfe/mulhw/mulhwu ---------------
    # This lifter does not track XER[OV]/[SO] for ANY OE-form op (see add,
    # subf, mullw, addc, subfc above and addme/subfme/subfze below, none of
    # which write OV either) -- so the OE-form must be lifted identically to
    # the plain form: same result, same XER[CA] behavior where applicable.
    # These cases encode the o/o. mnemonics (oe=1 in the XO-form word, per
    # ppu_disasm's "if oe: mne += 'o'") and reuse the plain-form reference
    # functions, since the expected result does not change with OE.
    # mulhw/mulhwu have no architected OE form at all (PowerISA gives OE only
    # to mullw/mulld); XO=75/11 with OE=1 is a reserved encoding that
    # ppu_disasm's shared decode still labels mulhwo/mulhwuo, so the case
    # here is a guard against the lifter mishandling that reserved encoding
    # (e.g. falling through to the unhandled-opcode TODO stub).
    oe_ops = [
        ("adde",   138, lambda a, b, ca: ref_adde(a, b, ca), True, True),
        ("subfe",  136, lambda a, b, ca: ref_subfe(a, b, ca), True, True),
        ("mulhw",   75, lambda a, b, ca: ref_mulhw(a, b), False, False),
        ("mulhwu",  11, lambda a, b, ca: ref_mulhwu(a, b), False, False),
    ]
    for name, xo, ref, uses_ca, sets_ca in oe_ops:
        for a, b in pairs():
            for ca in ((0, 1) if uses_ca else (None,)):
                v, cout = ref(a, b, ca or 0)
                mask = MASK32 if name in ("mulhw", "mulhwu") else MASK64
                case(f"{name}o a={a:#x} b={b:#x} ca={ca}",
                     xo_form(xo, R[0], R[1], R[2], oe=1),
                     {R[1]: a, R[2]: b}, [(R[0], v, mask)],
                     in_ca=ca, exp_ca=(cout if sets_ca else None))

    for name, xo, ref in [("addme", 234, ref_addme), ("addze", 202, ref_addze),
                          ("subfme", 232, ref_subfme), ("subfze", 200, ref_subfze)]:
        for a in E64[:8]:
            for ca in (0, 1):
                v, cout = ref(a, ca)
                case(f"{name} a={a:#x} ca={ca}",
                     xo_form(xo, R[0], R[1], 0),
                     {R[1]: a}, [(R[0], v, MASK64)], in_ca=ca, exp_ca=cout)

    for a in (0, 1, MASK64, 0x8000000000000000):
        v, _ = ref_neg(a)
        case(f"neg a={a:#x}", xo_form(104, R[0], R[1], 0), {R[1]: a}, [(R[0], v, MASK64)])

    # divides: defined-result vectors + UNDEFINED vectors as no-crash probes
    for name, xo, ref, m in [("divw", 491, ref_divw, MASK32), ("divwu", 459, ref_divwu, MASK32),
                             ("divd", 489, ref_divd, MASK64), ("divdu", 457, ref_divdu, MASK64)]:
        for a, b in [(100, 7), (MASK64, 3), (0x80000000, 0xFFFFFFFF),
                     (0x7FFFFFFF, 2), (5, MASK64)]:
            v, _ = ref(a, b)
            if v is not None:
                case(f"{name} a={a:#x} b={b:#x}", xo_form(xo, R[0], R[1], R[2]),
                     {R[1]: a, R[2]: b}, [(R[0], v, m)])
        for a, b in [(1, 0), (0x8000000000000000, MASK64), (0x80000000, 0xFFFFFFFF00000000 | MASK32)]:
            case(f"{name} UNDEF a={a:#x} b={b:#x}", xo_form(xo, R[0], R[1], R[2]),
                 {R[1]: a, R[2]: b}, [], may_trap=True)

    # --- X-form logicals / extends / counts ------------------------------
    for name, xo in [("and", 28), ("or", 444), ("xor", 316), ("nand", 476),
                     ("nor", 124), ("andc", 60), ("orc", 412), ("eqv", 284)]:
        for a, b in pairs(3):
            v, _ = ref_logic(name, a, b)
            case(f"{name} a={a:#x} b={b:#x}", x_logic(xo, R[0], R[1], R[2]),
                 {R[0]: a, R[2]: b}, [(R[1], v, MASK64)])

    for name, xo, ref in [("extsb", 954, ref_extsb), ("extsh", 922, ref_extsh),
                          ("extsw", 986, ref_extsw),
                          ("cntlzw", 26, ref_cntlzw), ("cntlzd", 58, ref_cntlzd)]:
        for a in [0, 1, 0x80, 0x7F, 0xFF, 0x8000, 0xFFFF, 0x80000000, MASK32,
                  0x8000000000000000, MASK64, 0x40000000, 0x0000000100000000]:
            v, _ = ref(a)
            case(f"{name} a={a:#x}", x_logic(xo, R[0], R[1], 0),
                 {R[0]: a}, [(R[1], v, MASK64)])

    # --- shifts -----------------------------------------------------------
    for name, xo, ref in [("slw", 24, ref_slw), ("srw", 536, ref_srw),
                          ("sld", 27, ref_sld), ("srd", 539, ref_srd)]:
        for a in (0x1, 0x80000000, MASK32, 0x8000000000000000, MASK64):
            for n in (0, 1, 31, 32, 33, 63, 64):
                v, _ = ref(a, n)
                case(f"{name} a={a:#x} n={n}", x_logic(xo, R[0], R[1], R[2]),
                     {R[0]: a, R[2]: n}, [(R[1], v, MASK64)])

    for a in (0x1, 0x80000000, 0xFFFFFFFF, 0x80000001, 0x7FFFFFFF, 0xFFFFFFFF80000000):
        for sh in (0, 1, 4, 31):
            v, ca = ref_srawi(a, sh)
            case(f"srawi a={a:#x} sh={sh}",
                 (31 << 26) | (R[0] << 21) | (R[1] << 16) | (sh << 11) | (824 << 1),
                 {R[0]: a}, [(R[1], v, MASK64)], exp_ca=ca)
        for n in (0, 1, 31, 32, 40):
            v, ca = ref_sraw(a, n)
            case(f"sraw a={a:#x} n={n}", x_logic(792, R[0], R[1], R[2]),
                 {R[0]: a, R[2]: n}, [(R[1], v, MASK64)], exp_ca=ca)
    for a in (0x1, 0x8000000000000000, MASK64, 0x8000000000000001):
        for sh in (0, 1, 32, 63):
            v, ca = ref_sradi(a, sh)
            case(f"sradi a={a:#x} sh={sh}", xs_sradi(R[0], R[1], sh),
                 {R[0]: a}, [(R[1], v, MASK64)], exp_ca=ca)

    # --- rotates ----------------------------------------------------------
    for a in (0x12345678, 0x80000001, MASK32, 0xFEDCBA9876543210):
        for sh, mb, me in [(0, 0, 31), (1, 0, 31), (13, 5, 20), (16, 16, 31),
                           (4, 28, 3), (31, 31, 0), (0, 31, 31)]:   # incl. mb>me wraps
            v, _ = ref_rlwinm(a, sh, mb, me)
            case(f"rlwinm a={a:#x} sh={sh} mb={mb} me={me}",
                 m_form(21, R[0], R[1], sh, mb, me),
                 {R[0]: a}, [(R[1], v, MASK32)])
            r0 = 0xAAAAAAAABBBBBBBB
            v2, _ = ref_rlwimi(r0, a, sh, mb, me)
            case(f"rlwimi a={a:#x} sh={sh} mb={mb} me={me}",
                 m_form(20, R[0], R[1], sh, mb, me),
                 {R[0]: a, R[1]: r0}, [(R[1], v2, MASK32)])
    for a in (0x123456789ABCDEF0, 0x8000000000000001, MASK64):
        for sh, mbe in [(0, 0), (0, 63), (1, 0), (17, 5), (32, 31), (63, 1), (40, 63)]:
            v, _ = ref_rldicl(a, sh, mbe)
            case(f"rldicl a={a:#x} sh={sh} mb={mbe}", md_form(0, R[0], R[1], sh, mbe),
                 {R[0]: a}, [(R[1], v, MASK64)])
            v, _ = ref_rldicr(a, sh, mbe)
            case(f"rldicr a={a:#x} sh={sh} me={mbe}", md_form(1, R[0], R[1], sh, mbe),
                 {R[0]: a}, [(R[1], v, MASK64)])
        for sh, mb in [(8, 4), (0, 0), (32, 16)]:
            if mb <= 63 - sh:
                r0 = 0xCCCCCCCCDDDDDDDD
                v, _ = ref_rldimi(r0, a, sh, mb)
                case(f"rldimi a={a:#x} sh={sh} mb={mb}", md_form(3, R[0], R[1], sh, mb),
                     {R[0]: a, R[1]: r0}, [(R[1], v, MASK64)])

    # --- D-form immediates (negative-immediate class) ---------------------
    for a in (0, 5, MASK64, 0x7FFFFFFFFFFFFFFF):
        for imm in (0, 1, -1, -4, 0x7FFF, -0x8000):
            v, _ = ref_addi(a, imm)
            case(f"addi a={a:#x} imm={imm}", d_form(14, R[0], R[1], imm),
                 {R[1]: a}, [(R[0], v, MASK64)])
            v, _ = ref_addi(a, imm << 16)
            case(f"addis a={a:#x} imm={imm}", d_form(15, R[0], R[1], imm),
                 {R[1]: a}, [(R[0], v, MASK64)])
            v, ca = ref_addic(a, imm if imm >= 0 else (imm & MASK64))
            case(f"addic a={a:#x} imm={imm}", d_form(12, R[0], R[1], imm),
                 {R[1]: a}, [(R[0], v, MASK64)], exp_ca=ca)
            v, ca = ref_subfic(a, imm if imm >= 0 else (imm & MASK64))
            case(f"subfic a={a:#x} imm={imm}", d_form(8, R[0], R[1], imm),
                 {R[1]: a}, [(R[0], v, MASK64)], exp_ca=ca)
            v, _ = ref_mulli(a, imm)
            case(f"mulli a={a:#x} imm={imm}", d_form(7, R[0], R[1], imm),
                 {R[1]: a}, [(R[0], v, MASK64)])

    # --- compares (CR field placement; cr0 and cr7) -----------------------
    for a, b in [(0, 0), (1, 2), (2, 1), (MASK64, 1), (0x80000000, 0x7FFFFFFF),
                 (0xFFFFFFFF80000000, 0x7FFFFFFF)]:
        for bf in (0, 7):
            shift = 28 - bf * 4
            case(f"cmpd bf={bf} a={a:#x} b={b:#x}", cmp_form(0, bf, 1, R[1], R[2]),
                 {R[1]: a, R[2]: b}, [], exp_cr=(cr_nibble_signed(a, b), shift))
            case(f"cmpw bf={bf} a={a:#x} b={b:#x}", cmp_form(0, bf, 0, R[1], R[2]),
                 {R[1]: a, R[2]: b}, [], exp_cr=(cr_nibble_signed32(a, b), shift))
            case(f"cmpld bf={bf} a={a:#x} b={b:#x}", cmp_form(32, bf, 1, R[1], R[2]),
                 {R[1]: a, R[2]: b}, [], exp_cr=(cr_nibble_unsigned(a, b, 64), shift))
            case(f"cmplw bf={bf} a={a:#x} b={b:#x}", cmp_form(32, bf, 0, R[1], R[2]),
                 {R[1]: a, R[2]: b}, [], exp_cr=(cr_nibble_unsigned(a, b, 32), shift))
        case(f"cmpdi a={a:#x}", cmpi_form(11, 0, 1, R[1], 1),
             {R[1]: a}, [], exp_cr=(cr_nibble_signed(a, 1), 28))
        case(f"cmpwi a={a:#x}", cmpi_form(11, 0, 0, R[1], 1),
             {R[1]: a}, [], exp_cr=(cr_nibble_signed32(a, 1), 28))

    # --- Rc=1 CR0 recording ------------------------------------------------
    for a, b in [(1, 1), (0, 0), (MASK64, 1), (0x8000000000000000, 0)]:
        v, _ = ref_add(a, b)
        nib = 8 if s64(v) < 0 else (4 if s64(v) > 0 else 2)
        case(f"add. a={a:#x} b={b:#x}", xo_form(266, R[0], R[1], R[2], rc=1),
             {R[1]: a, R[2]: b}, [(R[0], v, MASK64)], exp_cr=(nib, 28))

    # --- FP D-form load/store-with-update RA writeback ---------------------
    # lfsu/lfdu/stfsu/stfdu (opcodes 49/51/53/55) sat in the plain lfs/lfd/
    # stfs/stfd handling, so the access happened but RA never got EA written
    # back. PowerISA_V2.03_Final_Public.pdf p.129 (Load Floating-Point Single
    # with Update), p.130 (...Double with Update), p.132 (Store
    # Floating-Point Single with Update) and p.133 (...Double with Update)
    # all give RA <- EA as part of the update form and all four state RA=0
    # is itself an invalid instruction on these forms, so no rA=0 guard
    # belongs in the update handlers.
    pi_bits = struct.unpack(">Q", struct.pack(">d", 3.14159265358979))[0]
    fp_val = 2.5   # exactly representable in both float and double
    fp_double_bits = struct.unpack(">Q", struct.pack(">d", fp_val))[0]
    fp_float_bytes = struct.pack(">f", fp_val)

    # lfsu f7,0x10(r3): single-load-with-update -- checks the widened double
    # bits AND the updated RA.
    case("lfsu loaded single widened and ra writeback", d_form(49, 7, 3, 0x10),
         {3: 0x4000}, [(3, 0x4010, MASK64)],
         in_mem={0x4010: fp_float_bytes},
         exp_fpr=[(7, fp_double_bits)])

    # lfdu f8,0x40(r4): checks BOTH the loaded double bits and the updated RA.
    case("lfdu loaded double and ra writeback", d_form(51, 8, 4, 0x40),
         {4: 0x5000}, [(4, 0x5040, MASK64)],
         in_mem={0x5040: struct.pack(">Q", pi_bits)},
         exp_fpr=[(8, pi_bits)])

    # stfsu f9,0x30(r5): checks BOTH the stored float bytes and the updated RA.
    case("stfsu stored float and ra writeback", d_form(53, 9, 5, 0x30),
         {5: 0x6000}, [(5, 0x6030, MASK64)],
         in_fpr={9: fp_double_bits},
         exp_mem={0x6030: fp_float_bytes})

    # stfdu f11,0x20(r12): double-store-with-update -- checks BOTH the stored
    # double bytes and the updated RA.
    case("stfdu stored double and ra writeback", d_form(55, 11, 12, 0x20),
         {12: 0x8000}, [(12, 0x8020, MASK64)],
         in_fpr={11: pi_bits},
         exp_mem={0x8020: struct.pack(">Q", pi_bits)})

    # lfd f10,0x20(r6): non-update regression check -- RA must be left alone.
    case("lfd non-update leaves ra untouched", d_form(50, 10, 6, 0x20),
         {6: 0x7000}, [(6, 0x7000, MASK64)],
         in_mem={0x7020: struct.pack(">Q", pi_bits)},
         exp_fpr=[(10, pi_bits)])

    # --- Memory barriers -------------------------------------------------
    # What a barrier is FOR cannot be tested here: this driver is one thread,
    # and a fence's whole effect is on what a second one observes. What can be
    # tested, and is what regressed the day these were no-ops, is the rest of
    # it: that the barrier reaches the compiler as a real fence (checked as
    # text by check_barrier_lowering below and asserted again against the
    # generated driver in emit_c), that the fence COMPILES in the same
    # translation unit as the lifted code on every host toolchain, that it
    # runs, and that it changes no architected state on the way past. The last
    # is the trap a hand-written fence macro falls into -- one that clobbers a
    # scratch register, or that a compiler turns into a call with its own
    # side effects, would corrupt the guest for the sake of ordering it.
    for nm, word in (("sync", sync_form(0)), ("lwsync", sync_form(1)),
                     ("ptesync", sync_form(2)), ("eieio", EIEIO_WORD),
                     ("isync", ISYNC_WORD)):
        case(f"{nm} leaves gprs, ca and cr alone", word,
             {3: 0x0123456789ABCDEF, 4: MASK64, 5: 0},
             [(3, 0x0123456789ABCDEF, MASK64), (4, MASK64, MASK64),
              (5, 0, MASK64)],
             in_ca=1, exp_ca=1, exp_cr=(0, 28))

build_cases()

def build_vcases():
    VD, VB = 2, 5   # arbitrary distinct vector registers; VD != VB probes aliasing-free operation

    # Input vectors span: all-positive, all-negative (top bit set in every
    # lane), and a mixed-sign lane pattern, so each unpack's sign-extension
    # is exercised on both a 0-extend and a 1-extend path per lane.
    vb_h_pos = bytes(range(1, 17))                                    # 8 positive halfwords
    vb_h_neg = bytes([0xFF, 0x80] * 8)                                 # 8 negative halfwords (-128)
    vb_h_mix = struct.pack(">8h", 1, -1, 0x7FFF, -0x8000, 0, -2, 100, -100)
    vb_b_pos = bytes(range(1, 17))                                     # 16 positive bytes
    vb_b_neg = bytes([0x80] * 16)                                      # 16 negative bytes (-128)
    vb_b_mix = bytes([1, 0xFF, 0x7F, 0x80, 0, 2, 0xFE, 3,
                      0x81, 0x01, 0x7E, 0x00, 0xFF, 0x02, 0x80, 0x10])

    for label, vb in [("pos", vb_h_pos), ("neg", vb_h_neg), ("mix", vb_h_mix)]:
        vcase(f"vupkhsh {label}", vx_form(590, VD, 0, VB), VB, vb, VD, ref_vupkhsh(vb))
        vcase(f"vupklsh {label}", vx_form(718, VD, 0, VB), VB, vb, VD, ref_vupklsh(vb))
    for label, vb in [("pos", vb_b_pos), ("neg", vb_b_neg), ("mix", vb_b_mix)]:
        vcase(f"vupkhsb {label}", vx_form(526, VD, 0, VB), VB, vb, VD, ref_vupkhsb(vb))
        vcase(f"vupklsb {label}", vx_form(654, VD, 0, VB), VB, vb, VD, ref_vupklsb(vb))

    # --- VMX per-lane byte-order discrimination cases -----------------------
    # (ppu: vmx element wise ops read big endian lanes fix). Each pair below
    # is chosen so the PRE-FIX (native-cast) lifter and the POST-FIX
    # (vrh/vrw/vrf accessor) lifter compute DIFFERENT results -- i.e. these
    # fail on a revert of just the lifter change (see the discrimination
    # count in the PR description), unlike an equality compare or a splat,
    # where a byte-swap is either invariant or cancels out.

    # vcmpgtsw: true 256 > 2, but a native (unswapped) 32-bit read sees BE
    # bytes 00 00 01 00 / 00 00 00 02 as 0x00010000 / 0x02000000 -- 65536 >
    # 33554432 is false. This is the exact audit example. Lanes 1-3 are a
    # sanity/equal-value check (not part of the discriminating lane). xo=902
    # with Rc=0 (bit 10 of the 11-bit xo field clear) decodes as "vcmpgtsw"
    # with no dot -- the case name below must match exactly.
    vcase("vcmpgtsw mixed", vx_form(902, 2, 0, 1),
          1, be_w([2, 10, 1, 7]), 2, be_w([0xFFFFFFFF, 0, 0, 0]),
          va_reg=0, va_bytes=be_w([256, 10, 0, 7]))

    # vcmpgtsh: same 256-vs-2 audit example at halfword width (native 16-bit
    # read of BE bytes 01 00 / 00 02 sees 0x0001 / 0x0200 -- 1 > 512 is false).
    vcase("vcmpgtsh mixed", vx_form(838, 2, 0, 1),
          1, be_h([2, 10, 1, 7, 5, 5, 5, 5]), 2, be_h([0xFFFF, 0, 0, 0, 0, 0, 0, 0]),
          va_reg=0, va_bytes=be_h([256, 10, 0, 7, 5, 5, 5, 5]))

    # vadduwm: values straddling a byte boundary (0xFF + 0x01 = 0x100). A
    # native (unswapped) add sees BE bytes 00 00 00 FF / 00 00 00 01 as
    # 0xFF000000 / 0x01000000; that sum overflows 32 bits and wraps to 0,
    # not 0x100. (0x100+0x100 alone does NOT discriminate here -- neither
    # representation carries across a byte boundary for that pair, so it is
    # included only as a non-discriminating sanity lane; 0xFFFFFFFF+1 is a
    # second, stronger discriminating lane: true result 0, native-read result
    # 0xFFFFFF00.)
    vcase("vadduwm canary", vx_form(128, 2, 0, 1),
          1, be_w([0x01, 0x100, 1, 0]), 2, be_w([0x100, 0x200, 0, 0]),
          va_reg=0, va_bytes=be_w([0xFF, 0x100, 0xFFFFFFFF, 0]))

    # vmaddfp: vD = vA*vC + vB with exact 2.0-family values (2.0*3.0+1.0=7.0).
    # A native float* cast on raw-BE storage reinterprets 2.0's bytes
    # (40 00 00 00) as a subnormal near zero, so the pre-fix product/sum is
    # nowhere near 7.0. Raw encoding fields: RA=vA (mult. operand 1, "va"
    # here), RB=vC (mult. operand 2 -- the mandatory vb_reg/vb_bytes below is
    # actually the ADD operand vB; RC=vC is the optional vc_reg/vc_bytes),
    # per ppu_lifter.py's vmaddfp comment ("operand order is vD, vA, vC, vB").
    vcase("vmaddfp canary", va_form(46, 2, 0, 1, 3),
          1, be_f([1.0, 1.0, 1.0, 1.0]), 2, be_f([7.0, 7.0, 7.0, 7.0]),
          va_reg=0, va_bytes=be_f([2.0, 2.0, 2.0, 2.0]),
          vc_reg=3, vc_bytes=be_f([3.0, 3.0, 3.0, 3.0]))

    # vspltw/vsplth of a multi-byte value: included per the audit's requested
    # coverage, but these do NOT discriminate the bug (see ppu_lifter.py's
    # comment on vspltw) -- a splat only copies an existing lane's raw bytes
    # to other lanes with no arithmetic in between, so reading and writing
    # with the same (even wrong) reinterpretation cancels out and the
    # transferred bytes are correct either way. Kept green across the
    # revert-and-recount step as a deliberate non-discriminating control.
    vcase("vspltw w=1 canary", vx_form(652, 2, 1, 5),
          5, be_w([0x12345678, 0x9ABCDEF0, 0x11111111, 0x22222222]),
          2, be_w([0x9ABCDEF0] * 4))
    vcase("vsplth w=3 canary", vx_form(588, 2, 3, 5),
          5, be_h([0x1234, 0x5678, 0x9ABC, 0xDEF0, 0x1111, 0x2222, 0x3333, 0x4444]),
          2, be_h([0xDEF0] * 8))

build_vcases()

# ---------------------------------------------------------------------------
# Decode/lift-only checks: FPSCR bit ops (mtfsb0/mtfsb1/mtfsfi) and the
# VRSAVE mfspr/mtspr pair. Their correct lift is a leading "/* ... */"
# comment (a visible no-op, since FPSCR/VRSAVE are unmodeled), which is
# indistinguishable from an unhandled instruction to the CASES driver above
# (its "code.startswith('/*')" skip heuristic would drop them from the
# generated C file either way), so verify them here directly against
# ppu_disasm.decode and PPULifter._translate instead.
# ---------------------------------------------------------------------------

def check_decode_and_lift():
    lifter = PPULifter()
    dummy = LiftedFunction(name="conf", start_addr=0, end_addr=0x10000)
    fails = [0]

    def check(label, word, expect_mn, operand_substr=None, code_substr=None):
        insn = ppu_disasm.decode(word, 0x30000)
        mn = insn.mnemonic.rstrip(".")
        if mn != expect_mn:
            print(f"FAIL {label}: decoded as {insn.mnemonic} {insn.operands} "
                  f"(word {word:#010x}), want {expect_mn}")
            fails[0] += 1
            return
        if operand_substr and operand_substr not in insn.operands:
            print(f"FAIL {label}: operands {insn.operands!r} missing {operand_substr!r}")
            fails[0] += 1
            return
        code = lifter._translate(insn, dummy)
        if code.startswith("/* TODO"):
            print(f"FAIL {label}: lifter fell through to the unhandled catch-all: {code}")
            fails[0] += 1
            return
        if code_substr and code_substr not in code:
            print(f"FAIL {label}: lifted code {code!r} missing {code_substr!r}")
            fails[0] += 1
            return
        print(f"[fpscr/vrsave] ok {label}: {insn.mnemonic} {insn.operands} -> {code}")

    check("mtfsb0", fp_x_form(70, 5), "mtfsb0", code_substr="unmodeled")
    check("mtfsb1", fp_x_form(38, 5), "mtfsb1", code_substr="unmodeled")
    check("mtfsfi", fp_x_form(134, 5), "mtfsfi", code_substr="unmodeled")
    check("mfspr VRSAVE", spr_form(339, 3, 256), "mfspr",
          operand_substr="VRSAVE", code_substr="ctx->gpr[3] = 0")
    check("mtspr VRSAVE", spr_form(467, 3, 256), "mtspr",
          operand_substr="VRSAVE", code_substr="unmodeled")

    n_ok = 5 - fails[0]
    print(f"[fpscr/vrsave] {n_ok} checks passed, {fails[0]} FAILED")
    return fails[0] == 0

# ---------------------------------------------------------------------------
# Memory barriers: the decode and the fence each one lowers to.
#
# The CASES driver runs a barrier and checks it disturbs nothing, which is
# necessary and nowhere near sufficient -- a barrier lowered back to a no-op
# passes every one of those checks. What says the ordering is really there is
# the text: WHICH fence each mnemonic emits, and that the three XO-598 forms
# are told apart before the lifter ever sees them. sync and lwsync are one bit
# apart in the encoding and a whole barrier apart in what they cost.
# ---------------------------------------------------------------------------

BARRIERS = [
    # word,            mnemonic,  the fence it must lower to
    (sync_form(0),     "sync",    "PPU_FENCE(seq_cst)"),
    (sync_form(1),     "lwsync",  "PPU_FENCE(acq_rel)"),
    (sync_form(2),     "ptesync", "PPU_FENCE(seq_cst)"),
    (EIEIO_WORD,       "eieio",   "PPU_FENCE(release)"),
    (ISYNC_WORD,       "isync",   "PPU_FENCE(acquire)"),
]

def check_barrier_lowering():
    lifter = PPULifter()
    dummy = LiftedFunction(name="conf", start_addr=0, end_addr=0x10000)
    fails = 0

    for word, want_mn, want_fence in BARRIERS:
        insn = ppu_disasm.decode(word, 0x30000)
        if insn.mnemonic != want_mn:
            print(f"FAIL barrier {want_mn}: word {word:#010x} decoded as "
                  f"{insn.mnemonic!r}")
            fails += 1
            continue
        code = lifter._translate(insn, dummy)
        if want_fence not in code:
            print(f"FAIL barrier {want_mn}: lifted to {code!r}, want "
                  f"{want_fence!r} -- a barrier that lowers to nothing is the "
                  f"bug this checks for")
            fails += 1
            continue
        print(f"[barriers] ok {want_mn}: {insn.mnemonic} -> {code}")

    # The macro itself has to reach the lifted code, and it has to carry both
    # spellings: the split output is C++, --single-file output and the smoke
    # title are C.
    wanted = ("define PPU_FENCE(o)", "std::atomic_thread_fence",
              "atomic_thread_fence(memory_order_##o)", "<atomic>",
              "<stdatomic.h>")
    for want in wanted:
        if want not in HEADER_PREAMBLE:
            print(f"FAIL barrier preamble: header preamble is missing {want!r}")
            fails += 1

    n = len(BARRIERS) + len(wanted)
    print(f"[barriers] {n - fails} checks passed, {fails} FAILED")
    return fails == 0

# ---------------------------------------------------------------------------
# C driver generation
# ---------------------------------------------------------------------------

def emit_c(path):
    lifter = PPULifter()
    dummy = LiftedFunction(name="conf", start_addr=0, end_addr=0x10000)
    pre = lifter._preamble_lines()
    # Replace the generated-header include with the real (small) context
    # header. PPU_FENCE goes in behind it, taken from the lifter rather than
    # written out again: this driver is the one consumer of lifted statements
    # that does NOT include a generated ppu_recomp.h, so it would otherwise be
    # the one place where the fence could be spelt differently from the code
    # under test, which is the same as not testing it.
    pre[0] = pre[0].replace('#include "ppu_recomp.h"',
                            '#include "ppu_context.h"\n#include <stdint.h>\n'
                            + FENCE_PREAMBLE)
    out = ["/* Auto-generated by tools/test_ppu_lift.py -- conformance driver. */"]
    out.append("#define _CRT_SECURE_NO_WARNINGS")
    out.extend(pre)
    out.append("""
static int g_fail = 0, g_pass = 0, g_skip = 0;
static ppu_context g_ctx;

static void check_reg(const char* name, int reg, uint64_t got, uint64_t want, uint64_t mask) {
    if ((got & mask) != (want & mask)) {
        printf("FAIL %s: r%d = 0x%016llX want 0x%016llX (mask 0x%016llX)\\n",
               name, reg, (unsigned long long)got, (unsigned long long)want,
               (unsigned long long)mask);
        g_fail++;
    } else g_pass++;
}
static void check_ca(const char* name, uint32_t xer, int want) {
    int got = (xer >> 29) & 1;
    if (got != want) { printf("FAIL %s: XER[CA] = %d want %d\\n", name, got, want); g_fail++; }
    else g_pass++;
}
static void check_cr(const char* name, uint32_t cr, int nib, int shift) {
    int got = (cr >> shift) & 0xF;
    /* only LT/GT/EQ (top 3 bits of the nibble); SO passthrough not asserted */
    if ((got & 0xE) != (nib & 0xE)) {
        printf("FAIL %s: CR nibble@%d = %X want %X\\n", name, shift, got, nib); g_fail++;
    } else g_pass++;
}
static void check_vr(const char* name, const uint8_t* got, const uint8_t* want) {
    if (memcmp(got, want, 16) != 0) {
        printf("FAIL %s: vr =", name);
        for (int i = 0; i < 16; i++) printf(" %02X", got[i]);
        printf(" want");
        for (int i = 0; i < 16; i++) printf(" %02X", want[i]);
        printf("\\n");
        g_fail++;
    } else g_pass++;
}
static void check_mem(const char* name, uint64_t addr, const uint8_t* got, const uint8_t* want, int n) {
    for (int i = 0; i < n; i++) {
        if (got[i] != want[i]) {
            printf("FAIL %s: mem[0x%llX] byte %d = 0x%02X want 0x%02X\\n",
                   name, (unsigned long long)addr, i, got[i], want[i]);
            g_fail++;
            return;
        }
    }
    g_pass++;
}
static void check_fpr(const char* name, int reg, uint64_t got, uint64_t want) {
    if (got != want) {
        printf("FAIL %s: f%d raw bits = 0x%016llX want 0x%016llX\\n",
               name, reg, (unsigned long long)got, (unsigned long long)want);
        g_fail++;
    } else g_pass++;
}
/* vm stub over a 64 KB scratch page for the FP update-form load/store cases
 * (RA writeback needs a real access to write RA back after); EAs are masked
 * into it. Widths beyond what these cases need aren't provided -- add as
 * later memory-op tranches need them. */
#include <stdlib.h>   /* MSVC _byteswap_* */
static uint8_t g_vm_stub[65536];
#define VMOFF(a) ((uint32_t)(a) & 0xFFFFu)
/* Trap guard. A "may_trap" case exists to prove lifted code does NOT fault --
 * e.g. divw by zero must be lowered to a defined result, not a hardware trap.
 * MSVC catches that with SEH. POSIX has no SEH, so arm a signal handler and
 * siglongjmp back: without this the whole run dies on the first bad case and
 * the remaining 1300 checks never report. */
#ifdef _MSC_VER
#  define CONF_TRY     __try {
#  define CONF_EXCEPT  } __except (1) {
#  define CONF_ENDTRY  }
#else
#  include <setjmp.h>
#  include <signal.h>
static sigjmp_buf g_conf_jmp;
static volatile sig_atomic_t g_conf_armed = 0;
static void conf_trap_handler(int sig) {
    (void)sig;
    if (g_conf_armed) { g_conf_armed = 0; siglongjmp(g_conf_jmp, 1); }
    abort();   /* <stdlib.h>, already included */
}
#  define CONF_TRY                                                          \
     { struct sigaction _sa, _ofpe, _osegv, _oill;                          \
       _sa.sa_handler = conf_trap_handler;                                  \
       sigemptyset(&_sa.sa_mask); _sa.sa_flags = 0;                         \
       sigaction(SIGFPE,  &_sa, &_ofpe);                                    \
       sigaction(SIGSEGV, &_sa, &_osegv);                                   \
       sigaction(SIGILL,  &_sa, &_oill);                                    \
       g_conf_armed = 1;                                                    \
       if (sigsetjmp(g_conf_jmp, 1) == 0) {
#  define CONF_EXCEPT  } else {
#  define CONF_ENDTRY                                                       \
       }                                                                    \
       g_conf_armed = 0;                                                    \
       sigaction(SIGFPE,  &_ofpe,  0);                                      \
       sigaction(SIGSEGV, &_osegv, 0);                                      \
       sigaction(SIGILL,  &_oill,  0); }
#endif
/* _byteswap_* are MSVC intrinsics; this driver is built with plain cl on
 * Windows and with clang/gcc everywhere else, so pick per compiler. */
#ifdef _MSC_VER
#  define CONF_BSWAP32 _byteswap_ulong
#  define CONF_BSWAP64 _byteswap_uint64
#else
#  define CONF_BSWAP32 __builtin_bswap32
#  define CONF_BSWAP64 __builtin_bswap64
#endif
extern "C" uint32_t vm_read32(uint64_t a) { uint32_t v; memcpy(&v, g_vm_stub + VMOFF(a), 4); return CONF_BSWAP32(v); }
extern "C" void vm_write32(uint64_t a, uint32_t v) { v = CONF_BSWAP32(v); memcpy(g_vm_stub + VMOFF(a), &v, 4); }
extern "C" uint64_t vm_read64(uint64_t a) { uint64_t v; memcpy(&v, g_vm_stub + VMOFF(a), 8); return CONF_BSWAP64(v); }
extern "C" void vm_write64(uint64_t a, uint64_t v) { v = CONF_BSWAP64(v); memcpy(g_vm_stub + VMOFF(a), &v, 8); }
""")
    out.append("int main(void) {")
    out.append("    ppu_context* ctx = &g_ctx;")
    n_encoding_skipped = 0
    for i, c in enumerate(CASES):
        insn = ppu_disasm.decode(c["word"], 0x10000 + i * 4)
        exp_mn = c["name"].split()[0].rstrip(".")
        got_mn = insn.mnemonic.rstrip(".")
        if got_mn != exp_mn:
            print(f"ENCODING mismatch for {c['name']!r}: decoded as "
                  f"{insn.mnemonic} {insn.operands} (word {c['word']:#010x}) -- case skipped")
            n_encoding_skipped += 1
            continue
        code = lifter._translate(insn, dummy)
        if code.startswith("/*"):
            print(f"UNHANDLED by lifter: {c['name']!r} -> {code[:60]} -- case skipped")
            n_encoding_skipped += 1
            continue
        nm = c["name"].replace('"', "'")
        out.append(f'    {{ /* case {i}: {nm} | {insn.mnemonic} {insn.operands} */')
        out.append("      memset(ctx, 0, sizeof(*ctx));")
        if c["in_mem"] or c["exp_mem"]:
            out.append("      memset(g_vm_stub, 0, sizeof(g_vm_stub));")
        for reg, val in c["in_regs"].items():
            out.append(f"      ctx->gpr[{reg}] = 0x{val:016X}ULL;")
        if c["in_ca"]:
            out.append("      ctx->xer |= (1u << 29);")
        for addr, blob in c["in_mem"].items():
            byts = ", ".join(f"0x{b:02X}" for b in blob)
            out.append(f"      {{ static const uint8_t _m[{len(blob)}] = {{ {byts} }}; "
                       f"memcpy(g_vm_stub + VMOFF(0x{addr:X}), _m, {len(blob)}); }}")
        for reg, bits in c["in_fpr"].items():
            out.append(f"      {{ uint64_t _b = 0x{bits:016X}ULL; memcpy(&ctx->fpr[{reg}], &_b, 8); }}")
        body = [f"        {code}"]
        for reg, val, mask in c["expects"]:
            body.append(f'        check_reg("{nm}", {reg}, ctx->gpr[{reg}], '
                        f"0x{val:016X}ULL, 0x{mask:016X}ULL);")
        for addr, blob in c["exp_mem"].items():
            byts = ", ".join(f"0x{b:02X}" for b in blob)
            body.append(f'        {{ static const uint8_t _want[{len(blob)}] = {{ {byts} }}; '
                        f'check_mem("{nm}", 0x{addr:X}ULL, g_vm_stub + VMOFF(0x{addr:X}), _want, {len(blob)}); }}')
        for reg, bits in c["exp_fpr"]:
            body.append(f'        {{ uint64_t _got; memcpy(&_got, &ctx->fpr[{reg}], 8); '
                        f'check_fpr("{nm}", {reg}, _got, 0x{bits:016X}ULL); }}')
        if c["exp_ca"] is not None:
            body.append(f'        check_ca("{nm}", ctx->xer, {int(bool(c["exp_ca"]))});')
        if c["exp_cr"] is not None:
            nib, shift = c["exp_cr"]
            body.append(f'        check_cr("{nm}", ctx->cr, {nib}, {shift});')
        if c["may_trap"]:
            out.append("      CONF_TRY")
            out.extend(body)
            out.append("        g_pass++;")
            out.append("      CONF_EXCEPT")
            out.append(f'        printf("FAIL {nm}: HOST TRAP (div?) -- lifted code must not fault\\n");')
            out.append("        g_fail++;")
            out.append("      CONF_ENDTRY")
        else:
            out.extend(body)
        out.append("    }")

    for i, c in enumerate(VCASES):
        insn = ppu_disasm.decode(c["word"], 0x20000 + i * 4)
        exp_mn = c["name"].split()[0]
        if insn.mnemonic != exp_mn:
            print(f"ENCODING mismatch for {c['name']!r}: decoded as "
                  f"{insn.mnemonic} {insn.operands} (word {c['word']:#010x}) -- case skipped")
            n_encoding_skipped += 1
            continue
        code = lifter._translate(insn, dummy)
        if code.startswith("/*"):
            print(f"UNHANDLED by lifter: {c['name']!r} -> {code[:60]} -- case skipped")
            n_encoding_skipped += 1
            continue
        nm = c["name"].replace('"', "'")
        vb_hex = ", ".join(f"0x{b:02X}" for b in c["vb_bytes"])
        want_hex = ", ".join(f"0x{b:02X}" for b in c["exp_bytes"])
        out.append(f'    {{ /* vcase {i}: {nm} | {insn.mnemonic} {insn.operands} */')
        out.append("      memset(ctx, 0, sizeof(*ctx));")
        out.append(f"      {{ uint8_t _vb[16] = {{ {vb_hex} }}; "
                    f"memcpy(&ctx->vr[{c['vb_reg']}], _vb, 16); }}")
        if c.get("va_reg") is not None:
            va_hex = ", ".join(f"0x{b:02X}" for b in c["va_bytes"])
            out.append(f"      {{ uint8_t _va[16] = {{ {va_hex} }}; "
                        f"memcpy(&ctx->vr[{c['va_reg']}], _va, 16); }}")
        if c.get("vc_reg") is not None:
            vc_hex = ", ".join(f"0x{b:02X}" for b in c["vc_bytes"])
            out.append(f"      {{ uint8_t _vc[16] = {{ {vc_hex} }}; "
                        f"memcpy(&ctx->vr[{c['vc_reg']}], _vc, 16); }}")
        out.append(f"      {code}")
        out.append(f"      {{ uint8_t _want[16] = {{ {want_hex} }}; "
                    f'check_vr("{nm}", (const uint8_t*)&ctx->vr[{c["vd_reg"]}], _want); }}')
        out.append("    }")

    out.append("""
    printf("\\n[ppu-conformance] %d checks passed, %d FAILED, %d skipped\\n",
           g_pass, g_fail, g_skip);
    return g_fail ? 1 : 0;
}
""")
    text = "\n".join(out)
    with open(path, "w") as f:
        f.write(text)
    n_total = len(CASES) + len(VCASES)
    print(f"wrote {path}: {n_total - n_encoding_skipped} cases "
          f"({n_encoding_skipped} skipped at generation)")

    # A case whose lifted code starts with a comment is dropped above with a
    # note on stdout, which is how a barrier that went back to being a no-op
    # would leave this suite green: nothing would fail, there would just be
    # five fewer cases and a line nobody reads. So say out loud how many
    # fences reached the driver, and make it an error if any is missing.
    n_fences = text.count("PPU_FENCE(seq_cst)") + text.count("PPU_FENCE(acq_rel)") \
             + text.count("PPU_FENCE(release)") + text.count("PPU_FENCE(acquire)")
    want_fences = len(BARRIERS)
    print(f"  memory barriers lowered to fences in the driver: {n_fences}")
    if n_fences < want_fences:
        print(f"FAIL barriers: {n_fences} fences in the generated driver, "
              f"want {want_fences} -- a barrier lifted back to a no-op is "
              f"dropped from this file without failing anything")
        return False
    return True

# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit", action="store_true", help="only write the C file")
    args = ap.parse_args()

    decode_ok = check_decode_and_lift()
    decode_ok = check_barrier_lowering() and decode_ok

    os.makedirs(os.path.join(ROOT, "scratch"), exist_ok=True)
    cpath = os.path.join(ROOT, "scratch", "ppu_conformance.cpp")   # preamble is C++ (extern "C")
    epath = os.path.join(ROOT, "scratch", "ppu_conformance.exe"
                         if os.name == "nt" else "ppu_conformance")
    decode_ok = emit_c(cpath) and decode_ok
    if args.emit:
        sys.exit(0 if decode_ok else 1)

    log = os.path.join(ROOT, "scratch", "ppu_conformance.log")

    # The generated driver has to be COMPILED AND RUN to be worth anything:
    # decoding and lifting alone never executes a single lifted statement, and
    # for a long time a missing compiler silently reduced this suite to that.
    # On Windows cl is usually not on PATH, so self-launch vcvars; everywhere
    # else use the system compiler directly, so CI runs the same 1300+ checks
    # on macOS and Linux rather than only on a developer's Windows box.
    if os.name == "nt":
        # Located rather than hardcoded: a literal version here silently reduced
        # this suite to compiling nothing on any machine with a different Visual
        # Studio installed, which is exactly the green-tick-meaning-nothing this
        # file's own comments warn about.
        from msvc_env import find_vcvars
        vcvars = find_vcvars() or ""
        bat = os.path.join(ROOT, "scratch", "ppu_conformance_run.bat")
        with open(bat, "w") as f:
            f.write("@echo off\n")
            f.write(f'call "{vcvars}" >nul 2>nul\n')
            f.write(f'cd /d "{ROOT}"\n')
            f.write(f'cl /nologo /O1 /W3 /I runtime\\ppu /Fe:"{epath}" "{cpath}" > "{log}" 2>&1\n')
            f.write("if errorlevel 1 exit /b 2\n")
            f.write(f'"{epath}" >> "{log}" 2>&1\n')
        rc = subprocess.run(["cmd", "/c", bat], cwd=ROOT).returncode
    else:
        cxx = (os.environ.get("CXX") or shutil.which("c++")
               or shutil.which("clang++") or shutil.which("g++"))
        if not cxx:
            rc = 2
            with open(log, "w") as f:
                f.write("no C++ compiler found (set CXX)\n")
        else:
            with open(log, "w") as f:
                cp = subprocess.run([cxx, "-std=c++17", "-O1", "-w",
                                     "-I", os.path.join("runtime", "ppu"),
                                     "-o", epath, cpath],
                                    cwd=ROOT, stdout=f, stderr=subprocess.STDOUT)
            if cp.returncode != 0:
                rc = 2
            else:
                with open(log, "a") as f:
                    rc = subprocess.run([epath], cwd=ROOT, stdout=f,
                                        stderr=subprocess.STDOUT).returncode
    tail = open(log).read() if os.path.exists(log) else ""
    fails = [ln for ln in tail.splitlines() if ln.startswith("FAIL")]
    for ln in fails[:40]:
        print(ln)
    if len(fails) > 40:
        print(f"... and {len(fails) - 40} more failures")
    for ln in tail.splitlines():
        if "[ppu-conformance]" in ln or "error C" in ln or "error:" in ln:
            print(ln)
    if rc == 2:
        # Distinguish "no compiler on PATH" from "the generated code does not
        # compile". Both used to print the same COMPILE FAILED, so a run without
        # a developer prompt looked like an environment hiccup -- and it silently
        # skipped the half of this test that RUNS the lifted code. Fifteen real
        # VMX conformance failures hid behind that for as long as it took someone
        # to run it from a vcvars shell.
        no_compiler = ("is not recognized as an internal or external command" in tail
                       or "no C++ compiler found" in tail)
        if no_compiler:
            print("SKIPPED: no C++ compiler available -- the generated code was",
                  "NOT compiled or run, so the conformance checks above did not",
                  "execute. Install a compiler (or run from a Visual Studio",
                  "developer prompt) for real coverage.")
            # Under CI a skip is a FALSE GREEN: the whole point of the run is to
            # execute the lifted code, and a tick that means "compiled nothing"
            # is worse than a red one. Locally, staying quiet is reasonable.
            if os.environ.get("CI"):
                print("...and this is CI, where skipping the only test that runs",
                      "lifted code is a failure, not a pass.")
                sys.exit(1)
            sys.exit(0 if decode_ok else 1)
        print("COMPILE FAILED (the lifted C is broken) -- see", log)
    sys.exit(0 if (rc == 0 and decode_ok) else 1)

if __name__ == "__main__":
    main()
