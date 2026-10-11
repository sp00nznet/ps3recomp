"""The PPU op table for the conformance ELF: every instruction RPCS3's PPU
decoder knows (rpcs3/Emu/Cell/PPUOpcodes.h), encoded from the ISA here.

Names follow RPCS3's spelling. Suffixes: '.' = record form (Rc=1), 'O' = OE=1
(RPCS3 handles both in one function). Excluded on purpose (see EXCLUDED): ops
that are privileged, trap, or are nondeterministic by definition.

A generator returns (words, fix, extra) where
  words  the instruction(s) under test (the harness branches nowhere else),
  fix    fn(state_bytearray, layout, case) adjusting the random input state
         (pointing address registers into the scratch buffer, branch targets),
  extra  dict merged into the case: "mem" (dump scratch), "mask" (compare masks).
"""
import struct
from ppu_isa import (GPR_POOL, rand_state, set_gpr, S_GPR, S_LR, S_CTR, S_XER, S_VSCR, S_VR,
                     SCRATCH_SIZE)

FLAG_STORE = (36 << 26) | (31 << 21) | (31 << 16) | 0x444      # stw r31,FLAG(r31)

EXCLUDED = {
    "SC": "the harness itself runs on sc (sys_tty_write / sys_process_exit)",
    "DCBI": "privileged",
    "ECIWX": "external control (optional facility, traps on Cell user mode)",
    "ECOWX": "external control (optional facility, traps on Cell user mode)",
}


class Op:
    def __init__(self, name, gen, count=40):
        self.name, self.gen, self.count = name, gen, count

    def case(self, rng, k):
        st = rand_state(rng)
        r = self.gen(rng, k)
        words, fix, extra = (r + (None, None))[:3] if isinstance(r, tuple) else (r, None, None)
        extra = extra or {}
        # VSCR: RPCS3 reads/writes it in BE word 0, the ISA says word 3 -- give both.
        vscr = st[S_VSCR + 12:S_VSCR + 16]
        st[S_VSCR:S_VSCR + 4] = vscr

        def state(c, d, st=st, fix=fix):
            s = bytearray(st)
            if fix:
                fix(s, d, c)
            return bytes(s)

        def body(t, c, words=words):
            t.emit(*words)

        out = {"body": body, "state": state, "words": list(words)}
        out.update(extra)
        return out


OPS = []


def op(name, gen, count=40):
    OPS.append(Op(name, gen, count))


# ------------------------------------------------------------------ operands

def g(rng):    return rng.choice(GPR_POOL)
def gnz(rng):  return rng.choice([r for r in GPR_POOL if r])
def fr(rng):   return rng.randrange(32)
def vr(rng):   return rng.randrange(32)
def s16(rng):  return rng.choice([0, 1, -1, 2, 0x7FFF, -0x8000, rng.randrange(-0x8000, 0x8000)]) & 0xFFFF
def u16(rng):  return rng.choice([0, 1, 0xFFFF, 0x8000, 0x7FFF, rng.getrandbits(16)])


def W(po, a=0, b=0, c=0, rest=0):
    return (po << 26) | (a << 21) | (b << 16) | (c << 11) | rest


def X31(a, b, c, xo, rc=0):
    return W(31, a, b, c, (xo << 1) | rc)


# ------------------------------------------------------------------ integer

def simple(fn):
    """gen from a word builder taking rng."""
    def gen(rng, k):
        return [fn(rng)]
    return gen


def xo_ops(name, xo, unary=False, oe=True, rcs=(0, 1)):
    for o in ((0, 1) if oe else (0,)):
        for rc in rcs:
            nm = name + ("O" if o else "") + ("." if rc else "")
            op(nm, simple(lambda rng, o=o, rc=rc: W(31, g(rng), g(rng), 0 if unary else g(rng),
                                                    (o << 10) | (xo << 1) | rc)))


for nm, xo in [("SUBFC", 8), ("ADDC", 10), ("SUBF", 40), ("SUBFE", 136), ("ADDE", 138),
               ("MULLD", 233), ("MULLW", 235), ("ADD", 266), ("DIVDU", 457), ("DIVWU", 459),
               ("DIVD", 489), ("DIVW", 491)]:
    xo_ops(nm, xo)
for nm, xo in [("NEG", 104), ("SUBFZE", 200), ("ADDZE", 202), ("SUBFME", 232), ("ADDME", 234)]:
    xo_ops(nm, xo, unary=True)
for nm, xo in [("MULHDU", 9), ("MULHWU", 11), ("MULHD", 73), ("MULHW", 75)]:
    xo_ops(nm, xo, oe=False)

