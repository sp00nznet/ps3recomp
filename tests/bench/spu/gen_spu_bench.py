#!/usr/bin/env python3
"""Generate the SPU operation benchmark: one looped kernel per SPU operation.

Every kernel is a counted loop

    k:  <op> x OPS     ; the operation under test
        ai   $2,$2,-1
        brnz $2,k
        ori  $0,$81,0  ; (call kernels only: brsl/bisl clobber $0)
        bi   $0

placed at its own LS address, so a single SPU image holds every kernel. The
harness (spu_bench_main.c) sets the registers each kernel reads, puts the
iteration count in $2, and times the kernel lifted (spu_func_<addr>) and
interpreted (spu_interp_run). ops/sec = iterations * OPS / seconds.

Modes:
  lat    one dependency chain (rt = ra = $3): each op waits for the previous
         one, so this is the op's latency.
  tput   eight independent chains ($10..$17, four ops each): the compiler and
         the host core may overlap them, so this is the op's throughput.
  issue  the op has no register result to chain through (hints, sync, halts
         not taken, constants, stores, branches): OPS independent copies.

Operands are chosen so the chain stays in the op's ordinary range (ints:
small shift counts; floats: values near 1.0). Float ops also get a
"special" row whose chain holds an extended-range lane (SPU single
exponent 255) or a double NaN: the helpers have a fast path for ordinary
values and fall back to an exact path otherwise, and the game hits both.

Writes, into --out:
  bench.elf       the SPU image (lifted by run_spu_bench.sh)
  bench_cases.h   the case table and the LS image for spu_bench_main.c
"""
import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.join(HERE, "..", "..", "..")
sys.path.insert(0, os.path.join(REPO, "tools"))
sys.path.insert(0, os.path.join(REPO, "tests", "conformance", "spu"))
from wrap_spu_elf import wrap  # noqa: E402
from spu_ops import OPCODES    # noqa: E402

OP = {n: (w, v) for n, w, v in OPCODES}

OPS = 32                    # operations per loop iteration
KERNEL_STRIDE = 0x100       # LS bytes per kernel (32 ops + 4 control words fit)
LEAF_LSA = 0x30000          # bi $0   (target of the call kernels)
STOP_LSA = 0x30010          # stop    (where the interpreter returns to)
CHASE_LSA = 0x38000         # pointer-chase slots for the load kernels
STORE_LSA = 0x3A000         # store buffer for the store kernels
LS_SIZE = 0x40000

# The harness's register-file conventions.
R_CNT = 2                   # loop counter
R_CHAIN = 3                 # lat chain
R_B, R_C = 4, 5             # second / third operand (fixed)
R_ZERO = 20                 # 0 in every word (lqx/stqx index)
R_LEAF = 9                  # LEAF_LSA (bisl target)
R_TAKE, R_SKIP = 7, 8       # 0 (brz taken) / 1 (brz not taken)
R_STORE = 6                 # STORE_LSA (store base)
R_LINK_SAVE = 81            # $0 saved around call kernels
TPUT = list(range(10, 18))  # tput chains

# ---- operand classes ------------------------------------------------------
F32 = lambda f: struct.unpack(">I", struct.pack(">f", f))[0]
F64 = lambda f: struct.unpack(">Q", struct.pack(">d", f))[0]


def w4(*w):
    return list(w) + [w[-1]] * (4 - len(w))


def d2(a, b=None):
    b = a if b is None else b
    return [a >> 32, a & 0xFFFFFFFF, b >> 32, b & 0xFFFFFFFF]


SP_OPS = {"fa", "fs", "fm", "fma", "fms", "fnms", "frest", "frsqest", "fi", "fcgt", "fcmgt",
          "fceq", "fcmeq", "cflts", "cfltu", "fesd"}
DP_OPS = {"dfa", "dfs", "dfm", "dfma", "dfms", "dfnms", "dfnma", "frds"}
INT_TO_F = {"csflt", "cuflt"}

