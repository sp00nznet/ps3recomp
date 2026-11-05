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
def w_lwax(rt, ra, rb):    return (31 << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (341 << 1)
def w_add(rt, ra, rb):     return (31 << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (266 << 1)
def w_slwi(ra, rs, n):     return (21 << 26) | (rs << 21) | (ra << 16) | (n << 11) | (0 << 6) | ((31 - n) << 1)
def w_bctr():              return (19 << 26) | (20 << 21) | (528 << 1)
def w_blr():               return (19 << 26) | (20 << 21) | (16 << 1)
def w_ld(rt, ra, d):       return (58 << 26) | (rt << 21) | (ra << 16) | (d & 0xFFFC)
def w_std(rs, ra, d):      return (62 << 26) | (rs << 21) | (ra << 16) | (d & 0xFFFC)
def w_li(rt, si):          return (14 << 26) | (rt << 21) | (si & 0xFFFF)
def w_addis(rt, ra, si):   return (15 << 26) | (rt << 21) | (ra << 16) | (si & 0xFFFF)
def w_stdu(rs, ra, d):     return (62 << 26) | (rs << 21) | (ra << 16) | (d & 0xFFFC) | 1
def w_bc(bo, bi, rel):     return (16 << 26) | (bo << 21) | (bi << 16) | (rel & 0xFFFC)
def w_b(rel):              return (18 << 26) | (rel & 0x3FFFFFC)


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
                      w_add(0, 0, 11), w_slwi(9, 3, 2), w_mtctr(0), w_bctr(),
                      w_lwax(0, 9, 11)])
    got = [i.mnemonic for i in ins]
    want = ['cmplwi', 'lwz', 'lwzx', 'add', 'rlwinm', 'mtctr', 'bctr', 'lwax']   # slwi is rlwinm
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


def test_offset_idiom_lwax():
    """The offset idiom with the table loaded by `lwax` instead of `lwzx`.

        lwz r11,16(r2); slwi r9,r3,2; lwax r0,r9,r11; add r0,r0,r11; mtctr r0; bctr

    `lwax` is the sign-extending word load; some compilers use it for offset
    tables, whose entries are signed (a case body may sit below the table, as
    the negative entry here does). Only `lwzx` was accepted as the indexed load,
    so the dispatcher was dropped and the bctr lifted to an unresolved indirect
    call.
    """
    table = 0x11000
    mem = {TOC + 16: table,
           table + 0: 0x100, table + 4: (-0x200) & 0xFFFFFFFF, table + 8: 0x300,
           table + 12: 0xFFFFFFFF}                   # misaligned target: the table ends
    got = discover([w_cmplwi(0, 3, 2), w_lwz(11, 2, 16), w_slwi(9, 3, 2),
                    w_lwax(0, 9, 11), w_add(0, 0, 11), w_mtctr(0), w_bctr()], mem)
    check("offset table loaded with lwax", got, [0x10E00, 0x11100, 0x11300])


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