# D-form arithmetic / logical immediates
op("MULLI", simple(lambda r: W(7, g(r), g(r), 0, s16(r))))
op("SUBFIC", simple(lambda r: W(8, g(r), g(r), 0, s16(r))))
op("ADDIC", simple(lambda r: W(12, g(r), g(r), 0, s16(r))))
op("ADDIC.", simple(lambda r: W(13, g(r), g(r), 0, s16(r))))
op("ADDI", simple(lambda r: W(14, g(r), g(r), 0, s16(r))))
op("ADDIS", simple(lambda r: W(15, g(r), g(r), 0, s16(r))))
for nm, po in [("ORI", 24), ("ORIS", 25), ("XORI", 26), ("XORIS", 27), ("ANDI.", 28), ("ANDIS.", 29)]:
    op(nm, simple(lambda r, po=po: W(po, g(r), g(r), 0, u16(r))))

# compares: BF (3 bits) << 2 | L in the RT field
op("CMPI", simple(lambda r: W(11, (r.randrange(8) << 2) | r.randrange(2), g(r), 0, s16(r))))
op("CMPLI", simple(lambda r: W(10, (r.randrange(8) << 2) | r.randrange(2), g(r), 0, u16(r))))
op("CMP", simple(lambda r: X31((r.randrange(8) << 2) | r.randrange(2), g(r), g(r), 0)))
op("CMPL", simple(lambda r: X31((r.randrange(8) << 2) | r.randrange(2), g(r), g(r), 32)))

# X-form logical / shifts / counts (RS in the RT slot, RA = dest)
for nm, xo, unary in [("SLW", 24, 0), ("CNTLZW", 26, 1), ("SLD", 27, 0), ("AND", 28, 0),
                      ("CNTLZD", 58, 1), ("ANDC", 60, 0), ("NOR", 124, 0), ("EQV", 284, 0),
                      ("XOR", 316, 0), ("ORC", 412, 0), ("OR", 444, 0), ("NAND", 476, 0),
                      ("SRW", 536, 0), ("SRD", 539, 0), ("SRAW", 792, 0), ("SRAD", 794, 0),
                      ("EXTSH", 922, 1), ("EXTSB", 954, 1), ("EXTSW", 986, 1)]:
    for rc in (0, 1):
        op(nm + ("." if rc else ""),
           simple(lambda r, xo=xo, rc=rc, unary=unary: X31(g(r), g(r), 0 if unary else g(r), xo, rc)))
for rc in (0, 1):
    op("SRAWI" + ("." if rc else ""), simple(lambda r, rc=rc: X31(g(r), g(r), r.randrange(32), 824, rc)))
    # sradi: XS-form, sh = sh5 (bit 1) || sh0-4 (RB slot)
    op("SRADI" + ("." if rc else ""),
       simple(lambda r, rc=rc: (lambda sh: W(31, g(r), g(r), sh & 31, (413 << 2) | ((sh >> 5) << 1) | rc))(
           r.randrange(64))))

# rotates
for rc in (0, 1):
    sfx = "." if rc else ""
    op("RLWIMI" + sfx, simple(lambda r, rc=rc: W(20, g(r), g(r), r.randrange(32), (r.randrange(32) << 6) | (r.randrange(32) << 1) | rc)))
    op("RLWINM" + sfx, simple(lambda r, rc=rc: W(21, g(r), g(r), r.randrange(32), (r.randrange(32) << 6) | (r.randrange(32) << 1) | rc)))
    op("RLWNM" + sfx, simple(lambda r, rc=rc: W(23, g(r), g(r), g(r), (r.randrange(32) << 6) | (r.randrange(32) << 1) | rc)))

    def md(xo, rc=rc):
        def f(r):
            sh, mb = r.randrange(64), r.randrange(64)
            mbf = ((mb & 31) << 1) | (mb >> 5)          # mb is stored mb0-4 || mb5 rotated
            return W(30, g(r), g(r), sh & 31, (mbf << 5) | (xo << 2) | ((sh >> 5) << 1) | rc)
        return f
    op("RLDICL" + sfx, simple(md(0)))
    op("RLDICR" + sfx, simple(md(1)))
    op("RLDIC" + sfx, simple(md(2)))
    op("RLDIMI" + sfx, simple(md(3)))

    def mds(xo, rc=rc):
        def f(r):
            mb = r.randrange(64)
            mbf = ((mb & 31) << 1) | (mb >> 5)
            return W(30, g(r), g(r), g(r), (mbf << 5) | (xo << 1) | rc)
        return f
    op("RLDCL" + sfx, simple(mds(8)))
    op("RLDCR" + sfx, simple(mds(9)))


# traps that do not trap: pick TO, then make the inputs fail every selected condition
def trap_ok(to, a, b, bits):
    m = (1 << bits) - 1
    a &= m; b &= m
    sa = a - (1 << bits) if a >> (bits - 1) else a
    sb = b - (1 << bits) if b >> (bits - 1) else b
    return not ((to & 16 and sa < sb) or (to & 8 and sa > sb) or (to & 4 and a == b) or
                (to & 2 and a < b) or (to & 1 and a > b))