INT_CHAIN = w4(0x12345678, 0x0BADF00D, 0x00C0FFEE, 0x7FFF0001)
INT_B = w4(3, 5, 7, 9)                                   # small shift counts
INT_C = w4(0x00010203, 0x04050607, 0x18191A1B, 0x1C1D1E1F)   # shufb: mix ra / rb bytes
SP_CHAIN = w4(F32(1.5), F32(2.0), F32(0.75), F32(3.0))
SP_ONE = w4(F32(1.0))
SP_HALF = w4(F32(0.5))
SP_SPECIAL = w4(0x7F800000, F32(2.0), F32(0.75), F32(3.0))  # lane 0: SPU 2^128 (extended range)
SP_ONE_ZERO = [F32(1.0), F32(1.0), F32(1.0), 0]               # lane 3: 0.0 (w = 0, padding)
DP_CHAIN = d2(F64(1.5), F64(0.75))
DP_ONE = d2(F64(1.0))
DP_HALF = d2(F64(0.5))
DP_SPECIAL = d2(0x7FF8000000000000, F64(0.75))           # lane 0: quiet NaN

# Ops whose result is not in rt (or that have no result): "issue" mode only.
NO_RESULT = {"lnop", "nop", "sync", "dsync", "hbr", "hbra", "hbrr", "mtspr", "fscrwr"}
CONST = {"il", "ilh", "ilhu", "ila", "fsmbi", "fscrrd", "mfspr"}   # write rt, read nothing
HALTS = {"heq", "hgt", "hlgt", "heqi", "hgti", "hlgti"}
# Not register-to-register: own kernels below, or not benchmarkable in a loop.
CUSTOM = {"rdch", "rchcnt", "wrch", "lqd", "lqx", "lqa", "lqr", "stqd", "stqx", "stqa", "stqr",
          "br", "bra", "brsl", "brasl", "brz", "brnz", "brhz", "brhnz",
          "bi", "bisl", "biz", "binz", "bihz", "bihnz"}
SKIP = {
    "stop": "ends the SPU program", "stopd": "ends the SPU program",
    "iret": "returns from an SPU interrupt", "bisled": "branches on a pending event",
    "unk": "not an instruction",
    # SPU ISA 1.2 / PowerXCell 8i only; the PS3's SPU does not implement them.
    "dfceq": "PowerXCell only", "dfcmeq": "PowerXCell only", "dfcgt": "PowerXCell only",
    "dfcmgt": "PowerXCell only", "dftsv": "PowerXCell only",
}

# Immediates: shift-by-4 (RI7 sits in the rb field), unit scale for the
# float<->int conversions, small RI10 / RI16 / RI18 values.
I8 = {"cflts": 173, "cfltu": 173, "csflt": 155, "cuflt": 155}
I10, I16, I18 = 5, 0x1234, 0x1234


def enc(name, rt, ra=0, rb=0, rc=0, imm=None):
    w, v = OP[name]
    if w == 11:
        return (v << 21) | ((rb & 127) << 14) | ((ra & 127) << 7) | (rt & 127)
    if w == 10:
        i = I8[name] if imm is None else imm
        return (v << 22) | ((i & 0xFF) << 14) | ((ra & 127) << 7) | (rt & 127)
    if w == 8:
        i = I10 if imm is None else imm
        return (v << 24) | ((i & 0x3FF) << 14) | ((ra & 127) << 7) | (rt & 127)
    if w == 9:
        i = I16 if imm is None else imm
        return (v << 23) | ((i & 0xFFFF) << 7) | (rt & 127)
    if w == 7:
        i = I18 if imm is None else imm
        return (v << 25) | ((i & 0x3FFFF) << 7) | (rt & 127)
    if w == 4:   # RRR: rt in bits 21-27, rc in the low 7
        return (v << 28) | ((rt & 127) << 21) | ((rb & 127) << 14) | ((ra & 127) << 7) | (rc & 127)
    raise ValueError(name)


def ri16_branch(name, lsa_here, target, rt):
    """Relative (br, brz, brsl...) or absolute (bra, brasl) RI16 branch."""
    absolute = name in ("bra", "brasl")
    field = (target >> 2) if absolute else ((target - lsa_here) >> 2)
    return enc(name, rt, imm=field & 0xFFFF)


