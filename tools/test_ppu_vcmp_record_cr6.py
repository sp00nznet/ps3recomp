#!/usr/bin/env python3
"""Regression test: the record form of every AltiVec compare sets CR6.

AltiVec PEM (vcmp*.): CR6 = 0b1000 when the compare is true in every lane,
0b0010 when it is false in every lane; vcmpbfp.: CR6 = 0b0010 when every lane
is within bounds. vcmpequb. and vcmpequh. computed vD and left CR6 untouched,
so the `b[n]e cr6` after them followed whatever compare wrote CR6 last. The
other forms are covered as controls.

This test lifts each form, compiles the emitted C with the lifter's own lane
helpers, runs it and checks vD's CR6 effect and that no other CR field moved.
It needs a host C compiler (under CI=1 a missing one fails instead of skipping).
"""
from __future__ import annotations

import importlib.util
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]


def _load(name, rel_path):
    spec = importlib.util.spec_from_file_location(name, ROOT / rel_path)
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


L = _load("ppu_lifter", "tools/ppu_lifter.py")


def _lift_one(mnemonic, operands):
    insns = [SimpleNamespace(addr=0x1000, mnemonic=mnemonic, operands=operands),
             SimpleNamespace(addr=0x1004, mnemonic="blr", operands="")]
    func = L.PPULifter().lift_function(insns, 0x1000, 0x1008)
    return "\n".join(ln for ln in func.body_lines if "return" not in ln)


def _accessors():
    blob = "\n".join(L.PPULifter()._preamble_lines())
    start = blob.index("/* AltiVec register byte order")
    return blob[start:blob.index("/* The guest timebase")]


def _be(kind, vals):
    if kind == "f":
        return struct.pack(">4f", *vals)
    return struct.pack(">4I", *vals)


# (mnemonic, lane kind of vA/vB, vA, vB, expected CR6 field)
CASES = [
    ("vcmpequw.", "w", [1, 2, 3, 4], [1, 2, 3, 4], 0x8),
    ("vcmpequw.", "w", [1, 2, 3, 4], [5, 6, 7, 8], 0x2),
    ("vcmpequw.", "w", [1, 2, 3, 4], [1, 0, 3, 0], 0x0),
    ("vcmpeqfp.", "f", [1.0, -2.0, 0.5, 3.0], [1.0, -2.0, 0.5, 3.0], 0x8),
    ("vcmpeqfp.", "f", [1.0, -2.0, 0.5, 3.0], [9.0, 9.0, 9.0, 9.0], 0x2),
    ("vcmpgtfp.", "f", [2.0, 2.0, 2.0, 2.0], [1.0, 1.0, 1.0, 1.0], 0x8),
    ("vcmpgtfp.", "f", [0.0, 0.0, 0.0, 0.0], [1.0, 1.0, 1.0, 1.0], 0x2),
    ("vcmpgtfp.", "f", [2.0, 0.0, 2.0, 0.0], [1.0, 1.0, 1.0, 1.0], 0x0),
    ("vcmpgefp.", "f", [1.0, 1.0, 1.0, 1.0], [1.0, 1.0, 1.0, 1.0], 0x8),
    ("vcmpgefp.", "f", [0.0, 0.0, 0.0, 0.0], [1.0, 1.0, 1.0, 1.0], 0x2),
    # vcmpequh./vcmpequb. left CR6 stale before the fix
    ("vcmpequh.", "w", [0x00010002, 3, 4, 5], [0x00010002, 3, 4, 5], 0x8),
    ("vcmpequh.", "w", [0x00010002] * 4, [0x00030004] * 4, 0x2),
    ("vcmpequh.", "w", [0x00010002] * 4, [0x00010004] * 4, 0x0),
    ("vcmpequb.", "w", [0x01020304] * 4, [0x01020304] * 4, 0x8),
    ("vcmpequb.", "w", [0, 0, 0, 0], [0x01010101] * 4, 0x2),
    ("vcmpequb.", "w", [0x01020304] * 4, [0x01020305] * 4, 0x0),
    # vcmpbfp. with a NaN lane: NaN is out of bounds on both sides
    ("vcmpbfp.", "f", [0.5, float("nan"), 1.0, 0.0], [1.0, 1.0, 1.0, 1.0], 0x0),
    # vcmpbfp.: |a| <= b in every lane -> all in bounds -> 0b0010; one out -> 0
    ("vcmpbfp.", "f", [0.5, -0.5, 1.0, 0.0], [1.0, 1.0, 1.0, 1.0], 0x2),
    ("vcmpbfp.", "f", [0.5, -2.0, 1.0, 0.0], [1.0, 1.0, 1.0, 1.0], 0x0),
    # already correct (the integer vcmpgt* handlers set CR6 themselves)
    ("vcmpgtsw.", "w", [5, 5, 5, 5], [1, 1, 1, 1], 0x8),
]


def _cc():
    return shutil.which(os.environ.get("CC", "cc")) or shutil.which("clang") or shutil.which("gcc")


def check_vcmp_record_sets_cr6(cc, mn, kind, a, b, cr6):
    body = _lift_one(mn, "v3,v1,v2")
    src = "\n".join([
        "#include <stdint.h>", "#include <string.h>", "#include <stdio.h>",
        _accessors(),
        "typedef struct { uint8_t vr[32][16]; uint32_t cr; } ctx_t;",
        "int main(void) { static ctx_t c; ctx_t* ctx = &c;",
        "  const unsigned char A[16] = {" + ",".join(str(x) for x in _be(kind, a)) + "};",
        "  const unsigned char B[16] = {" + ",".join(str(x) for x in _be(kind, b)) + "};",
        "  memcpy(ctx->vr[1], A, 16); memcpy(ctx->vr[2], B, 16);",
        "  ctx->cr = 0xFFFFFFFFu;  /* stale CR6 = 0xF: must be overwritten */",
        body,
        "  printf(\"%X\\n\", (unsigned)((ctx->cr >> 4) & 0xF));",
        "  return (ctx->cr & ~0xF0u) == (0xFFFFFFFFu & ~0xF0u) ? 0 : 3; }",
    ])
    with tempfile.TemporaryDirectory() as td:
        c = Path(td) / "t.c"
        exe = Path(td) / ("t.exe" if os.name == "nt" else "t")
        c.write_text(src)
        r = subprocess.run([cc, "-std=c11", "-O1", "-o", str(exe), str(c)],
                           capture_output=True, text=True)
        assert r.returncode == 0, r.stderr + "\n" + src
        out = subprocess.run([str(exe)], capture_output=True, text=True)
        assert out.returncode == 0, "other CR fields were touched"
        assert int(out.stdout.strip(), 16) == cr6, f"{mn}: CR6={out.stdout.strip()} want {cr6:X}\n{body}"


def test_vcmp_record_sets_cr6():
    cc = _cc()
    if cc is None:
        # same rule as the conformance suite: a skipped run must not pass in CI
        if os.environ.get("CI"):
            raise SystemExit("FAIL: no C compiler found")
        print("skip test_vcmp_record_sets_cr6: no C compiler")
        return
    for case in CASES:
        check_vcmp_record_sets_cr6(cc, *case)
    print(f"  {len(CASES)} compare forms checked")


def test_non_record_compare_leaves_cr_alone():
    body = _lift_one("vcmpequw", "v3,v1,v2")
    assert "ctx->cr" not in body


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print("ok", name)