def trap_gen(kind):
    def gen(rng, k):
        bits = 64 if kind in ("TD", "TDI") else 32
        ra = gnz(rng)
        for _ in range(100):
            to = rng.randrange(32)
            a = rng.getrandbits(64)
            if kind in ("TW", "TD"):
                rb = gnz(rng)
                b = a if rb == ra else rng.getrandbits(64)
                if trap_ok(to, a, b, bits):
                    w = X31(to, ra, rb, 4 if kind == "TW" else 68)
                    def fix(s, d, c, ra=ra, rb=rb, a=a, b=b):
                        set_gpr(s, ra, a); set_gpr(s, rb, b)
                    return [w], fix
            else:
                si = rng.randrange(-0x8000, 0x8000)
                if trap_ok(to, a, si & 0xFFFFFFFFFFFFFFFF, bits):
                    w = W(2 if kind == "TDI" else 3, to, ra, 0, si & 0xFFFF)
                    return [w], (lambda s, d, c, ra=ra, a=a: set_gpr(s, ra, a))
        return [W(3, 0, ra, 0, 0)]          # twi 0: never traps
    return gen


for k_ in ("TW", "TD", "TWI", "TDI"):
    op(k_, trap_gen(k_))

# ------------------------------------------------------------------ SPRs, CR

SPRS = [1, 8, 9, 256]          # XER, LR, CTR, VRSAVE


def spr_f(n):
    return ((n & 31) << 5) | (n >> 5)


op("MFSPR", simple(lambda r: X31(g(r), 0, 0, 339) | (spr_f(r.choice(SPRS)) << 11)))
op("MTSPR", simple(lambda r: X31(g(r), 0, 0, 467) | (spr_f(r.choice(SPRS)) << 11)))
# mftb: the value is time -- run it, compare nothing of rt
def mftb_gen(rng, k):
    rt = g(rng)
    tbr = rng.choice([268, 269])
    return [X31(rt, 0, 0, 371) | (spr_f(tbr) << 11)], None, {"mask": {"r%d" % rt: "0" * 16}}


op("MFTB", mftb_gen)
op("MFOCRF", simple(lambda r: X31(g(r), 0, 0, 19) | (((1 << 9) | ((1 << r.randrange(8)) << 1)) << 11)
                    if r.random() < .5 else X31(g(r), 0, 0, 19)))
op("MTOCRF", simple(lambda r: W(31, g(r), (1 << 4) if True else 0, 0, ((1 << r.randrange(8)) << 12) | (144 << 1))
                    if r.random() < .5 else W(31, g(r), 0, 0, (r.getrandbits(8) << 12) | (144 << 1))))
op("MCRF", simple(lambda r: W(19, r.randrange(8) << 2, r.randrange(8) << 2, 0, 0)))
for nm, xo in [("CRNOR", 33), ("CRANDC", 129), ("CRXOR", 193), ("CRNAND", 225), ("CRAND", 257),
               ("CREQV", 289), ("CRORC", 417), ("CROR", 449)]:
    op(nm, simple(lambda r, xo=xo: W(19, r.randrange(32), r.randrange(32), r.randrange(32), xo << 1)))

# ------------------------------------------------------------------ branches

def bo_any(rng):
    return rng.choice([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 24, 25, 26, 27])


def bc_gen(rng, k):
    """bc: LK=0 skips the flag store when taken; LK=1 only as the get-PC idiom
    (bcl BO,BI,$+4), the one shape real code links on a relative branch."""
    bo, bi = bo_any(rng), rng.randrange(32)
    return [W(16, bo, bi, 0, 8), FLAG_STORE]


def b_gen(rng, k):
    return [W(18, 0, 0, 0, 8), FLAG_STORE]


def getpc_gen(rng, k):
    """The get-PC idiom: bl $+4 / bcl 20,31,$+4 (LR = address of the next insn)."""
    if k % 2:
        return [W(16, 20, 31, 0, 4 | 1)]
    return [W(18, 0, 0, 0, 4 | 1)]


BL = lambda off: W(18, 0, 0, 0, (off & 0x3FFFFFC) | 1)
B_ = lambda off: W(18, 0, 0, 0, off & 0x3FFFFFC)
BLR = 0x4E800020


def bclr_gen(rng, k):
    """Conditional return: bl W; b over;  W: bclr BO,BI; stw flag; blr.
    The LK=1 form (bclrl) calls through LR to a function entry instead."""
    bo, bi = bo_any(rng), rng.randrange(32)
    if rng.random() < .3:
        # bclrl with LR -> H:  T: bclrl ; T+4: b +12 ; H=T+8: stw flag ; blr
        w = W(19, bo, bi, 0, (16 << 1) | 1)
        def fix(s, d, c):
            struct.pack_into(">Q", s, S_LR, c["pc"] + 8)
        return [w, B_(12), FLAG_STORE, BLR], fix, {"funcs": [8]}
    w = W(19, bo, bi, 0, 16 << 1)
    return [BL(8), B_(16), w, FLAG_STORE, BLR], None, {"funcs": [8]}


