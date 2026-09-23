#!/usr/bin/env python3
"""Unit vectors for discover_jump_tables: which lwzx operand is the table base.

No SDK, no ELF: the PowerPC words are hand-assembled and memory is a dict, so
this runs anywhere python does. Every word is round-tripped through
ppu_disasm.decode first, so an encoding mistake here reports as ENCODING and
never as a false lifter failure.

The bug this pins: `lwzx rD, rA, rB` computes MEM(rA + rB), so either operand
can be the table base, and both are often defined by an r2-relative load (the
base from its TOC slot, the index from a TOC-addressed global). The `add`-based
reordering in discover_jump_tables only promotes a candidate when an `add`
combines the loaded value with it -- the OFFSET-table idiom. An ABSOLUTE table
has no `add`, so the candidates stayed in raw operand order and the first one
that resolved was taken as the base without its table ever being scored. When
that was the index register, the table read from its slot validated zero case
targets, `best` stayed empty and the dispatcher was dropped with no message.
The bctr then lifted to an unresolved indirect call.

The fix scores every operand that resolves, the way the multi-TOC loop already
scores TOC candidates, and keeps the one that validates the most targets. Ties
go to the earlier candidate, so a dispatcher only changes when the base picked
before validated strictly fewer targets. The vectors below cover the defect,
the offset idiom that must not move, and the tie-break.

Run: python tools/test_ppu_jumptables.py
"""

import os
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)

import ppu_disasm                                   # noqa: E402
from ppu_lifter import discover_jump_tables         # noqa: E402

TEXT_LO, TEXT_HI = 0x10000, 0x20000
TOC = 0x100000


