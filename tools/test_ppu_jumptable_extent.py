#!/usr/bin/env python3
"""End-to-end: a switch whose case blocks lie past the function's first blr.

A function inventory that ends functions at their first `blr` (or an export
that truncates them) cuts a switch function in two: the dispatch stays in the
declared range and the case blocks, which sit after an early-return `blr` and
share a later epilogue, are outside it. Once the dispatcher is resolved,
ppu_lifter.main() extends the enclosing function over its cases and the bctr
becomes a computed goto to internal labels. If the dispatcher is NOT resolved,
nothing extends the function, the bctr lifts to ps3_indirect_call and the case
blocks only exist as unrelated fragments.

The vector here combines that shape with a table base that was TOC-loaded,
spilled to the stack and reloaded at the dispatch (the base register then
reused for the case address), which discover_jump_tables used to drop. It
builds a tiny big-endian ELF64, passes a --functions list that ends the switch
function at its first blr, runs the real lifter, and checks the emitted C.

Run: python tools/test_ppu_jumptable_extent.py
"""

import glob
import json
import os
import re
import struct
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)

import ppu_disasm                                   # noqa: E402
import ppu_lifter                                   # noqa: E402
from test_ppu_jumptables import (w_add, w_b, w_bc, w_blr, w_bctr, w_cmplwi,  # noqa: E402
                                 w_ld, w_li, w_lwz, w_lwzx, w_mtctr, w_slwi,
                                 w_std, w_stdu)

DATA = 0x10000          # TOC, OPD entry descriptor, jump table
TEXT = 0x20000
TOC = DATA + 0x8000
TABLE = DATA + 0x100


def w_addi(rt, ra, si):    return (14 << 26) | (rt << 21) | (ra << 16) | (si & 0xFFFF)
def _spr(n):               return ((n & 0x1F) << 5) | ((n >> 5) & 0x1F)
def w_mflr(rt):            return (31 << 26) | (rt << 21) | (_spr(8) << 11) | (339 << 1)
def w_mtlr(rs):            return (31 << 26) | (rs << 21) | (_spr(8) << 11) | (467 << 1)


def build():
    """Return (text words, label addresses). Layout of the switch function:

        mflr r0 ; stdu r1,-0x300(r1) ; std r0,0x310(r1)
        lwz r10,16(r2) ; std r10,0x2D0(r1)          TOC-load, spill
        li r0,1 (the index) ; cmplwi r0,2 ; bgt early
        ld r11,0x2D0(r1) ; slwi r9,r0,2 ; lwzx r0,r9,r11
        add r11,r0,r11 ; mtctr r11 ; bctr
      early:  li r3,-1 ; addi r1,r1,0x300 ; blr     FIRST blr
      case0/1/2: li r3,N ; b done
      done:   ld r0,0x310(r1) ; mtlr r0 ; addi r1,r1,0x300 ; blr
    followed by an unrelated function (its own prologue)."""
    w, at = [], {}

    def here(name):
        at[name] = TEXT + 4 * len(w)

    here("func")
    w += [w_mflr(0), w_stdu(1, 1, -0x300), w_std(0, 1, 0x310),
          w_lwz(10, 2, 16), w_std(10, 1, 0x2D0),
          w_li(0, 1), w_cmplwi(0, 0, 2)]
    bgt_at = len(w); w.append(0)                     # patched below
    w += [w_ld(11, 1, 0x2D0), w_slwi(9, 0, 2), w_lwzx(0, 9, 11),
          w_add(11, 0, 11), w_mtctr(11)]
    here("bctr"); w.append(w_bctr())
    here("early"); w += [w_li(3, -1), w_addi(1, 1, 0x300), w_blr()]
    here("first_end")
    branches = []
    for n in range(3):
        here(f"case{n}"); w.append(w_li(3, 10 + n))
        branches.append(len(w)); w.append(0)
    here("done"); w += [w_ld(0, 1, 0x310), w_mtlr(0), w_addi(1, 1, 0x300), w_blr()]
    here("next"); w += [w_mflr(0), w_stdu(1, 1, -0x80), w_addi(1, 1, 0x80), w_blr()]
    here("end")
    w[bgt_at] = w_bc(12, 1, at["early"] - (TEXT + 4 * bgt_at))
    for k in branches:
        w[k] = w_b(at["done"] - (TEXT + 4 * k))
    return w, at