def test_two_level_base_past_tail_call():
    """Same as above, but the early exit leaves by a tail call, not a blr.

        lwz    r30, 8(r2)        <- prologue: the base's base
        cmplwi cr0, r3, 2
        bc     (le) -> +12       <- to the dispatch
        ld     r30, 0(r1)        <- early-exit epilogue restores r30...
        b      elsewhere         <- ...and tail-calls another function
        lwz    r11, -16(r30)     <- the table base, through r30
        slwi r9,r3,2; lwzx r0,r9,r11; add r0,r0,r11; mtctr r0; bctr

    inFAMOUS 2 func_000B9BD0: the exit block ends in `b 0xB8570`, a function
    before this one. `_live` only dropped blocks ending in `blr`, so the
    epilogue's `ld r30` was taken as the base register's nearest definition,
    nothing resolved, and the switch lifted to an unresolved indirect call.
    A `b` that goes back past the function's entry, or forward to another
    function's entry, leaves the function the same way a `blr` does. Without
    func_starts there is no entry to compare against, so only `blr` counts.
    """
    mid, table = 0x200A00, 0x11000
    mem = {TOC + 8: mid, mid - 16: table,
           table + 0: 0x100, table + 4: 0x200, table + 8: 0xFFFFFFFF}

    def words(tail_rel):
        return [w_lwz(30, 2, 8), w_cmplwi(0, 3, 2), w_bc(4, 1, 12),
                w_ld(30, 1, 0), w_b(tail_rel),
                w_lwz(11, 30, -16), w_slwi(9, 3, 2), w_lwzx(0, 9, 11),
                w_add(0, 0, 11), w_mtctr(0), w_bctr()]

    # Backward, to a function before this one (the inFAMOUS 2 shape). The `b`
    # is at TEXT_LO + 16, so -0x110 lands at TEXT_LO - 0x100.
    insns = decode_all(words(-0x110))
    tables = discover_jump_tables(insns, lambda a: mem.get(a & 0xFFFFFFFF),
                                  [TOC], TEXT_LO, TEXT_HI, func_starts=[TEXT_LO])
    check("two-level base past a backward tail call", tables.get(insns[-1].addr),
          [0x11100, 0x11200])

    # Forward, to the entry of the next function.
    insns = decode_all(words(0x100 - 16))
    tables = discover_jump_tables(insns, lambda a: mem.get(a & 0xFFFFFFFF),
                                  [TOC], TEXT_LO, TEXT_HI,
                                  func_starts=[TEXT_LO, TEXT_LO + 0x100])
    check("two-level base past a forward tail call", tables.get(insns[-1].addr),
          [0x11100, 0x11200])

    # Control: with no function entries to compare against, a `b` is not
    # trusted to leave the function, and the restore still hides the base.
    insns = decode_all(words(-0x110))
    tables = discover_jump_tables(insns, lambda a: mem.get(a & 0xFFFFFFFF),
                                  [TOC], TEXT_LO, TEXT_HI)
    check("tail call needs func_starts to count as an exit",
          tables.get(insns[-1].addr), None)


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


def test_multi_toc_prefers_targets_near_the_dispatcher():
    """Multi-TOC: the wrong TOC decodes MORE valid targets, but far away.

    MAG (BCUS98110) carries seven TOCs. func_00BB32A0's inline 5-entry table
    sits right after its bctr, loaded through TOC 0x01B94D70; the main TOC's
    slot at the same displacement pointed 12 MB away at data that decoded as 7
    in-text "targets". Most-targets-wins took those, the real cases went
    unlifted, and every call was an unresolved indirect call. Case blocks
    belong to the dispatcher's own function, so targets near the bctr rank
    first; the count only breaks ties.

        lwz r11,16(r2); slwi r9,r3,2; lwzx r0,r9,r11; add r0,r0,r11; mtctr r0; bctr
    """
    text_hi = 0x01000000
    wrong_toc, right_toc = 0x200000, 0x300000
    near, far = 0x11000, 0x800000
    mem = {wrong_toc + 16: far,
           far + 0: 0x10, far + 4: 0x20, far + 8: 0x30, far + 12: 0xFFFFFFFF,
           right_toc + 16: near,
           near + 0: 0x100, near + 4: 0x200, near + 8: 0xFFFFFFFF}
    insns = decode_all([w_cmplwi(0, 3, 2), w_lwz(11, 2, 16), w_slwi(9, 3, 2),
                        w_lwzx(0, 9, 11), w_add(0, 0, 11), w_mtctr(0), w_bctr()])
    tables = discover_jump_tables(insns, lambda a: mem.get(a & 0xFFFFFFFF),
                                  [wrong_toc, right_toc], TEXT_LO, text_hi)
    check("multi-TOC: near targets beat more targets far away",
          tables.get(insns[-1].addr), [0x11100, 0x11200])


def w_ori(ra, rs, ui):     return (24 << 26) | (rs << 21) | (ra << 16) | (ui & 0xFFFF)