def bcctr_gen(rng, k):
    """bcctr never decrements (BO bit 0x4 set). LK=0: a tail call from a helper
    (bl W; b over; W: bcctr; blr; H: stw flag; blr). LK=1: a call (bcctrl; b over;
    H: stw flag; blr)."""
    bo, bi = rng.choice([4, 5, 6, 7, 12, 13, 14, 15, 20, 21]), rng.randrange(32)
    if rng.random() < .5:
        w = W(19, bo, bi, 0, (528 << 1) | 1)
        def fix(s, d, c):
            struct.pack_into(">Q", s, S_CTR, c["pc"] + 8)
        return [w, B_(12), FLAG_STORE, BLR], fix, {"funcs": [8]}
    w = W(19, bo, bi, 0, 528 << 1)
    def fix(s, d, c):
        struct.pack_into(">Q", s, S_CTR, c["pc"] + 16)
    return [BL(8), B_(20), w, BLR, FLAG_STORE, BLR], fix, {"funcs": [8, 16]}


op("BC", bc_gen, count=80)
op("B", b_gen)
op("BL_GETPC", getpc_gen, count=10)
op("BCLR", bclr_gen, count=80)
op("BCCTR", bcctr_gen, count=60)


# ------------------------------------------------------------------ memory

def ea_regs(rng, rt=None, update=False, xform=False, avoid=()):
    """Pick RA (and RB) for an access; returns (ra, rb, fix(st, base_ea))."""
    bad = set(avoid) | ({rt} if update and rt is not None else set())
    if xform:
        if not update and rng.random() < .25:
            ra = 0
        else:
            ra = rng.choice([r for r in GPR_POOL if r and r not in bad])
        rb = rng.choice([r for r in GPR_POOL if r != ra or ra == 0])
        if rb == ra:
            rb = rng.choice([r for r in GPR_POOL if r and r != ra])
        return ra, rb
    ra = rng.choice([r for r in GPR_POOL if r and r not in bad])
    return ra, None


def dmem(po, size, update=False, ds_xo=None, fp=False, vec=False):
    def gen(rng, k):
        rt = fr(rng) if fp else g(rng)
        ra, _ = ea_regs(rng, None if fp else rt, update)
        disp = rng.randrange(-0x40, 0x40)
        if ds_xo is not None:
            disp &= ~3
        off = rng.randrange(0x40, SCRATCH_SIZE - 0x40 - size) & (~3 if rng.random() < .7 else ~0)
        if ds_xo is not None:
            w = W(po, rt, ra, 0, (disp & 0xFFFC) | ds_xo)
        else:
            w = W(po, rt, ra, 0, disp & 0xFFFF)
        def fix(s, d, c):
            set_gpr(s, ra, d["scratch"] + off - disp)
        return [w], fix, {"mem": True}
    return gen


for nm, po, sz, up in [("LWZ", 32, 4, 0), ("LWZU", 33, 4, 1), ("LBZ", 34, 1, 0), ("LBZU", 35, 1, 1),
                       ("STW", 36, 4, 0), ("STWU", 37, 4, 1), ("STB", 38, 1, 0), ("STBU", 39, 1, 1),
                       ("LHZ", 40, 2, 0), ("LHZU", 41, 2, 1), ("LHA", 42, 2, 0), ("LHAU", 43, 2, 1),
                       ("STH", 44, 2, 0), ("STHU", 45, 2, 1)]:
    op(nm, dmem(po, sz, bool(up)))
for nm, po, sz, up in [("LFS", 48, 4, 0), ("LFSU", 49, 4, 1), ("LFD", 50, 8, 0), ("LFDU", 51, 8, 1),
                       ("STFS", 52, 4, 0), ("STFSU", 53, 4, 1), ("STFD", 54, 8, 0), ("STFDU", 55, 8, 1)]:
    op(nm, dmem(po, sz, bool(up), fp=True))
op("LD", dmem(58, 8, ds_xo=0))
op("LDU", dmem(58, 8, True, ds_xo=1))
op("LWA", dmem(58, 4, ds_xo=2))
op("STD", dmem(62, 8, ds_xo=0))
op("STDU", dmem(62, 8, True, ds_xo=1))


def xmem(xo, size, update=False, fp=False, vec=False, extra=None):
    def gen(rng, k):
        rt = vr(rng) if vec else fr(rng) if fp else g(rng)
        ra, rb = ea_regs(rng, None if (fp or vec) else rt, update, xform=True)
        off = rng.randrange(0x40, SCRATCH_SIZE - 0x40 - size)
        if rng.random() < .7:
            off &= ~(min(size, 16) - 1)
        idx = rng.randrange(-0x40, 0x40)
        w = X31(rt, ra, rb, xo)
        def fix(s, d, c):
            ea = d["scratch"] + off
            if ra == 0:
                set_gpr(s, rb, ea)
            else:
                set_gpr(s, rb, idx)
                set_gpr(s, ra, ea - idx)
        ex = {"mem": True}
        if extra:
            ex.update(extra(rt, off))
        return [w], fix, ex
    return gen


