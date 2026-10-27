#!/usr/bin/env python3
"""Regression test: ten VMX integer ops use big-endian lane order.

ctx->vr holds raw big-endian guest bytes, so an op that reads halfword or word
VALUES must assemble each lane from its bytes in big-endian order. vmulouh,
vmsumshm, vadduws, vadduhs and vpkuwum read lanes through int16_t*/uint32_t*
casts (byte-reversed lanes on a little-endian host); vmsumuhm, vsubuws,
vpkswss, vsrb and vsrab had no lowering and were emitted as "/* TODO */".

For each op this test lifts one instruction, compiles the emitted C with the
lifter's own preamble, runs it on random inputs (plus edge values) and
compares vD with an independent Python model of the instruction as the
AltiVec PEM defines it. It needs a host C compiler (under CI=1 a missing one
fails instead of skipping).
"""
from __future__ import annotations

import importlib.util
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
CASES_PER_OP = 300


def _load(name, rel_path):
    spec = importlib.util.spec_from_file_location(name, ROOT / rel_path)
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


L = _load("ppu_lifter", "tools/ppu_lifter.py")


# The instruction words, for the fixed operands check_op uses (vD=4, vA=1,
# vB=2, vC=3): the lifter routes VMX by raw XO (#286), so the test must hand
# it the same word the disassembler would.
XO = {"vmulouh": 72, "vadduws": 640, "vsubuws": 1664, "vadduhs": 576,
      "vpkuwum": 78, "vpkswss": 462, "vsrb": 516, "vsrab": 772,
      "vmsumuhm": (3 << 6) | 38, "vmsumshm": (3 << 6) | 40}


def _lift_one(mnemonic, operands):
    raw = (4 << 26) | (4 << 21) | (1 << 16) | (2 << 11) | XO[mnemonic]
    insns = [SimpleNamespace(addr=0x1000, mnemonic=mnemonic, operands=operands, raw=raw),
             SimpleNamespace(addr=0x1004, mnemonic="blr", operands="", raw=0x4E800020)]
    func = L.PPULifter().lift_function(insns, 0x1000, 0x1008)
    return "\n".join(ln for ln in func.body_lines if "return" not in ln)


def _accessors():
    blob = "\n".join(L.PPULifter()._preamble_lines())
    start = blob.index("/* AltiVec register byte order")
    return blob[start:blob.index("/* The guest timebase")]


# ---- reference model (AltiVec PEM), on big-endian byte strings -------------
def h(v, signed=False):
    return list(struct.unpack(">8h" if signed else ">8H", v))


def w(v, signed=False):
    return list(struct.unpack(">4i" if signed else ">4I", v))


def ph(xs):
    return struct.pack(">8H", *[x & 0xFFFF for x in xs])


def pw(xs):
    return struct.pack(">4I", *[x & 0xFFFFFFFF for x in xs])


def sat(x, lo, hi):
    return lo if x < lo else hi if x > hi else x


def ref(mn, a, b, c):
    if mn == "vmulouh":
        return pw([h(a)[2 * i + 1] * h(b)[2 * i + 1] for i in range(4)])
    if mn in ("vmsumshm", "vmsumuhm"):
        s = mn == "vmsumshm"
        ha, hb = h(a, s), h(b, s)
        return pw([w(c)[i] + ha[2 * i] * hb[2 * i] + ha[2 * i + 1] * hb[2 * i + 1] for i in range(4)])
    if mn == "vadduws":
        return pw([sat(x + y, 0, 0xFFFFFFFF) for x, y in zip(w(a), w(b))])
    if mn == "vsubuws":
        return pw([sat(x - y, 0, 0xFFFFFFFF) for x, y in zip(w(a), w(b))])
    if mn == "vadduhs":
        return ph([sat(x + y, 0, 0xFFFF) for x, y in zip(h(a), h(b))])
    if mn == "vpkuwum":
        return ph(w(a) + w(b))
    if mn == "vpkswss":
        return ph([sat(x, -32768, 32767) for x in w(a, True) + w(b, True)])
    if mn == "vsrb":
        return bytes(x >> (y & 7) for x, y in zip(a, b))
    if mn == "vsrab":
        return bytes(((x - 256 if x > 127 else x) >> (y & 7)) & 0xFF for x, y in zip(a, b))
    raise KeyError(mn)


