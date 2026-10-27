#!/usr/bin/env python3
"""Callee-save pass: a register set by a carry-form instruction is redefined there.

The pass pairs one `std rN, off(r1)` with one `ld rN, off(r1)` as a callee-save
save/restore when the store comes before rN's first redefinition, and rewrites
the reload to the value rN had at function entry (`_cs_N`). The first
redefinition was looked up only on lines that START with `ctx->gpr[N] = `. The
carry forms (addic, addic., addc, adde, addze, subfc, subfic) are emitted as
`{ uint64_t _a = ...; ctx->gpr[N] = _r; ... }`, so their definition was
invisible: a function that computed a value with one of them, spilled it and
reloaded it later got the stale entry value back (a frozen AI waypoint search in
Killzone 2).

    addic r22,r27,1 ; std r22,0xA8(r1) ; mr r3,r4 ; ld r22,0xA8(r1) ; blr

The spill follows the only definition of r22, so the reload is a real load.

Run: python tools/test_ppu_cs_carry_forms.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ppu_disasm                       # noqa: E402
from ppu_lifter import PPULifter        # noqa: E402

BASE = 0x10000


def w_std(rs, ra, d):  return (62 << 26) | (rs << 21) | (ra << 16) | (d & 0xFFFC)
def w_ld(rt, ra, d):   return (58 << 26) | (rt << 21) | (ra << 16) | (d & 0xFFFC)
def w_mr(ra, rs):      return (31 << 26) | (rs << 21) | (ra << 16) | (rs << 11) | (444 << 1)
def w_addi(rt, ra, s): return (14 << 26) | (rt << 21) | (ra << 16) | (s & 0xFFFF)
def w_addic(rt, ra, s): return (12 << 26) | (rt << 21) | (ra << 16) | (s & 0xFFFF)
def w_blr():           return (19 << 26) | (20 << 21) | (16 << 1)


def lift(define):
    words = [define, w_std(22, 1, 0xA8), w_mr(3, 4), w_ld(22, 1, 0xA8), w_blr()]
    insns = [ppu_disasm.decode(w, BASE + 4 * i) for i, w in enumerate(words)]
    func = PPULifter().lift_function(insns, BASE, BASE + 4 * len(words))
    return "\n".join(func.body_lines)


def test_carry_form_definition_is_seen():
    body = lift(w_addic(22, 27, 1))
    assert "ctx->gpr[22] = _r;" in body, "addic no longer emitted in the compound form this test targets:\n" + body
    assert "_cs_22" not in body, "reload of an addic result became the entry value:\n" + body
    assert "ctx->gpr[22] = vm_read64(ctx->gpr[1] + 0xA8);" in body, body


def test_plain_definition_unchanged():
    # addi starts its line with the assignment: it was seen before, and must be after
    body = lift(w_addi(22, 27, 1))
    assert "_cs_22" not in body, body
    assert "ctx->gpr[22] = vm_read64(ctx->gpr[1] + 0xA8);" in body, body


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print(f"PASS: {name}")