for nm, xo, sz, up in [("LWZX", 23, 4, 0), ("LWZUX", 55, 4, 1), ("LBZX", 87, 1, 0), ("LBZUX", 119, 1, 1),
                       ("LHZX", 279, 2, 0), ("LHZUX", 311, 2, 1), ("LHAX", 343, 2, 0), ("LHAUX", 375, 2, 1),
                       ("LWAX", 341, 4, 0), ("LWAUX", 373, 4, 1), ("LDX", 21, 8, 0), ("LDUX", 53, 8, 1),
                       ("STWX", 151, 4, 0), ("STWUX", 183, 4, 1), ("STBX", 215, 1, 0), ("STBUX", 247, 1, 1),
                       ("STHX", 407, 2, 0), ("STHUX", 439, 2, 1), ("STDX", 149, 8, 0), ("STDUX", 181, 8, 1),
                       ("LWBRX", 534, 4, 0), ("LHBRX", 790, 2, 0), ("LDBRX", 532, 8, 0),
                       ("STWBRX", 662, 4, 0), ("STHBRX", 918, 2, 0), ("STDBRX", 660, 8, 0)]:
    op(nm, xmem(xo, sz, bool(up)))
for nm, xo, sz, up in [("LFSX", 535, 4, 0), ("LFSUX", 567, 4, 1), ("LFDX", 599, 8, 0), ("LFDUX", 631, 8, 1),
                       ("STFSX", 663, 4, 0), ("STFSUX", 695, 4, 1), ("STFDX", 727, 8, 0), ("STFDUX", 759, 8, 1),
                       ("STFIWX", 983, 4, 0)]:
    op(nm, xmem(xo, sz, bool(up), fp=True))


def lve_mask(size):
    def m(vt, off):
        # only the addressed element is defined after lve*x
        b = off & 15 & ~(size - 1)
        mask = ["00"] * 16
        for i in range(b, b + size):
            mask[i] = "ff"
        return {"mask": {"v%d" % vt: "".join(mask)}}
    return m


for nm, xo, sz, ex in [("LVX", 103, 16, None), ("LVXL", 359, 16, None), ("STVX", 231, 16, None),
                       ("STVXL", 487, 16, None), ("LVEBX", 7, 1, lve_mask(1)), ("LVEHX", 39, 2, lve_mask(2)),
                       ("LVEWX", 71, 4, lve_mask(4)), ("STVEBX", 135, 1, None), ("STVEHX", 167, 2, None),
                       ("STVEWX", 199, 4, None), ("LVSL", 6, 1, None), ("LVSR", 38, 1, None),
                       ("LVLX", 519, 1, None), ("LVLXL", 775, 1, None), ("LVRX", 551, 1, None),
                       ("LVRXL", 807, 1, None), ("STVLX", 647, 1, None), ("STVLXL", 903, 1, None),
                       ("STVRX", 679, 1, None), ("STVRXL", 935, 1, None)]:
    op(nm, xmem(xo, sz, vec=True, extra=ex))

# cache / sync: no architected effect except dcbz (zeroes the 128-byte block)
for nm, xo in [("DCBST", 54), ("DCBF", 86), ("DCBTST", 246), ("DCBT", 278), ("ICBI", 982), ("DCBZ", 1014)]:
    def cgen(rng, k, xo=xo):
        ra, rb = ea_regs(rng, xform=True)
        off = rng.randrange(0, SCRATCH_SIZE)
        idx = rng.randrange(-0x40, 0x40)
        def fix(s, d, c):
            ea = d["scratch"] + off
            if ra == 0:
                set_gpr(s, rb, ea)
            else:
                set_gpr(s, rb, idx); set_gpr(s, ra, ea - idx)
        return [X31(0, ra, rb, xo)], fix, {"mem": True}
    op(nm, cgen)
op("SYNC", simple(lambda r: X31(r.randrange(2), 0, 0, 598)))
op("EIEIO", simple(lambda r: X31(0, 0, 0, 854)))
op("ISYNC", simple(lambda r: W(19, 0, 0, 0, 150 << 1)))
op("DST", simple(lambda r: X31((r.randrange(2) << 4) | r.randrange(4), gnz(r), g(r), 342)))
op("DSTST", simple(lambda r: X31((r.randrange(2) << 4) | r.randrange(4), gnz(r), g(r), 374)))
op("DSS", simple(lambda r: X31((r.randrange(2) << 4) | r.randrange(4), 0, 0, 822)))


