#!/usr/bin/env python3
"""Diff two PPU conformance transcripts (RPCS3 oracle vs ps3recomp) field by field.

usage: compare.py <manifest.json> <oracle.txt> <ours.txt> [--show N] [--json out]

A transcript is the TTY output of ppu_conform.elf: "PPUCONF BEGIN n", one hex
line per case (the saved state slot), plus one more hex line (the scratch
buffer) after each memory case, then "PPUCONF END".
"""
import argparse
import json
import math
import struct
import sys
from collections import OrderedDict, defaultdict

S_GPR, S_FPR, S_VR = 0x000, 0x100, 0x200
FIELDS = ([("r%d" % i, S_GPR + 8 * i, 8) for i in range(32)] +
          [("f%d" % i, S_FPR + 8 * i, 8) for i in range(32)] +
          [("v%d" % i, S_VR + 16 * i, 16) for i in range(32)] +
          [("cr", 0x400, 4), ("xer", 0x408, 8), ("lr", 0x410, 8), ("ctr", 0x418, 8),
           ("fpscr", 0x420, 8), ("vscr", 0x430, 16), ("vrsave", 0x440, 4), ("flag", 0x444, 4)])


def read_transcript(path):
    lines = open(path, "rb").read().decode("latin1").split("\n")
    out, on = [], False
    for l in lines:
        l = l.strip()
        if l.startswith("PPUCONF BEGIN"):
            on, out = True, []
            continue
        if l.startswith("PPUCONF END"):
            return out, True
        if on and l and all(c in "0123456789abcdef" for c in l):
            out.append(l)
    return out, False


def split_cases(man, lines):
    res, i = [], 0
    for c in man["cases"]:
        st = bytes.fromhex(lines[i]) if i < len(lines) else None
        i += 1
        sc = None
        if c["mem"]:
            sc = bytes.fromhex(lines[i]) if i < len(lines) else None
            i += 1
        res.append((st, sc))
    return res


FMA_FAMILY = {"FMSUB", "FMSUBS", "FNMSUB", "FNMSUBS", "FNMADD", "FNMADDS", "FMADD", "FMADDS"}
ESTIMATES = {"FRES", "FRSQRTE"}
allowed_tally = defaultdict(int)


def _dbl(h):
    return struct.unpack(">d", bytes.fromhex(h))[0]


def isa_nan(c, field):
    """The PowerISA NaN result of an A-form FP op whose destination is `field`,
    or None if no operand is a NaN / not an A-form op."""
    w = c["words"][0] if c["words"] else 0
    po, xo = w >> 26, (w >> 1) & 31
    if po not in (59, 63) or xo < 18 or "f%d" % ((w >> 21) & 31) != field:
        return None
    inp = bytes.fromhex(c["input"])
    reg = lambda r: struct.unpack(">Q", inp[0x100 + 8 * r:0x108 + 8 * r])[0]
    uses_b = xo != 25                      # fmul/fmuls: A, C
    uses_c = xo in (23, 25, 28, 29, 30, 31)
    uses_a = xo not in (22, 24, 26)        # fsqrt/fres/frsqrte: B only
    order = ([reg((w >> 16) & 31)] if uses_a else []) + ([reg((w >> 11) & 31)] if uses_b else []) + \
            ([reg((w >> 6) & 31)] if uses_c else [])
    for v in order:
        if (v >> 52) & 0x7FF == 0x7FF and v & ((1 << 52) - 1):
            q = v | (1 << 51)
            if po == 59:                   # single: payload truncated to the single fraction
                q &= ~((1 << 29) - 1)
            return q
    return None


def isa_fnm_zero(c):
    """fnmsub/fnmadd (double) result under round-to-nearest, as raw bits:
    -(A*C - B) and -(A*C + B), negated AFTER rounding (Book I 4.6.5.2)."""
    w = c["words"][0] if c["words"] else 0
    if w >> 26 != 63 or (w >> 1) & 31 not in (30, 31):
        return None
    inp = bytes.fromhex(c["input"])
    reg = lambda r: struct.unpack(">d", inp[0x100 + 8 * r:0x108 + 8 * r])[0]
    a, b, cc = reg((w >> 16) & 31), reg((w >> 11) & 31), reg((w >> 6) & 31)
    r = -math.fma(a, cc, -b if (w >> 1) & 31 == 30 else b)
    return struct.unpack(">Q", struct.pack(">d", r))[0]


