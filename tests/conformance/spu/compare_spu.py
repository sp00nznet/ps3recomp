#!/usr/bin/env python3
"""Per-instruction diff of the SPU conformance transcripts.

usage: compare_spu.py MANIFEST ORACLE OURS [--show N] [--json OUT]

Each case line holds the registers its operand fields name (rt, ra, rb, rc
order, 16 bytes each). For every op: cases, mismatching cases, and for the
first N mismatches the instruction word, the inputs, and both results.
"""
import argparse
import json
import os
import re
import sys
from collections import OrderedDict

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..",
                                "runtime", "spu", "tests"))
from spu_float_referee import FLOAT_OPS, spec  # noqa: E402  (ISA single-precision model)

# Ops whose RPCS3 static-interpreter result is known not to be the Cell's: double precision in
# host IEEE without the Cell's denormal/NaN rules, fesd/frds likewise, fi a TODO approximation.
# They are not judged here; runtime/spu/tests/build_diff_test.sh checks them against RPCS3's
# precise model and the ISA.
ORACLE_INEXACT = {"dfa", "dfs", "dfm", "dfma", "dfms", "dfnms", "dfnma", "fesd", "frds", "fi"}


def case_lines(path):
    return [l.strip() for l in open(path, "rb").read().decode("latin-1").splitlines()
            if re.fullmatch(r"[0-9a-f]{128}", l.strip())]


def qwords(h):
    return [h[i:i + 32] for i in range(0, 128, 32)]


def by_isa(c, want, got):
    """True when every differing word of rt is one where ours is the SPU ISA's exact result
    (or the ISA leaves it undefined) -- RPCS3's interpreter does single precision in host IEEE."""
    w, g = qwords(want), qwords(got)
    rt = c["regs"][0]
    for k, r in enumerate(c["regs"][1:], 1):   # only rt may differ (or a field naming rt)
        if r != rt and w[k] != g[k]:
            return False
    ins = [bytes.fromhex(q) for q in c["inputs"]]
    word = c["word"]
    regs = c["regs"]
    # A register named by several fields was loaded once, from its first field's input.
    held = [ins[regs.index(r)] for r in regs]
    def lane(j, k):
        return int.from_bytes(held[j][4*k:4*k+4], "big") if j < len(held) else 0
    for k in range(4):
        a, b, cc = lane(1, k), lane(2, k), lane(3, k)
        ow, gw = int(w[0][8*k:8*k+8], 16), int(g[0][8*k:8*k+8], 16)
        if ow == gw:
            continue
        s_ = spec(c["op"], word, a, b, cc)
        if s_ is not None and s_ != gw:
            return False
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("manifest"); ap.add_argument("oracle"); ap.add_argument("ours")
    ap.add_argument("--show", type=int, default=2)
    ap.add_argument("--json")
    a = ap.parse_args()
    man = json.load(open(a.manifest))
    cases = man["cases"]
    want, got = case_lines(a.oracle), case_lines(a.ours)
    if len(want) != len(cases) or len(got) != len(cases):
        print("line counts: cases %d oracle %d ours %d" % (len(cases), len(want), len(got)))
    per = OrderedDict()
    for i, c in enumerate(cases):
        p = per.setdefault(c["op"], dict(n=0, bad=[]))
        p["n"] += 1
        if i < len(want) and i < len(got) and want[i] != got[i]:
            if c["op"] in ORACLE_INEXACT:
                p["skip"] = p.get("skip", 0) + 1
            elif c["op"] in FLOAT_OPS and by_isa(c, want[i], got[i]):
                p["isa"] = p.get("isa", 0) + 1     # RPCS3 differs; ours is the ISA's answer
            else:
                p["bad"].append(i)
    nbad = 0
    for op, p in per.items():
        if not p["bad"]:
            continue
        nbad += 1
        print("FAIL %-8s %3d/%d cases" % (op, len(p["bad"]), p["n"]))
        for i in p["bad"][:a.show]:
            c = cases[i]
            names = ["rt", "ra", "rb", "rc"][:len(c["regs"])]
            print("     word %08X regs %s" % (c["word"], " ".join("%s=r%d" % (n, r) for n, r in zip(names, c["regs"]))))
            print("       in : %s" % " ".join(q for q in c["inputs"][:len(c["regs"])]))
            w, g = qwords(want[i]), qwords(got[i])
            for k, n in enumerate(names):
                if w[k] != g[k]:
                    print("       %s oracle %s\n       %s ours   %s" % (n, w[k], " " * len(n), g[k]))
    isa = sum(p.get("isa", 0) for p in per.values())
    if isa:
        print("%d case(s) where RPCS3's float result differs and ours is the SPU ISA's exact result" % isa)
    skip = sorted(op for op, p in per.items() if p.get("skip"))
    if skip:
        print("not judged here (RPCS3's interpreter is inexact; see build_diff_test.sh): %s" % " ".join(skip))
    print("ops: %d  failing: %d  cases: %d" % (len(per), nbad, len(cases)))
    if a.json:
        json.dump({op: dict(n=p["n"], bad=len(p["bad"])) for op, p in per.items()}, open(a.json, "w"), indent=1)
    sys.exit(1 if nbad else 0)


if __name__ == "__main__":
    main()