# load/store multiple: lmw clobbers rt..r31, so plant the slot pointer where r31 reloads from
def lmw_gen(store):
    def gen(rng, k):
        rt = rng.randrange(2 if store else 3, 31)
        ra = rng.choice([r for r in GPR_POOL if r and (store or r < rt)])
        n = 32 - rt
        disp = rng.randrange(-0x20, 0x20) & ~3
        off = rng.randrange(0x40, SCRATCH_SIZE - 0x40 - 4 * n) & ~3
        words = []
        if not store:
            words.append(W(36, 31, ra, 0, (disp + 4 * (31 - rt)) & 0xFFFF))     # stw r31 -> r31's slot
        words.append(W(47 if store else 46, rt, ra, 0, disp & 0xFFFF))
        def fix(s, d, c):
            set_gpr(s, ra, d["scratch"] + off - disp)
        return words, fix, {"mem": True}
    return gen


op("LMW", lmw_gen(False))
op("STMW", lmw_gen(True))


# string ops: keep rt..rt+regs-1 inside r2..r30 (no wrap through r31/r0/r1)
def lsw_gen(kind):
    def gen(rng, k):
        nb = rng.randrange(1, 33)
        regs = (nb + 3) // 4
        rt = rng.randrange(2, 31 - regs + 1)
        rng_set = set(range(rt, rt + regs))
        load = kind in ("LSWI", "LSWX")
        ra = rng.choice([r for r in GPR_POOL if r and (not load or r not in rng_set)])
        off = rng.randrange(0x40, SCRATCH_SIZE - 0x60)
        if kind in ("LSWI", "STSWI"):
            w = X31(rt, ra, nb & 31, 597 if kind == "LSWI" else 725)
            def fix(s, d, c):
                set_gpr(s, ra, d["scratch"] + off)
        else:
            rb = rng.choice([r for r in GPR_POOL if r and r != ra and (not load or r not in rng_set)])
            w = X31(rt, ra, rb, 533 if kind == "LSWX" else 661)
            idx = rng.randrange(0, 0x20)
            def fix(s, d, c):
                set_gpr(s, ra, d["scratch"] + off - idx); set_gpr(s, rb, idx)
                xer = struct.unpack_from(">Q", s, S_XER)[0]
                struct.pack_into(">Q", s, S_XER, (xer & ~0x7F) | nb)
        return [w], fix, {"mem": True}
    return gen


for k_ in ("LSWI", "LSWX", "STSWI", "STSWX"):
    op(k_, lsw_gen(k_))


# reservations
def resv_gen(kind):
    def gen(rng, k):
        dw = kind in ("LDARX", "STDCX")
        size = 8 if dw else 4
        rt, rs = g(rng), g(rng)
        ra = gnz(rng)
        rb = rng.choice([r for r in GPR_POOL if r != ra])
        off = rng.randrange(0x40, SCRATCH_SIZE - 0x40) & ~(size - 1)
        mode = k % 3          # 0: larx+stcx same EA, 1: lone stcx, 2: larx only
        larx = X31(rt, ra, rb, 84 if dw else 20)
        stcx = X31(rs, ra, rb, 214 if dw else 150, 1)
        if kind in ("LWARX", "LDARX"):
            words = [larx] if mode == 2 else [larx, stcx]
        else:
            words = [stcx] if mode == 1 else [larx, stcx]
        if rt in (ra, rb) and len(words) > 1:
            words = words[1:] if kind in ("STWCX", "STDCX") else words[:1]
        def fix(s, d, c):
            set_gpr(s, rb, 0); set_gpr(s, ra, d["scratch"] + off)
        return words, fix, {"mem": True, "resv": any((w >> 1) & 0x3FF in (150, 214) for w in words)}
    return gen


op("LWARX", resv_gen("LWARX"), count=30)
op("LDARX", resv_gen("LDARX"), count=30)
op("STWCX", resv_gen("STWCX"), count=30)
op("STDCX", resv_gen("STDCX"), count=30)

# ------------------------------------------------------------------ floating point

def afp(po, xo, b=True, c=False):
    def mk(rc):
        return simple(lambda r: W(po, fr(r), fr(r) if (b or c) and not (xo in (22, 24, 26) ) else 0,
                                  fr(r) if b else 0, ((fr(r) if c else 0) << 6) | (xo << 1) | rc))
    return mk


for nm, po, xo, b, c in [("FDIVS", 59, 18, 1, 0), ("FSUBS", 59, 20, 1, 0), ("FADDS", 59, 21, 1, 0),
                         ("FSQRTS", 59, 22, 1, 0), ("FRES", 59, 24, 1, 0), ("FMULS", 59, 25, 0, 1),
                         ("FMSUBS", 59, 28, 1, 1), ("FMADDS", 59, 29, 1, 1), ("FNMSUBS", 59, 30, 1, 1),
                         ("FNMADDS", 59, 31, 1, 1),
                         ("FDIV", 63, 18, 1, 0), ("FSUB", 63, 20, 1, 0), ("FADD", 63, 21, 1, 0),
                         ("FSQRT", 63, 22, 1, 0), ("FSEL", 63, 23, 1, 1), ("FMUL", 63, 25, 0, 1),
                         ("FRSQRTE", 63, 26, 1, 0), ("FMSUB", 63, 28, 1, 1), ("FMADD", 63, 29, 1, 1),
                         ("FNMSUB", 63, 30, 1, 1), ("FNMADD", 63, 31, 1, 1)]:
    for rc in (0, 1):
        op(nm + ("." if rc else ""), afp(po, xo, b, c)(rc), count=60)