OPS = ["vmulouh", "vmsumshm", "vmsumuhm", "vadduws", "vsubuws", "vadduhs",
       "vpkuwum", "vpkswss", "vsrb", "vsrab"]
FOUR = {"vmsumshm", "vmsumuhm"}
EDGE = [bytes(16), b"\xff" * 16, b"\x7f\xff\xff\xff" * 4, b"\x80\x00\x00\x00" * 4,
        b"\x00\x01\x80\x00\xff\xfe\x7f\xff" * 2, bytes(range(16))]


def _vectors(rng):
    out = [(x, y, z) for x in EDGE for y in EDGE for z in EDGE[:2]]
    while len(out) < CASES_PER_OP:
        out.append(tuple(bytes(rng.getrandbits(8) for _ in range(16)) for _ in range(3)))
    return out


def _carr(bs):
    return "{" + ",".join(str(x) for x in bs) + "}"


def check_op(cc, mn, rng):
    operands = "v4,v1,v2,v3" if mn in FOUR else "v4,v1,v2"
    body = _lift_one(mn, operands)
    if "TODO" in body:
        return f"{mn}: no lowering (emitted {body.strip()[:60]})"
    vecs = _vectors(rng)
    src = "\n".join([
        "#include <stdint.h>", "#include <string.h>", "#include <stdio.h>",
        _accessors(),
        "#include \"ppu_vmx.h\"",   # the helpers #286 lowers VMX to
        "typedef struct { uint8_t vr[32][16]; uint32_t vscr; } ctx_t;",
        f"static const unsigned char IN[{len(vecs)}][3][16] = {{" +
        ",".join("{" + ",".join(_carr(v) for v in t) + "}" for t in vecs) + "};",
        "int main(void) { static ctx_t c; ctx_t* ctx = &c;",
        f"  for (int n = 0; n < {len(vecs)}; n++) {{",
        "    memcpy(ctx->vr[1], IN[n][0], 16); memcpy(ctx->vr[2], IN[n][1], 16);",
        "    memcpy(ctx->vr[3], IN[n][2], 16); memset(ctx->vr[4], 0xCD, 16);",
        "    " + body,
        "    for (int i = 0; i < 16; i++) printf(\"%02x\", ctx->vr[4][i]); printf(\"\\n\"); }",
        "  return 0; }",
    ])
    with tempfile.TemporaryDirectory() as td:
        cpath = Path(td) / "t.c"
        exe = Path(td) / ("t.exe" if os.name == "nt" else "t")
        cpath.write_text(src)
        r = subprocess.run([cc, "-std=c11", "-O1", "-I" + str(ROOT / "runtime" / "ppu"), "-o", str(exe), str(cpath)],
                           capture_output=True, text=True)
        if r.returncode:
            return f"{mn}: emitted C does not compile: {r.stderr.strip()[:200]}"
        out = subprocess.run([str(exe)], capture_output=True, text=True).stdout.split()
    bad = 0
    first = None
    for (a, b, cc_), got in zip(vecs, out):
        want = ref(mn, a, b, cc_).hex()
        if got != want:
            bad += 1
            first = first or (a.hex(), b.hex(), cc_.hex(), got, want)
    if bad:
        return (f"{mn}: {bad}/{len(vecs)} cases differ, first: a={first[0]} b={first[1]} c={first[2]} "
                f"got={first[3]} want={first[4]}")
    return None


def test_vmx_big_endian_lanes():
    cc = shutil.which(os.environ.get("CC", "cc")) or shutil.which("clang") or shutil.which("gcc")
    if cc is None:
        if os.environ.get("CI"):
            raise SystemExit("FAIL: no C compiler found")
        print("skip test_vmx_big_endian_lanes: no C compiler")
        return
    rng = random.Random(214)
    errors = []
    for mn in OPS:
        e = check_op(cc, mn, rng)
        print(f"  {'FAIL' if e else 'ok  '} {e or mn}")
        if e:
            errors.append(e)
    assert not errors, f"{len(errors)}/{len(OPS)} ops wrong"
    print(f"  {len(OPS)} ops x {CASES_PER_OP} cases agree with the reference model")


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print("ok", name)
