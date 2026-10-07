"""PPU instruction specs + random state generation for the conformance ELF.

Each op is encoded from its fields here (no assembler), so every operand
field can be randomized. make_cases() returns case dicts for
gen_ppu_conform.build():
  name   unique case id ("ADD#3")
  op     op name (RPCS3's PPUOpcodes.h spelling, e.g. ADD, ADDO_ for addo.)
  body   fn(asm, case) emitting the instruction(s) under test
  state  fn(case, layout) -> STATE_SIZE input bytes (needs the scratch address)
  mem    True if the case touches the scratch buffer
"""
import struct

S_GPR, S_FPR, S_VR = 0x000, 0x100, 0x200
S_CR, S_XER, S_LR, S_CTR = 0x400, 0x408, 0x410, 0x418
S_FPSCR, S_VSCR, S_VRSAVE, S_FLAG = 0x420, 0x430, 0x440, 0x444
STATE_SIZE = 0x480
SCRATCH_SIZE = 0x400

GPR_POOL = [0] + list(range(2, 31))      # never r1 (stack) or r31 (slot pointer)

# ---------------------------------------------------------------- values

SPECIAL64 = [0, 1, 2, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF, 0x80000000,
             0xFFFFFFFF, 0x100000000, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000,
             0xFFFFFFFFFFFFFFFF, 0xFFFFFFFF80000000, 0x00000000FFFF0000, 0x5555555555555555,
             0xAAAAAAAAAAAAAAAA, 0x0123456789ABCDEF]

SPECIAL_DBL = [0x0000000000000000, 0x8000000000000000, 0x3FF0000000000000, 0xBFF0000000000000,
               0x7FF0000000000000, 0xFFF0000000000000, 0x7FF8000000000000, 0xFFF8000000000000,
               0x7FF4000000000000, 0x7FF0000000000001, 0x0000000000000001, 0x800FFFFFFFFFFFFF,
               0x0010000000000000, 0x7FEFFFFFFFFFFFFF, 0x41DFFFFFFFC00000, 0x41E0000000000000,
               0xC1E0000000000000, 0x41F0000000000000, 0x43E0000000000000, 0xC3E0000000000000,
               0x3FE0000000000000, 0x3FF8000000000000, 0x4330000000000000, 0x36A0000000000000,
               0x47EFFFFFE0000000, 0x47F0000000000000, 0x3810000000000000, 0x380FFFFFFFFFFFFF,
               0x3E70000000000000]

SPECIAL_FLT = [0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x7F800000, 0xFF800000,
               0x7FC00000, 0xFFC00000, 0x7FA00000, 0x7F800001, 0x00000001, 0x807FFFFF,
               0x00800000, 0x7F7FFFFF, 0x4F000000, 0xCF000000, 0x4F800000, 0x3F000000,
               0x4B000000, 0x5F000000, 0xDF000000, 0x3FC00000]


def r64(rng):
    k = rng.random()
    if k < 0.25:
        return rng.choice(SPECIAL64)
    if k < 0.35:
        return rng.getrandbits(8) | (0xFFFFFFFFFFFFFF00 if rng.random() < .5 else 0)
    if k < 0.5:
        v = rng.getrandbits(32)
        return v | (0xFFFFFFFF00000000 if (v >> 31) and rng.random() < .7 else 0)
    return rng.getrandbits(64)


def rdbl(rng):
    k = rng.random()
    if k < 0.3:
        return rng.choice(SPECIAL_DBL)
    if k < 0.45:   # single-representable double
        f = rflt(rng)
        return struct.unpack(">Q", struct.pack(">d", struct.unpack(">f", struct.pack(">I", f))[0]))[0] \
            if (f & 0x7F800000) != 0x7F800000 else rng.choice(SPECIAL_DBL)
    if k < 0.6:    # modest magnitude
        e = rng.randrange(1023 - 40, 1023 + 40)
        return (rng.getrandbits(1) << 63) | (e << 52) | rng.getrandbits(52)
    return rng.getrandbits(64)


def rflt(rng):
    k = rng.random()
    if k < 0.3:
        return rng.choice(SPECIAL_FLT)
    if k < 0.6:
        e = rng.randrange(127 - 20, 127 + 20)
        return (rng.getrandbits(1) << 31) | (e << 23) | rng.getrandbits(23)
    return rng.getrandbits(32)