# X-form unary FP (FRT, FRB)
for nm, xo in [("FRSP", 12), ("FCTIW", 14), ("FCTIWZ", 15), ("FNEG", 40), ("FMR", 72), ("FNABS", 136),
               ("FABS", 264), ("FCTID", 814), ("FCTIDZ", 815), ("FCFID", 846)]:
    for rc in (0, 1):
        op(nm + ("." if rc else ""), simple(lambda r, xo=xo, rc=rc: W(63, fr(r), 0, fr(r), (xo << 1) | rc)), count=60)
op("FCMPU", simple(lambda r: W(63, r.randrange(8) << 2, fr(r), fr(r), 0)), count=60)
op("FCMPO", simple(lambda r: W(63, r.randrange(8) << 2, fr(r), fr(r), 32 << 1)), count=60)
for rc in (0, 1):
    s_ = "." if rc else ""
    op("MFFS" + s_, simple(lambda r, rc=rc: W(63, fr(r), 0, 0, (583 << 1) | rc)))
    op("MTFSF" + s_, simple(lambda r, rc=rc: W(63, 0, 0, fr(r), (r.getrandbits(8) << 17) | (711 << 1) | rc)))
    op("MTFSFI" + s_, simple(lambda r, rc=rc: W(63, r.randrange(8) << 2, 0, 0, (r.randrange(16) << 12) | (134 << 1) | rc)))
    op("MTFSB0" + s_, simple(lambda r, rc=rc: W(63, r.randrange(32), 0, 0, (70 << 1) | rc)))
    op("MTFSB1" + s_, simple(lambda r, rc=rc: W(63, r.randrange(32), 0, 0, (38 << 1) | rc)))
op("MCRFS", simple(lambda r: W(63, r.randrange(8) << 2, r.randrange(8) << 2, 0, 64 << 1)))

# ------------------------------------------------------------------ VMX

VX = [("VADDUBM", 0x0), ("VMAXUB", 0x2), ("VRLB", 0x4), ("VCMPEQUB", 0x006), ("VCMPEQUB.", 0x406),
      ("VMULOUB", 0x8), ("VADDFP", 0xa), ("VMRGHB", 0xc), ("VPKUHUM", 0xe), ("VADDUHM", 0x40),
      ("VMAXUH", 0x42), ("VRLH", 0x44), ("VCMPEQUH", 0x046), ("VCMPEQUH.", 0x446), ("VMULOUH", 0x48),
      ("VSUBFP", 0x4a), ("VMRGHH", 0x4c), ("VPKUWUM", 0x4e), ("VADDUWM", 0x80), ("VMAXUW", 0x82),
      ("VRLW", 0x84), ("VCMPEQUW", 0x086), ("VCMPEQUW.", 0x486), ("VMRGHW", 0x8c), ("VPKUHUS", 0x8e),
      ("VCMPEQFP", 0x0c6), ("VCMPEQFP.", 0x4c6), ("VPKUWUS", 0xce), ("VMAXSB", 0x102), ("VSLB", 0x104),
      ("VMULOSB", 0x108), ("VMRGLB", 0x10c), ("VPKSHUS", 0x10e), ("VMAXSH", 0x142), ("VSLH", 0x144),
      ("VMULOSH", 0x148), ("VMRGLH", 0x14c), ("VPKSWUS", 0x14e), ("VADDCUW", 0x180), ("VMAXSW", 0x182),
      ("VSLW", 0x184), ("VMRGLW", 0x18c), ("VPKSHSS", 0x18e), ("VSL", 0x1c4), ("VCMPGEFP", 0x1c6),
      ("VCMPGEFP.", 0x5c6), ("VPKSWSS", 0x1ce), ("VADDUBS", 0x200), ("VMINUB", 0x202), ("VSRB", 0x204),
      ("VCMPGTUB", 0x206), ("VCMPGTUB.", 0x606), ("VMULEUB", 0x208), ("VADDUHS", 0x240), ("VMINUH", 0x242),
      ("VSRH", 0x244), ("VCMPGTUH", 0x246), ("VCMPGTUH.", 0x646), ("VMULEUH", 0x248), ("VADDUWS", 0x280),
      ("VMINUW", 0x282), ("VSRW", 0x284), ("VCMPGTUW", 0x286), ("VCMPGTUW.", 0x686), ("VSR", 0x2c4),
      ("VCMPGTFP", 0x2c6), ("VCMPGTFP.", 0x6c6), ("VADDSBS", 0x300), ("VMINSB", 0x302), ("VSRAB", 0x304),
      ("VCMPGTSB", 0x306), ("VCMPGTSB.", 0x706), ("VMULESB", 0x308), ("VPKPX", 0x30e), ("VADDSHS", 0x340),
      ("VMINSH", 0x342), ("VSRAH", 0x344), ("VCMPGTSH", 0x346), ("VCMPGTSH.", 0x746), ("VMULESH", 0x348),
      ("VADDSWS", 0x380), ("VMINSW", 0x382), ("VSRAW", 0x384), ("VCMPGTSW", 0x386), ("VCMPGTSW.", 0x786),
      ("VCMPBFP", 0x3c6), ("VCMPBFP.", 0x7c6), ("VSUBUBM", 0x400), ("VAVGUB", 0x402), ("VAND", 0x404),
      ("VMAXFP", 0x40a), ("VSLO", 0x40c), ("VSUBUHM", 0x440), ("VAVGUH", 0x442), ("VANDC", 0x444),
      ("VMINFP", 0x44a), ("VSRO", 0x44c), ("VSUBUWM", 0x480), ("VAVGUW", 0x482), ("VOR", 0x484),
      ("VXOR", 0x4c4), ("VAVGSB", 0x502), ("VNOR", 0x504), ("VAVGSH", 0x542), ("VSUBCUW", 0x580),
      ("VAVGSW", 0x582), ("VSUBUBS", 0x600), ("VSUM4UBS", 0x608), ("VSUBUHS", 0x640), ("VSUM4SHS", 0x648),
      ("VSUBUWS", 0x680), ("VSUM2SWS", 0x688), ("VSUBSBS", 0x700), ("VSUM4SBS", 0x708), ("VSUBSHS", 0x740),
      ("VSUBSWS", 0x780), ("VSUMSWS", 0x788)]