def elf64(text, data):
    """Minimal ET_EXEC, big-endian, two PT_LOADs (RX text, RW data), entry =
    the OPD descriptor {code, toc} at the start of data."""
    ehsz, phsz = 64, 56
    off_text = 0x1000
    off_data = off_text + ((len(text) + 0xFFF) & ~0xFFF)
    hdr = b"\x7fELF" + bytes([2, 2, 1, 0]) + bytes(8)
    hdr += struct.pack(">HHIQQQIHHHHHH", 2, 21, 1, DATA, ehsz, 0, 0,
                       ehsz, phsz, 2, 64, 0, 0)
    ph = struct.pack(">IIQQQQQQ", 1, 5, off_text, TEXT, TEXT, len(text), len(text), 0x10000)
    ph += struct.pack(">IIQQQQQQ", 1, 6, off_data, DATA, DATA, len(data), len(data), 0x10000)
    img = bytearray(off_data + len(data))
    img[:len(hdr)] = hdr
    img[ehsz:ehsz + len(ph)] = ph
    img[off_text:off_text + len(text)] = text
    img[off_data:off_data + len(data)] = data
    return bytes(img)


def lift(td, words, at, bounds):
    text = b"".join(struct.pack(">I", x) for x in words)
    data = bytearray(0x8000 + 0x100)
    struct.pack_into(">II", data, 0, at["func"], TOC)               # entry OPD
    struct.pack_into(">I", data, TOC + 16 - DATA, TABLE)            # TOC slot -> table
    for n in range(3):                                              # offset table
        struct.pack_into(">I", data, TABLE - DATA + 4 * n, at[f"case{n}"] - TABLE)
    elf = os.path.join(td, "switch.elf")
    with open(elf, "wb") as f:
        f.write(elf64(text, bytes(data)))
    funcs = os.path.join(td, "functions.json")
    with open(funcs, "w") as f:
        json.dump([{"start": hex(s), "end": hex(e)} for s, e in bounds], f)
    out = os.path.join(td, "lifted")
    old = sys.argv
    try:
        sys.argv = ["ppu_lifter.py", elf, "--functions", funcs, "--output", out,
                    "--jobs", "1"]
        ppu_lifter.main()
    finally:
        sys.argv = old
    src = ""
    for p in sorted(glob.glob(os.path.join(out, "*.c*"))):
        with open(p) as f:
            src += f.read()
    return src


def body(src, addr):
    m = re.search(r"^void func_%08X\(ppu_context\* ctx\) \{\n(.*?)^\}\n" % addr, src, re.M | re.S)
    return m.group(1) if m else None


FAILS = []


def check(name, ok, detail=""):
    print(f"{'ok  ' if ok else 'FAIL'} {name}{'' if ok else ': ' + detail}")
    if not ok:
        FAILS.append(name)


def main():
    words, at = build()
    for i, x in enumerate(words):                    # encoding sanity
        if ppu_disasm.decode(x, TEXT + 4 * i).mnemonic.startswith((".", "unk")):
            print(f"ENCODING word {i}: 0x{x:08X}")
            return 1
    # The inventory ends the switch function at its first blr.
    bounds = [(at["func"], at["first_end"]), (at["next"], at["end"])]
    with tempfile.TemporaryDirectory() as td:
        src = lift(td, words, at, bounds)
    fn = body(src, at["func"])
    check("switch function emitted", fn is not None)
    fn = fn or ""
    cases = [at[f"case{n}"] for n in range(3)]
    check("bctr is a computed goto to every case",
          all(f"case 0x{c:08X}u: goto loc_{c:08X};" in fn for c in cases),
          "dispatcher missing or cases outside the function")
    check("every case label is inside the function",
          all(f"loc_{c:08X}:" in fn for c in cases))
    check("the shared epilogue past the first blr is inside the function",
          f"loc_{at['done']:08X}:" in fn)
    check("no unresolved indirect call outside the switch default",
          "ps3_indirect_call" not in fn.replace("default: ps3_indirect_call(ctx); return; }", ""))
    check("the next function stays its own function", body(src, at["next"]) is not None)
    if FAILS:
        print(f"FAILED: {', '.join(FAILS)}")
        return 1
    print("switch function spans its case blocks past the first blr")
    return 0


if __name__ == "__main__":
    sys.exit(main())