def _fpscr_sum(f):
    f &= ~0x60000000
    if f & 0x01F80700:
        f |= 0x20000000
    if (f >> 22) & f & 0xF8:
        f |= 0x40000000
    return f


def fpscr_reference(c):
    """Independent PowerISA reference (Book I 4.2.2, 4.6.10) for the FPSCR
    instructions, which RPCS3 does not model. Returns {field: expected bytes}
    for the fields they define, or None for any other op."""
    w = c["words"][0] if c["words"] else 0
    if w >> 26 != 63:
        return None
    xo = (w >> 1) & 0x3FF
    if xo not in (583, 711, 134, 70, 38, 64):
        return None
    inp = bytes.fromhex(c["input"])
    cr = struct.unpack(">I", inp[0x400:0x404])[0]
    f = _fpscr_sum(struct.unpack(">Q", inp[0x420:0x428])[0] & 0xFFFFFFFF)  # as the harness's mtfsf loads it
    out = {}
    if xo == 583:                                             # mffs
        out["f%d" % ((w >> 21) & 31)] = struct.pack(">Q", f)
    elif xo == 711:                                           # mtfsf
        flm, v = (w >> 17) & 0xFF, struct.unpack(">Q", inp[0x100 + 8 * ((w >> 11) & 31):0x108 + 8 * ((w >> 11) & 31)])[0]
        m = sum(0xF0000000 >> (4 * i) for i in range(8) if flm & (0x80 >> i))
        f = _fpscr_sum((f & ~m) | (v & m))
    elif xo == 134:                                           # mtfsfi
        bf, u = (w >> 23) & 7, (w >> 12) & 0xF
        m = 0xF0000000 >> (4 * bf)
        f = _fpscr_sum((f & ~m) | ((u << (28 - 4 * bf)) & m))
    elif xo in (70, 38):                                      # mtfsb0 / mtfsb1
        bt = (w >> 21) & 31
        m = 0x80000000 >> bt
        if bt not in (1, 2):
            if xo == 38:
                if not f & m and m & 0x1FF80700:
                    f |= 0x80000000                          # exception bit 0 -> 1 sets FX
                f |= m
            else:
                f &= ~m
            f = _fpscr_sum(f)
    else:                                                     # mcrfs
        bf, bfa = (w >> 23) & 7, (w >> 18) & 7
        field = (f >> (28 - 4 * bfa)) & 0xF
        f = _fpscr_sum(f & ~(0x9FF80700 & (0xF << (28 - 4 * bfa))))
        cr = (cr & ~(0xF << (28 - 4 * bf))) | (field << (28 - 4 * bf))
    if w & 1:                                                 # record: CR1 = FX FEX VX OX
        cr = (cr & ~0x0F000000) | ((f >> 4) & 0x0F000000)
    out["fpscr"] = struct.pack(">Q", f)
    out["cr"] = struct.pack(">I", cr)
    return out