VX_B = [("VREFP", 0x10a), ("VRSQRTEFP", 0x14a), ("VEXPTEFP", 0x18a), ("VLOGEFP", 0x1ca), ("VRFIN", 0x20a),
        ("VUPKHSB", 0x20e), ("VRFIZ", 0x24a), ("VUPKHSH", 0x24e), ("VRFIP", 0x28a), ("VUPKLSB", 0x28e),
        ("VRFIM", 0x2ca), ("VUPKLSH", 0x2ce), ("VUPKHPX", 0x34e), ("VUPKLPX", 0x3ce)]
VX_UIMM = [("VSPLTB", 0x20c, 16), ("VSPLTH", 0x24c, 8), ("VSPLTW", 0x28c, 4), ("VCFUX", 0x30a, 32),
           ("VCFSX", 0x34a, 32), ("VCTUXS", 0x38a, 32), ("VCTSXS", 0x3ca, 32)]
VX_SIMM = [("VSPLTISB", 0x30c), ("VSPLTISH", 0x34c), ("VSPLTISW", 0x38c)]
VA = [("VMHADDSHS", 0x20), ("VMHRADDSHS", 0x21), ("VMLADDUHM", 0x22), ("VMSUMUBM", 0x24), ("VMSUMMBM", 0x25),
      ("VMSUMUHM", 0x26), ("VMSUMUHS", 0x27), ("VMSUMSHM", 0x28), ("VMSUMSHS", 0x29), ("VSEL", 0x2a),
      ("VPERM", 0x2b), ("VMADDFP", 0x2e), ("VNMSUBFP", 0x2f)]

for nm, xo in VX:
    op(nm, simple(lambda r, xo=xo: W(4, vr(r), vr(r), vr(r), xo)), count=50)
for nm, xo in VX_B:
    op(nm, simple(lambda r, xo=xo: W(4, vr(r), 0, vr(r), xo)), count=50)
for nm, xo, n in VX_UIMM:
    op(nm, simple(lambda r, xo=xo, n=n: W(4, vr(r), r.randrange(n), vr(r), xo)), count=50)
for nm, xo in VX_SIMM:
    op(nm, simple(lambda r, xo=xo: W(4, vr(r), r.randrange(32), 0, xo)), count=40)
for nm, xo in VA:
    op(nm, simple(lambda r, xo=xo: W(4, vr(r), vr(r), vr(r), (vr(r) << 6) | xo)), count=50)
op("VSLDOI", simple(lambda r: W(4, vr(r), vr(r), vr(r), (r.randrange(16) << 6) | 0x2c)), count=50)
# RPCS3 keeps VSCR in BE word 0 of the vector (the ISA: word 3). Mirror word 3
# into word 0 of mtvscr's source, and tell compare.py where mfvscr's lands.
def mfvscr_gen(rng, k):
    vd = vr(rng)
    return [W(4, vd, 0, 0, 0x604)], None, {"vscr_dest": vd}


def mtvscr_gen(rng, k):
    vb = vr(rng)
    def fix(s, d, c):
        o = S_VR + 16 * vb
        s[o:o + 4] = s[o + 12:o + 16]
    return [W(4, 0, 0, vb, 0x644)], fix


op("MFVSCR", mfvscr_gen)
op("MTVSCR", mtvscr_gen)