def rvec(rng, kind):
    k = rng.random()
    if kind == "f":
        return b"".join(struct.pack(">I", rflt(rng)) for _ in range(4))
    if k < 0.2:
        b = rng.choice([0x00, 0xFF, 0x7F, 0x80, 0x01])
        return bytes([b]) * 16
    if k < 0.4:
        return b"".join(struct.pack(">H", rng.choice([0, 1, 0x7FFF, 0x8000, 0xFFFF, 0x8001, rng.getrandbits(16)]))
                        for _ in range(8))
    if k < 0.55:
        return b"".join(struct.pack(">I", rng.choice(SPECIAL_FLT + [0x7FFFFFFF, 0x80000001, rng.getrandbits(32)]))
                        for _ in range(4))
    return bytes(rng.getrandbits(8) for _ in range(16))


# ---------------------------------------------------------------- state

def rand_state(rng, fp_rn=None):
    st = bytearray(STATE_SIZE)
    for r in range(32):
        struct.pack_into(">Q", st, S_GPR + 8 * r, r64(rng))
    for f in range(32):
        struct.pack_into(">Q", st, S_FPR + 8 * f, rdbl(rng))
    for v in range(32):
        st[S_VR + 16 * v:S_VR + 16 * v + 16] = rvec(rng, "f" if rng.random() < .4 else "i")
    struct.pack_into(">I", st, S_CR, rng.getrandbits(32))
    # XER: SO OV CA + byte count (bits 25..31 of the low word)
    struct.pack_into(">Q", st, S_XER, (rng.getrandbits(3) << 29) | rng.getrandbits(7))
    struct.pack_into(">Q", st, S_LR, rng.getrandbits(64))
    struct.pack_into(">Q", st, S_CTR, rng.choice([0, 1, 2, 3, 0xFFFFFFFF, 0x100000000, rng.getrandbits(64)]))
    # FPSCR: rounding mode; exceptions disabled (VE OE UE ZE XE = 0); NI off.
    # Sticky status bits random (FX OX UX ZX XX VXSNAN.. bits 0..12 minus FEX/VX summaries).
    rn = rng.randrange(4) if fp_rn is None else fp_rn
    sticky = rng.getrandbits(32) & 0x9FF80700 if rng.random() < .3 else 0
    fpscr = (sticky & ~0x000000FF) | rn
    struct.pack_into(">Q", st, S_FPSCR, fpscr)
    # VSCR: NJ (0x10000) and SAT (0x1) in the last word.
    struct.pack_into(">IIII", st, S_VSCR, 0, 0, 0,
                     (0x10000 if rng.random() < .5 else 0) | (1 if rng.random() < .3 else 0))
    struct.pack_into(">I", st, S_VRSAVE, rng.getrandbits(32))
    struct.pack_into(">I", st, S_FLAG, 0)
    return st


def set_gpr(st, r, v):
    struct.pack_into(">Q", st, S_GPR + 8 * r, v & 0xFFFFFFFFFFFFFFFF)


def pristine_scratch():
    out = bytearray(SCRATCH_SIZE)
    x = 0x9E3779B97F4A7C15
    for i in range(SCRATCH_SIZE):
        x ^= (x << 13) & 0xFFFFFFFFFFFFFFFF; x ^= x >> 7; x ^= (x << 17) & 0xFFFFFFFFFFFFFFFF
        out[i] = x & 0xFF
    # sprinkle float/double specials
    for i, v in enumerate(SPECIAL_DBL):
        struct.pack_into(">Q", out, 0x200 + 8 * i, v)
    for i, v in enumerate(SPECIAL_FLT):
        struct.pack_into(">I", out, 0x300 + 4 * i, v)
    return bytes(out)


# ---------------------------------------------------------------- encodings

def f_xo(po, xo, oe, rc):
    return lambda rt, ra, rb: (po << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (oe << 10) | (xo << 1) | rc


def make_cases(rng, scale, only):
    from ppu_ops import OPS
    cases = []
    for spec in OPS:
        if only and spec.name not in only:
            continue
        for k in range(spec.count * scale):
            c = spec.case(rng, k)
            c["name"] = "%s#%d" % (spec.name, k)
            c["op"] = spec.name
            cases.append(c)
    return cases