def allowed(c, diffs):
    """Known, documented oracle deviations (README: 'Known oracle limitations').
    Returns (remaining diffs, reasons)."""
    op = c["op"].rstrip(".")
    keep, why = [], []
    for d in diffs:
        name, o, u = d[0], d[1], d[2]
        if name.startswith("f") and len(o) == 16:
            vo, vu = int(o, 16), int(u, 16)
            # RPCS3 negates FMA operands with host arithmetic, flipping a
            # propagated NaN's sign; PowerISA: QNaNs propagate with no effect
            # on their sign bit.
            if op in FMA_FAMILY and _dbl(o) != _dbl(o) and (vo ^ vu) == 1 << 63:
                why.append("fma NaN sign (RPCS3 deviates from the ISA)"); continue
            # PowerISA NaN propagation: the first NaN operand in the order FRA,
            # FRB, FRC wins (quieted), signalling or not. RPCS3 follows the host
            # (ARM: an SNaN beats a QNaN). Accept ours iff it is the ISA's NaN.
            exp = isa_nan(c, name)
            if exp is not None and vu == exp and vo != vu:
                why.append("NaN operand priority (RPCS3 follows the host)"); continue
            # fnmsub/fnmadd: an exact-zero A*C -/+ B is negated after rounding,
            # so +0 becomes -0. RPCS3's compiled interpreter returns B - A*C
            # (+0) despite its source. Accept ours iff it is the ISA's zero.
            if op in ("FNMSUB", "FNMADD") and (vo | vu) & ~(1 << 63) == 0 and vu == isa_fnm_zero(c):
                why.append("fnmsub/fnmadd zero sign (RPCS3 folds the negation)"); continue
            # fres/frsqrte are estimates. PowerISA bounds them: fres within 1/256,
            # frsqrte within 1/32 of the exact value. RPCS3 reproduces the PPE's
            # tables; ps3recomp computes exactly. Accept anything within the bound.
            if op in ESTIMATES:
                fo, fu = _dbl(o), _dbl(u)
                tol = (2.0 ** -8 if op == "FRES" else 2.0 ** -5) + 2.0 ** -23  # bound + one single ulp
                if fo == fu or (fo == fo and fu == fu and abs(fo - fu) <= abs(fu) * tol):
                    why.append("fres/frsqrte within the ISA estimate bound"); continue
                # RPCS3's PPE table model flushes at the denormal edges: fres
                # results below single range become 0, frsqrte of a denormal
                # input is treated as +-0 (-> +-inf). The ISA does neither.
                # Accept ours iff it is the ISA's value for that input.
                if fo in (0.0, float("inf"), float("-inf")):
                    w = c["words"][0]
                    inp = bytes.fromhex(c["input"])
                    rb = (w >> 11) & 31
                    vb = struct.unpack(">Q", inp[0x100 + 8 * rb:0x108 + 8 * rb])[0]
                    fb = struct.unpack(">d", inp[0x100 + 8 * rb:0x108 + 8 * rb])[0]
                    if op == "FRSQRTE" and (vb >> 52) & 0x7FF == 0 and vb & ((1 << 52) - 1):
                        exp = float("nan") if fb < 0 else 1 / math.sqrt(fb)
                        if (fu != fu and exp != exp) or (fu == fu and abs(fu - exp) <= abs(exp) * tol):
                            why.append("fres/frsqrte denormal edge (RPCS3 PPE model flushes)"); continue
                    if op == "FRES" and fu != 0 and abs(fu) < 2.0 ** -126:
                        exp = struct.unpack("f", struct.pack("f", 1 / fb))[0]
                        if abs(fu - exp) <= abs(exp) * tol + 2.0 ** -149:
                            why.append("fres/frsqrte denormal edge (RPCS3 PPE model flushes)"); continue
        if name == "cr" and c["op"].startswith("F") and c["op"].endswith("."):
            # FP record forms set CR1 = FPSCR[FX FEX VX OX]; RPCS3 writes FPCC
            # there and does not model FPSCR. Pending an FPSCR reference: mask CR1.
            if (int(o, 16) ^ int(u, 16)) & ~0x0F000000 == 0:
                why.append("CR1 of FP record forms (pending FPSCR model)"); continue
        keep.append(d)
    return keep, why


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("manifest"); ap.add_argument("oracle"); ap.add_argument("ours")
    ap.add_argument("--show", type=int, default=3)
    ap.add_argument("--json")
    ap.add_argument("--ignore", default="r1", help="comma-separated fields to skip (r1: the harness stack)")
    a = ap.parse_args()
    man = json.load(open(a.manifest))
    ignore = set(x for x in a.ignore.split(",") if x)
    lo, end_o = read_transcript(a.oracle)
    lu, end_u = read_transcript(a.ours)
    co, cu = split_cases(man, lo), split_cases(man, lu)
    per_op = OrderedDict()
    examples = defaultdict(list)
    missing = 0
    for c, (so, sco), (su, scu) in zip(man["cases"], co, cu):
        e = per_op.setdefault(c["op"], {"cases": 0, "bad": 0, "fields": defaultdict(int)})
        e["cases"] += 1
        if so is None or su is None:
            missing += 1
            e["bad"] += 1
            e["fields"]["<missing>"] += 1
            continue
        diffs = []
        mask = c.get("mask", {})
        ref = fpscr_reference(c)
        for name, off, n in FIELDS:
            if name in ignore and not (ref and name in ref):
                continue
            vo, vu, vi = so[off:off + n], su[off:off + n], bytes.fromhex(c["input"])[off:off + n]
            if ref and name in ref:
                vo = ref[name]
                if name == "fpscr" or name.startswith("f"):
                    # FPSCR is 32 bits; mffs leaves FRT[0:31] undefined
                    vo, vu = vo[4:], vu[4:]
            if name == "vscr":
                # RPCS3 keeps VSCR in BE word 0; the ISA (and ps3recomp) in word 3
                vo, vu, vi = vo[0:4], vu[12:16], vi[12:16]
            if c.get("vscr_dest") is not None and name == "v%d" % c["vscr_dest"]:
                vo = bytes(12) + vo[0:4]          # RPCS3 mfvscr result sits in word 0
            if name in mask:
                m = bytes.fromhex(mask[name])
                vo = bytes(x & y for x, y in zip(vo, m)); vu = bytes(x & y for x, y in zip(vu, m))
            if vo != vu:
                diffs.append((name, vo.hex(), vu.hex(), vi.hex()))
        # A store-conditional may fail spuriously (PowerISA); RPCS3's reservation
        # model sometimes does. Oracle-failed / ours-succeeded is allowed.
        if c.get("resv") and not (so[0x400] & 0x20) and (su[0x400] & 0x20):
            diffs = [x for x in diffs if x[0] != "cr"]
            sco = scu
        diffs, why = allowed(c, diffs)
        for w in why:
            allowed_tally[w] += 1
        if c["mem"] and sco != scu:
            k = next(i for i in range(len(sco)) if sco[i] != scu[i])
            diffs.append(("mem+0x%x" % k, sco[k:k + 16].hex(), scu[k:k + 16].hex(), ""))
        if diffs:
            e["bad"] += 1
            for d in diffs:
                e["fields"][d[0]] += 1
            if len(examples[c["op"]]) < a.show:
                examples[c["op"]].append((c, diffs))
    nbad = sum(1 for e in per_op.values() if e["bad"])
    print("oracle: %d lines%s   ours: %d lines%s" % (len(lo), "" if end_o else " (NO END)",
                                                    len(lu), "" if end_u else " (NO END)"))
    for opn, e in per_op.items():
        tag = "ok  " if not e["bad"] else "FAIL"
        f = "" if not e["bad"] else "  fields: " + ", ".join("%s(%d)" % kv for kv in
                                                              sorted(e["fields"].items(), key=lambda kv: -kv[1])[:6])
        print("%s %-12s %4d cases %4d mismatched%s" % (tag, opn, e["cases"], e["bad"], f))
    for opn, exs in examples.items():
        for c, diffs in exs:
            print("\n-- %s  words=%s  pc=0x%x" % (c["name"], " ".join("%08x" % w for w in c["words"]), c["pc"]))
            for name, o, u, i in diffs[:8]:
                print("   %-8s oracle=%s ours=%s  (in=%s)" % (name, o, u, i))
    print("\nops: %d  failing ops: %d  cases: %d  missing: %d" % (len(per_op), nbad, len(man["cases"]), missing))
    for w, n in sorted(allowed_tally.items()):
        print("allowed (documented oracle deviation): %-55s %d fields" % (w, n))
    if a.json:
        json.dump({k: {"cases": v["cases"], "bad": v["bad"], "fields": dict(v["fields"])}
                   for k, v in per_op.items()}, open(a.json, "w"), indent=1)
    sys.exit(1 if nbad or missing or not (end_o and end_u) else 0)


if __name__ == "__main__":
    main()