def test_bound_compare_beyond_window():
    """The bounds compare sits more than 30 instructions before the bctr.

        cmplwi cr0, r3, 299     <- the bound: 300 entries
        lwz    r11, 16(r2)      <- the table base
        ori    r20, r20, k      <- x31: case setup that does not touch r3/r11
        slwi r9,r3,2; lwzx r0,r9,r11; add r0,r0,r11; mtctr r0; bctr

    Toro Lets Party (BCAS20087) func_000A2EC8, a 292-entry script-opcode
    dispatcher: `cmplwi r29,0x123` is 33 instructions before its bctr. The count
    was read from the 30 instructions before the bctr only; with no compare
    found it fell back to 256, so entries 256+ were never read and their cases
    lifted to unresolved indirect calls. The compare is now also looked for in
    the function-bounded window the base search already uses.
    """
    n, table = 300, 0x11000
    first = table + 4 * n                            # cases right after the table
    mem = {TOC + 16: table}
    for k in range(n):
        mem[table + 4 * k] = first + 4 * k - table   # offsets from the table base
    words = ([w_cmplwi(0, 3, n - 1), w_lwz(11, 2, 16)]
             + [w_ori(20, 20, k) for k in range(31)]
             + [w_slwi(9, 3, 2), w_lwzx(0, 9, 11), w_add(0, 0, 11),
                w_mtctr(0), w_bctr()])
    got = discover(words, mem)
    check("bound compare beyond the 30-instruction window", got,
          [first + 4 * k for k in range(n)])


def main() -> int:
    test_encodings()
    test_absolute_base_is_second_operand()
    test_offset_idiom()
    test_offset_idiom_lwax()
    test_offset_both_operands_r2_loaded()
    test_tie_keeps_first_candidate()
    test_two_level_base_past_early_return()
    test_two_level_base_past_tail_call()
    test_base_before_fallthrough_split()
    test_const_base_no_table()
    test_const_base_needs_constant()
    test_multi_toc_prefers_targets_near_the_dispatcher()
    test_bound_compare_beyond_window()
    test_spilled_base_reused_as_target()
    if FAILS:
        print(f"FAILED: {', '.join(FAILS)}")
        return 1
    print("all jump-table vectors passed")
    return 0


def test_const_base_no_table():
    """The SDK memset's jump into its unrolled tail: no table, a constant base.

        li r8,0x14 ; addis r8,r8,1 ; add r8,r8,r7 ; mtctr r8 ; bctr
        0x10014: stdu r4,8(r3) x3 ; blr

    Every word of the run through its blr is a landing site. Before, nothing
    was found and each short memset lifted to an unresolved indirect call.
    """
    base = TEXT_LO + 0x14
    mem = {base + 4 * k: w_stdu(4, 3, 8) for k in range(3)}
    mem[base + 12] = w_blr()
    got = discover([w_li(8, 0x14), w_addis(8, 8, 1), w_add(8, 8, 7),
                    w_mtctr(8), w_bctr()], mem)
    check("constant-base computed jump", got, [base, base + 4, base + 8, base + 12])


def test_const_base_needs_constant():
    """Same shape, but the base is loaded from memory: not the idiom."""
    base = TEXT_LO + 0x14
    mem = {base: w_blr(), TOC + 8: base}
    got = discover([w_lwz(8, 2, 8), w_add(8, 8, 7), w_mtctr(8), w_bctr()], mem)
    check("non-constant base is not a constant-base jump", got, None)


def test_spilled_base_reused_as_target():
    """The base is TOC-loaded, spilled to the stack, and reloaded at the
    dispatch, which also reuses the base register for the case address:

        lwz r10,16(r2) ; std r10,0x2D0(r1) ; ...
        cmplwi r0,2 ; ld r11,0x2D0(r1) ; slwi r9,r0,2 ; lwzx r0,r9,r11
        add r11,r0,r11 ; mtctr r11 ; bctr

    Both defeated the base search: the nearest definition of r11 was the
    `add` after the table load, and before it the stack reload. (A retail
    title's 32-case dispatcher lifted to an unresolved indirect call.)
    """
    table = 0x11000
    mem = {TOC + 16: table, table + 0: 0x100, table + 4: 0x200, table + 8: 0x300,
           table + 12: 0xFFFFFFFF}
    words = [w_lwz(10, 2, 16), w_std(10, 1, 0x2D0), w_li(5, 0), w_cmplwi(0, 0, 2),
             w_ld(11, 1, 0x2D0), w_slwi(9, 0, 2), w_lwzx(0, 9, 11), w_add(11, 0, 11),
             w_mtctr(11), w_bctr()]
    check("spilled base, base register reused for the target", discover(words, mem),
          [0x11100, 0x11200, 0x11300])
    # A store to a different slot is not the spill: no base, no table.
    words[1] = w_std(10, 1, 0x2D8)
    check("reload of a slot nothing stored is not a base", discover(words, mem), None)


if __name__ == "__main__":
    sys.exit(main())