def w_cmplwi(bf, ra, ui):  return (10 << 26) | (bf << 23) | (ra << 16) | (ui & 0xFFFF)
def w_lwz(rt, ra, d):      return (32 << 26) | (rt << 21) | (ra << 16) | (d & 0xFFFF)
def w_lwzx(rt, ra, rb):    return (31 << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (23 << 1)
def w_add(rt, ra, rb):     return (31 << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (266 << 1)
def w_slwi(ra, rs, n):     return (21 << 26) | (rs << 21) | (ra << 16) | (n << 11) | (0 << 6) | ((31 - n) << 1)
def w_bctr():              return (19 << 26) | (20 << 21) | (528 << 1)
def w_blr():               return (19 << 26) | (20 << 21) | (16 << 1)
def w_ld(rt, ra, d):       return (58 << 26) | (rt << 21) | (ra << 16) | (d & 0xFFFC)
def w_std(rs, ra, d):      return (62 << 26) | (rs << 21) | (ra << 16) | (d & 0xFFFC)
def w_bc(bo, bi, rel):     return (16 << 26) | (bo << 21) | (bi << 16) | (rel & 0xFFFC)


def w_mtctr(rs):
    spr = ((9 & 0x1F) << 5) | ((9 >> 5) & 0x1F)     # CTR is SPR 9, halves swapped
    return (31 << 26) | (rs << 21) | (spr << 11) | (467 << 1)


def decode_all(words):
    return [ppu_disasm.decode(w, TEXT_LO + 4 * i) for i, w in enumerate(words)]


def discover(words, mem):
    insns = decode_all(words)
    tables = discover_jump_tables(insns, lambda a: mem.get(a & 0xFFFFFFFF),
                                  [TOC], TEXT_LO, TEXT_HI)
    return tables.get(insns[-1].addr)                # the bctr is always last


FAILS = []


def check(name, got, want):
    if got == want:
        print(f"ok   {name}")
    else:
        show = lambda v: None if v is None else [f"0x{t:X}" for t in v]
        print(f"FAIL {name}: got {show(got)}, want {show(want)}")
        FAILS.append(name)


def test_encodings():
    ins = decode_all([w_cmplwi(0, 9, 3), w_lwz(11, 2, 16), w_lwzx(0, 9, 11),
                      w_add(0, 0, 11), w_slwi(9, 3, 2), w_mtctr(0), w_bctr()])
    got = [i.mnemonic for i in ins]
    want = ['cmplwi', 'lwz', 'lwzx', 'add', 'rlwinm', 'mtctr', 'bctr']   # slwi is rlwinm
    if got != want:
        print(f"ENCODING mismatch: got {got}, want {want}")
        sys.exit(2)
    print("ok   encodings decode as the vectors assume")


def test_absolute_base_is_second_operand():
    """ABSOLUTE table, both operands r2-loaded, the real base is the SECOND.

        cmplwi cr0, r9, 2
        lwz    r9,  8(r2)       <- a TOC-addressed global: the index, not a base
        lwz    r11, 16(r2)      <- the table base
        lwzx   r0,  r9, r11     <- index first, base second
        mtctr  r0 ; bctr        <- no `add`: the entries are case addresses

    Before the fix r9 resolved first and was taken as the base. Its slot holds
    the index's initial value (0), nothing validates there, and the whole
    dispatcher was dropped -- discover() returned None.
    """
    table = 0x200400
    mem = {TOC + 8: 0,
           TOC + 16: table,
           table + 0: 0x13000, table + 4: 0x13004, table + 8: 0x13008,
           table + 12: 0}                            # out of .text: the table ends
    got = discover([w_cmplwi(0, 9, 2), w_lwz(9, 2, 8), w_lwz(11, 2, 16),
                    w_lwzx(0, 9, 11), w_mtctr(0), w_bctr()], mem)
    check("absolute table, base is the second lwzx operand",
          got, [0x13000, 0x13004, 0x13008])


def test_offset_idiom():
    """The gcc offset idiom the pass was written for; only r11 is r2-loaded.

        lwz r11,16(r2); slwi r9,r3,2; lwzx r0,r9,r11; add r0,r0,r11; mtctr r0; bctr
    """
    table = 0x11000                                  # base inside .text, as gcc emits it
    mem = {TOC + 16: table,
           table + 0: 0x100, table + 4: 0x200, table + 8: 0x300,
           table + 12: 0xFFFFFFFF}                   # misaligned target: the table ends
    got = discover([w_cmplwi(0, 3, 2), w_lwz(11, 2, 16), w_slwi(9, 3, 2),
                    w_lwzx(0, 9, 11), w_add(0, 0, 11), w_mtctr(0), w_bctr()], mem)
    check("offset table, gcc idiom", got, [0x11100, 0x11200, 0x11300])


def test_offset_both_operands_r2_loaded():
    """Offset table with the index r2-loaded and first (the flOw shape).

    The `add` names r11, so the reordering already put it first; this pins that
    scoring the other operand too does not change the result. r9's slot points
    at a real-looking absolute table with ONE valid entry, fewer than the two the
    offset table validates, so the add-paired base must still win.
    """
    table, other = 0x11000, 0x200700
    mem = {TOC + 8: other, other + 0: 0x16000, other + 4: 0,
           TOC + 16: table,
           table + 0: 0x100, table + 4: 0x200, table + 8: 0xFFFFFFFF}
    got = discover([w_cmplwi(0, 9, 2), w_lwz(9, 2, 8), w_lwz(11, 2, 16),
                    w_lwzx(0, 9, 11), w_add(0, 0, 11), w_mtctr(0), w_bctr()], mem)
    check("offset table, both operands r2-loaded", got, [0x11100, 0x11200])


def test_tie_keeps_first_candidate():
    """Both operands decode tables of equal size: the first candidate stays.

    This is what the lifter did before the fix, so a dispatcher can only change
    when the old base validated strictly fewer targets than another operand.
    """
    ta, tb = 0x200500, 0x200600
    mem = {TOC + 8: ta, ta + 0: 0x14000, ta + 4: 0x14004, ta + 8: 0,
           TOC + 16: tb, tb + 0: 0x15000, tb + 4: 0x15004, tb + 8: 0}
    got = discover([w_cmplwi(0, 9, 2), w_lwz(9, 2, 8), w_lwz(11, 2, 16),
                    w_lwzx(0, 9, 11), w_mtctr(0), w_bctr()], mem)
    check("tie between operands keeps the first", got, [0x14000, 0x14004])


def test_two_level_base_past_early_return():
    """Two-level base loaded in the prologue, an early return in between.

        lwz    r30, 8(r2)        <- prologue: the base's base
        cmplwi cr0, r3, 2
        bc     (le) -> +12       <- to the dispatch
        ld     r30, 0(r1)        <- early-return epilogue restores r30...
        blr                      <- ...and returns: never reaches the bctr
        lwz    r11, -16(r30)     <- the table base, through r30
        slwi r9,r3,2; lwzx r0,r9,r11; add r0,r0,r11; mtctr r0; bctr

    GH3 func_0074715C. The base scans stopped at the early blr (missing the
    prologue load), and once widened they latched the epilogue's `ld r30` as the
    nearest definition. Dropped, the switch lifted to an indirect tail call and
    the case bodies clobbered the caller's r25.
    """
    mid, table = 0x200800, 0x11000
    mem = {TOC + 8: mid, mid - 16: table,
           table + 0: 0x100, table + 4: 0x200, table + 8: 0xFFFFFFFF}
    words = [w_lwz(30, 2, 8), w_cmplwi(0, 3, 2), w_bc(4, 1, 12),
             w_ld(30, 1, 0), w_blr(),
             w_lwz(11, 30, -16), w_slwi(9, 3, 2), w_lwzx(0, 9, 11),
             w_add(0, 0, 11), w_mtctr(0), w_bctr()]
    insns = decode_all(words)
    tables = discover_jump_tables(insns, lambda a: mem.get(a & 0xFFFFFFFF),
                                  [TOC], TEXT_LO, TEXT_HI, func_starts=[TEXT_LO])
    check("two-level base past an early return", tables.get(insns[-1].addr),
          [0x11100, 0x11200])


def test_base_before_fallthrough_split():
    """The table base is loaded before a split point that is not an entry.

        lwz  r30, 8(r2)         <- first "function"
        std  r30, 0x450(r1)     <- a STORE of r30: reads it, must not end the scan
        slwi r9, r3, 2          <- falls through into...
        lwz  r11, -16(r30)      <- ...a func_starts entry (GH3 0x0076571C)
        lwzx r0,r9,r11; add r0,r0,r11; mtctr r0; bctr

    find_functions cut the function here, so bounding the scan at the nearest
    start hid the r30 load. A start the previous instruction falls into is
    skipped.
    """
    mid, table = 0x200900, 0x11000
    mem = {TOC + 8: mid, mid - 16: table,
           table + 0: 0x100, table + 4: 0x200, table + 8: 0xFFFFFFFF}
    words = [w_lwz(30, 2, 8), w_std(30, 1, 0x450), w_slwi(9, 3, 2),
             w_lwz(11, 30, -16), w_lwzx(0, 9, 11), w_add(0, 0, 11),
             w_mtctr(0), w_bctr()]
    insns = decode_all(words)
    tables = discover_jump_tables(insns, lambda a: mem.get(a & 0xFFFFFFFF),
                                  [TOC], TEXT_LO, TEXT_HI,
                                  func_starts=[TEXT_LO, TEXT_LO + 12])
    check("base before a fall-through split point", tables.get(insns[-1].addr),
          [0x11100, 0x11200])


def main() -> int:
    test_encodings()
    test_absolute_base_is_second_operand()
    test_offset_idiom()
    test_offset_both_operands_r2_loaded()
    test_tie_keeps_first_candidate()
    test_two_level_base_past_early_return()
    test_base_before_fallthrough_split()
    if FAILS:
        print(f"FAILED: {', '.join(FAILS)}")
        return 1
    print("all jump-table vectors passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