class Case:
    def __init__(self, name, mode, ops, regs, note="", barrier="reg", interp=True):
        self.name, self.mode, self.ops, self.regs, self.note = name, mode, ops, regs, note
        self.barrier = barrier        # reg: value barrier per op; mem: also a memory clobber
        self.interp = interp
        self.body = []                # instruction words (not counting the loop control)
        self.saves_link = False


def operands(name, special=False):
    """(chain init, rb init, rc init) for a register op. special: "special"
    (an extended-range / NaN lane in the chain) or "zero" (a 0.0 lane in the
    second operand, which keeps the lane on the exact path every op)."""
    if name in SP_OPS:
        if special == "zero":
            return SP_CHAIN, SP_ONE_ZERO, SP_HALF
        return (SP_SPECIAL if special else SP_CHAIN), SP_ONE, SP_HALF
    if name in DP_OPS:
        return (DP_SPECIAL if special else DP_CHAIN), DP_ONE, DP_HALF
    return INT_CHAIN, INT_B, INT_C


def register_cases():
    cases = []
    for name, _w, _v in OPCODES:
        if name in SKIP or name in CUSTOM:
            continue
        if name in HALTS:
            c = Case(name, "issue", OPS, {R_CHAIN: w4(0), R_B: w4(3)}, "not taken")
            c.body = [enc(name, 0, R_CHAIN, R_B) for _ in range(OPS)]
            cases.append(c)
            continue
        if name in NO_RESULT:
            c = Case(name, "issue", OPS, {R_CHAIN: INT_CHAIN, R_B: INT_B})
            if name == "fscrwr":     # write the FPSCR from $20 (zeros: keep it clean)
                c.body = [enc(name, 0, R_ZERO) for _ in range(OPS)]
                c.regs[R_ZERO] = w4(0)
            elif name in ("hbra", "hbrr", "hbr"):
                c.body = [enc(name, 0, 0, 0) if OP[name][0] == 11 else enc(name, 0, imm=0)
                          for _ in range(OPS)]
            elif name == "mtspr":
                c.body = [enc(name, 0, R_CHAIN) for _ in range(OPS)]
            else:
                c.body = [enc(name, R_CHAIN, R_CHAIN, R_B) for _ in range(OPS)]
            cases.append(c)
            continue
        if name in CONST:
            c = Case(name, "issue", OPS, {}, "", "vol")
            c.body = [enc(name, 10 + i) for i in range(OPS)]
            cases.append(c)
            continue
        variants = ([False, "special", "zero"] if name in SP_OPS else
                    [False, "special"] if name in DP_OPS else [False])
        for special in variants:
            chain, b, cc = operands(name, special)
            if name in INT_TO_F:
                chain = w4(1000, 2000, 3000, 4000)
            sfx = " " + special if special else ""
            # lat: rt = ra = $3
            c = Case(name, "lat" + sfx, OPS, {R_CHAIN: chain, R_B: b, R_C: cc})
            c.body = [enc(name, R_CHAIN, R_CHAIN, R_B, R_C) for _ in range(OPS)]
            cases.append(c)
            # tput: eight chains
            regs = {r: chain for r in TPUT}
            regs.update({R_B: b, R_C: cc})
            c = Case(name, "tput" + sfx, OPS, regs)
            c.body = [enc(name, r, r, R_B, R_C) for _ in range(OPS // len(TPUT)) for r in TPUT]
            cases.append(c)
    return cases


def memory_cases():
    cases = []
    chase = lambda k: w4(CHASE_LSA + 16 * k, 0, 0, 0)
    # lqd / lqx: pointer chase ($r <- LS[$r]); every slot points at itself.
    c = Case("lqd", "lat", OPS, {R_CHAIN: chase(0)}, "pointer chase", "mem")
    c.body = [enc("lqd", R_CHAIN, R_CHAIN, imm=0) for _ in range(OPS)]
    cases.append(c)
    c = Case("lqd", "tput", OPS, {r: chase(1 + j) for j, r in enumerate(TPUT)}, "8 chases", "mem")
    c.body = [enc("lqd", r, r, imm=0) for _ in range(OPS // 8) for r in TPUT]
    cases.append(c)
    c = Case("lqx", "lat", OPS, {R_CHAIN: chase(0), R_ZERO: w4(0)}, "pointer chase", "mem")
    c.body = [enc("lqx", R_CHAIN, R_CHAIN, R_ZERO) for _ in range(OPS)]
    cases.append(c)
    # lqa / lqr: fixed addresses, OPS distinct destinations.
    c = Case("lqa", "issue", OPS, {}, "", "mem")
    c.body = [enc("lqa", 10 + i, imm=(CHASE_LSA + 16 * i) >> 2) for i in range(OPS)]
    cases.append(c)
    c = Case("lqr", "issue", OPS, {}, "", "mem")
    c.lqr = True                  # pc-relative: encoded once the kernel's LSA is known
    cases.append(c)
    # stores: OPS distinct quadwords from the eight tput registers.
    regs = {r: w4(r) for r in TPUT}
    regs[R_STORE] = w4(STORE_LSA)
    regs[R_ZERO] = w4(0)
    c = Case("stqd", "issue", OPS, dict(regs), "", "mem")
    c.body = [enc("stqd", TPUT[i % 8], R_STORE, imm=i) for i in range(OPS)]
    cases.append(c)
    regs_x = dict(regs)
    for i in range(OPS):
        regs_x[30 + i] = w4(16 * i)
    c = Case("stqx", "issue", OPS, regs_x, "", "mem")
    c.body = [enc("stqx", TPUT[i % 8], R_STORE, 30 + i) for i in range(OPS)]
    cases.append(c)
    c = Case("stqa", "issue", OPS, dict(regs), "", "mem")
    c.body = [enc("stqa", TPUT[i % 8], imm=(STORE_LSA + 16 * i) >> 2) for i in range(OPS)]
    cases.append(c)
    c = Case("stqr", "issue", OPS, dict(regs), "", "mem")
    c.stqr = True
    cases.append(c)
    return cases


def control_cases():
    """Branch kernels; bodies need the kernel's LSA, so they are built in layout()."""
    cases = []
    base = {R_TAKE: w4(0), R_SKIP: w4(1), R_LEAF: w4(LEAF_LSA)}
    for name, mode, note in (("br", "issue", "to the next word"),
                             ("bra", "issue", "to the next word"),
                             ("brz", "issue", "taken, to the next word"),
                             ("brz", "issue", "not taken"),
                             ("brnz", "issue", "taken, to the next word"),
                             ("brnz", "issue", "not taken"),
                             ("brhz", "issue", "taken, to the next word"),
                             ("brhz", "issue", "not taken"),
                             ("brhnz", "issue", "taken, to the next word"),
                             ("brhnz", "issue", "not taken"),
                             ("biz", "issue", "not taken"),
                             ("binz", "issue", "not taken"),
                             ("bihz", "issue", "not taken"),
                             ("bihnz", "issue", "not taken"),
                             ("brsl", "issue", "call + bi $0 return"),
                             ("brasl", "issue", "call + bi $0 return"),
                             ("bisl", "issue", "indirect call + bi $0 return")):
        c = Case(name, mode, OPS, dict(base), note)
        c.branch = True
        cases.append(c)
    return cases


def branch_body(c, lsa):
    out = []
    taken = "not taken" not in c.note
    for i in range(OPS):
        here = lsa + 4 * i
        n = c.name
        if n in ("br", "bra"):
            out.append(ri16_branch(n, here, here + 4, 0))
        elif n in ("brz", "brhz"):
            out.append(ri16_branch(n, here, here + 4, R_TAKE if taken else R_SKIP))
        elif n in ("brnz", "brhnz"):
            out.append(ri16_branch(n, here, here + 4, R_SKIP if taken else R_TAKE))
        elif n in ("biz", "bihz"):         # not taken: the tested register is nonzero
            out.append(enc(n, R_SKIP, R_LEAF))
        elif n in ("binz", "bihnz"):       # not taken: the tested register is zero
            out.append(enc(n, R_TAKE, R_LEAF))
        elif n in ("brsl", "brasl"):
            out.append(ri16_branch(n, here, LEAF_LSA, 0))
        elif n == "bisl":
            out.append(enc("bisl", 0, R_LEAF))
    return out


# ---- channels -------------------------------------------------------------
CH = dict(SPU_WrEventMask=1, SPU_WrEventAck=2, SPU_WrDec=7, SPU_RdDec=8, SPU_RdEventMask=11,
          MFC_RdTagMask=12, SPU_RdMachStat=13, SPU_WrSRR0=14, SPU_RdSRR0=15, MFC_LSA=16,
          MFC_EAH=17, MFC_EAL=18, MFC_Size=19, MFC_TagID=20, MFC_Cmd=21, MFC_WrTagMask=22,
          MFC_WrTagUpdate=23, MFC_RdTagStat=24, MFC_RdAtomicStat=27, SPU_WrOutMbox=28,
          SPU_RdInMbox=29, SPU_RdEventStat=0)
DMA_LSA = 0x3C000           # DMA buffer in LS (16 KB)
GUEST_EA = 0x00100000       # guest EA the DMA kernels use (harness maps it)
TAG = 2
# value registers for wrch
R_V = dict(lsa=21, eah=22, eal=23, size=24, tag=25, get=26, put=27, mask=28, upd=29,
           getllar=30, putllc=31, putlluc=32, zero=R_ZERO, big=33)


def rdch(ch, rt):
    return enc("rdch", rt, CH[ch])


def rchcnt(ch, rt):
    return enc("rchcnt", rt, CH[ch])


def wrch(ch, rv):
    return enc("wrch", rv, CH[ch])


def channel_cases():
    """Channel instructions on the real spu_channels.c. Single-instruction
    rows repeat the instruction OPS times; the MFC rows are whole sequences
    (counted as one op each: a DMA = 6 parameter/command writes + the tag
    wait, an atomic = its command + the atomic-status read)."""
    def regs(size=16):
        r = {R_V["lsa"]: w4(DMA_LSA), R_V["eah"]: w4(0), R_V["eal"]: w4(GUEST_EA),
             R_V["size"]: w4(size), R_V["tag"]: w4(TAG), R_V["get"]: w4(0x40),
             R_V["put"]: w4(0x20), R_V["mask"]: w4(1 << TAG), R_V["upd"]: w4(2),
             R_V["getllar"]: w4(0xD0), R_V["putllc"]: w4(0xB4), R_V["putlluc"]: w4(0xB0),
             R_ZERO: w4(0), R_V["big"]: w4(0x7FFFFFFF)}
        return r
    cases = []

    def single(name, note, word_fn, barrier="vol"):
        c = Case(name, "issue", OPS, regs(), note, barrier)
        c.body = [word_fn(i) for i in range(OPS)]
        cases.append(c)

    single("rchcnt", "MFC_RdTagStat", lambda i: rchcnt("MFC_RdTagStat", 40 + i))
    single("rchcnt", "SPU_RdInMbox (empty)", lambda i: rchcnt("SPU_RdInMbox", 40 + i))
    single("rchcnt", "SPU_RdEventStat", lambda i: rchcnt("SPU_RdEventStat", 40 + i))
    single("rdch", "SPU_RdDec", lambda i: rdch("SPU_RdDec", 40 + i))
    single("rdch", "SPU_RdEventMask", lambda i: rdch("SPU_RdEventMask", 40 + i))
    single("rdch", "MFC_RdTagMask", lambda i: rdch("MFC_RdTagMask", 40 + i))
    single("rdch", "SPU_RdMachStat", lambda i: rdch("SPU_RdMachStat", 40 + i))
    single("rdch", "SPU_RdSRR0", lambda i: rdch("SPU_RdSRR0", 40 + i))
    single("wrch", "SPU_WrDec", lambda i: wrch("SPU_WrDec", R_V["big"]))
    single("wrch", "SPU_WrEventMask (0)", lambda i: wrch("SPU_WrEventMask", R_ZERO))
    single("wrch", "SPU_WrEventAck (0)", lambda i: wrch("SPU_WrEventAck", R_ZERO))
    single("wrch", "SPU_WrSRR0", lambda i: wrch("SPU_WrSRR0", R_ZERO))
    single("wrch", "MFC_LSA", lambda i: wrch("MFC_LSA", R_V["lsa"]))
    single("wrch", "MFC_EAL", lambda i: wrch("MFC_EAL", R_V["eal"]))
    single("wrch", "MFC_Size", lambda i: wrch("MFC_Size", R_V["size"]))
    single("wrch", "MFC_TagID", lambda i: wrch("MFC_TagID", R_V["tag"]))
    single("wrch", "MFC_WrTagMask", lambda i: wrch("MFC_WrTagMask", R_V["mask"]))

    def mfc(cmd_reg):
        return [wrch("MFC_LSA", R_V["lsa"]), wrch("MFC_EAH", R_V["eah"]),
                wrch("MFC_EAL", R_V["eal"]), wrch("MFC_Size", R_V["size"]),
                wrch("MFC_TagID", R_V["tag"]), wrch("MFC_Cmd", cmd_reg)]

    tag_wait = [wrch("MFC_WrTagMask", R_V["mask"]), wrch("MFC_WrTagUpdate", R_V["upd"]),
                rdch("MFC_RdTagStat", 40)]
    for size in (16, 128, 1024, 16384):
        for cmd, label in (("get", "GET"), ("put", "PUT")):
            seq = mfc(R_V[cmd]) + tag_wait
            n = 64 // len(seq) - 1          # copies that fit the kernel with loop control
            c = Case("dma", "issue", n, regs(size), "%s %d B + tag wait" % (label, size), "mem")
            c.body = seq * n
            c.body_words = len(c.body)
            cases.append(c)
    # one DMA, waited on only after a batch of 4 (tags overlap the transfers)
    seq = mfc(R_V["get"]) * 4 + tag_wait
    c = Case("dma", "issue", 4, regs(128), "GET 128 B x4, one tag wait", "mem")
    c.body = seq
    c.body_words = len(seq)
    cases.append(c)

    atom = lambda cmd: [wrch("MFC_LSA", R_V["lsa"]), wrch("MFC_EAH", R_V["eah"]),
                        wrch("MFC_EAL", R_V["eal"]), wrch("MFC_Cmd", R_V[cmd]),
                        rdch("MFC_RdAtomicStat", 41)]
    for cmds, note in ((["getllar"], "GETLLAR + atomic stat"),
                       (["getllar", "putllc"], "GETLLAR, PUTLLC (succeeds)"),
                       (["putlluc"], "PUTLLUC + atomic stat")):
        seq = sum((atom(x) for x in cmds), [])
        n = 60 // len(seq)
        c = Case("atomic", "issue", n, regs(128), note, "mem")
        c.body = seq * n
        c.body_words = len(c.body)
        cases.append(c)
    return cases


def layout(cases):
    """Assign each kernel its LSA, finish the LSA-dependent bodies, and emit
    the LS image. Call kernels (brsl/brasl/bisl overwrite $0) save $0 in $81
    ahead of the loop and restore it before the final bi $0."""
    ls = bytearray(LS_SIZE)
    for k, c in enumerate(cases):
        lsa = KERNEL_STRIDE * k
        assert lsa + KERNEL_STRIDE <= LEAF_LSA, "too many kernels for the LS layout"
        c.lsa = lsa
        c.saves_link = c.name in ("brsl", "brasl", "bisl")
        words = [enc("or", R_LINK_SAVE, 0, 0)] if c.saves_link else []
        loop = lsa + 4 * len(words)          # first body word
        if getattr(c, "branch", False):
            c.body = branch_body(c, loop)
        if getattr(c, "lqr", False):
            c.body = [enc("lqr", 10 + i, imm=((CHASE_LSA + 16 * i) - (loop + 4 * i)) >> 2)
                      for i in range(OPS)]
        if getattr(c, "stqr", False):
            c.body = [enc("stqr", TPUT[i % 8], imm=((STORE_LSA + 16 * i) - (loop + 4 * i)) >> 2)
                      for i in range(OPS)]
        assert len(c.body) == getattr(c, "body_words", c.ops), (c.name, c.mode, len(c.body))
        words += c.body
        words.append(enc("ai", R_CNT, R_CNT, imm=-1))
        words.append(ri16_branch("brnz", lsa + 4 * len(words), loop, R_CNT))
        if c.saves_link:
            words.append(enc("or", 0, R_LINK_SAVE, R_LINK_SAVE))
        words.append(enc("bi", 0, 0))
        assert 4 * len(words) <= KERNEL_STRIDE
        struct.pack_into(">%dI" % len(words), ls, lsa, *words)
    struct.pack_into(">I", ls, LEAF_LSA, enc("bi", 0, 0))
    struct.pack_into(">I", ls, STOP_LSA, (OP["stop"][1] << 21) | 0x3FFF)
    return ls


def write_header(path, cases, ls):
    code_end = STOP_LSA + 16
    with open(path, "w") as f:
        f.write("/* Generated by gen_spu_bench.py -- do not edit. */\n")
        f.write("#define BENCH_LEAF_LSA 0x%05Xu\n#define BENCH_STOP_LSA 0x%05Xu\n"
                % (LEAF_LSA, STOP_LSA))
        f.write("#define BENCH_CHASE_LSA 0x%05Xu\n#define BENCH_STORE_LSA 0x%05Xu\n"
                % (CHASE_LSA, STORE_LSA))
        f.write("#define BENCH_LINK_SAVE %d\n#define BENCH_NCASES %d\n"
                % (R_LINK_SAVE, len(cases)))
        f.write("#define BENCH_GUEST_EA 0x%08Xu\n" % GUEST_EA)
        for c in cases:
            f.write("void spu_func_%08X(spu_context*);\n" % c.lsa)
        f.write("static const bench_case kCases[] = {\n")
        for c in cases:
            regs = ", ".join("{%d, {0x%08Xu, 0x%08Xu, 0x%08Xu, 0x%08Xu}}" % ((r,) + tuple(v))
                             for r, v in sorted(c.regs.items()))
            f.write('  {"%s", "%s", "%s", 0x%05Xu, %d, spu_func_%08X, %d, {%s}},\n'
                    % (c.name, c.mode, c.note, c.lsa, c.ops, c.lsa, len(c.regs), regs))
        f.write("};\n")
        f.write("static const unsigned char kLsImage[0x%X] = {\n" % code_end)
        for i in range(0, code_end, 16):
            f.write("  " + ",".join("0x%02X" % b for b in ls[i:i + 16]) + ",\n")
        f.write("};\n")
    with open(os.path.splitext(path)[0] + ".barriers", "w") as f:
        for c in cases:
            f.write("spu_func_%08X %s\n" % (c.lsa, c.barrier))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    cases = register_cases() + memory_cases() + control_cases() + channel_cases()
    ls = layout(cases)
    code_end = STOP_LSA + 16
    syms = [{"name": "k_%05x" % c.lsa, "addr": c.lsa, "size": KERNEL_STRIDE} for c in cases]
    syms.append({"name": "leaf", "addr": LEAF_LSA, "size": 4})
    open(os.path.join(args.out, "bench.elf"), "wb").write(
        wrap(bytes(ls[:code_end]), base=0, entry=0, symbols=syms))
    write_header(os.path.join(args.out, "bench_cases.h"), cases, ls)
    print("%d kernels, %d SPU ops benchmarked, %d skipped (%s)"
          % (len(cases), len({c.name for c in cases}), len(SKIP), ", ".join(sorted(SKIP))))


if __name__ == "__main__":
    main()
