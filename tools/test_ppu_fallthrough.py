#!/usr/bin/env python3
"""Fall-through checks for the PPU lifter: a function whose LAST instruction is
a conditional branch must record a fall-through continuation.

The bug this pins: _last_line_is_terminator treated any line containing
`goto ` as an unconditional terminator, including the conditional forms the
lifter emits for bc/bdnz (`if (...) goto loc_X;`). A function cut at a
conditional branch -- a loop fragment the boundary pass split off, whose exit
path is the next function in address order -- then got no fall-through
trampoline. When the branch was not taken the C function just returned: the
guest function "returned" mid-body, its stack frame still pushed and its
callee-saved registers never restored.

Seen in MAG (BCUS98110): func_0115C850, the member loop of Havok's hkClass
walk, ends `bne loc_0115C850` and falls through to func_0115C8C0. Every loop
exit came back two frames deep with the caller's r26/r31 clobbered, the outer
member loop ran to index 2746, and a parent-chain walk overran a 4-slot buffer
into the allocator's free list. 69 functions in that title had this shape.

Usage:  py -3 tools\\test_ppu_fallthrough.py
"""

import os
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)

import ppu_disasm                                                   # noqa: E402
from ppu_lifter import PPULifter, _last_line_is_terminator          # noqa: E402

BASE = 0x10000


def w_cmpwi(bf, ra, si):  return (11 << 26) | (bf << 23) | (ra << 16) | (si & 0xFFFF)
def w_bc(bo, bi, rel):    return (16 << 26) | (bo << 21) | (bi << 16) | (rel & 0xFFFC)
def w_blr():              return (19 << 26) | (20 << 21) | (16 << 1)
def w_li(rt, si):         return (14 << 26) | (rt << 21) | (si & 0xFFFF)
def w_sc():               return 0x44000002


def lift(words):
    insns = [ppu_disasm.decode(w, BASE + 4 * i) for i, w in enumerate(words)]
    return PPULifter(prefix="").lift_function(insns, BASE, BASE + 4 * len(words))


FAILS = []


def check(name, got, want):
    print(f"{'ok  ' if got == want else 'FAIL'} {name}" + ("" if got == want else f": got {got!r}, want {want!r}"))
    if got != want:
        FAILS.append(name)


def main() -> int:
    # The emitted-line classifier, on the shapes the lifter actually produces.
    check("conditional goto is not a terminator",
          _last_line_is_terminator(["        if ((!((ctx->cr >> 0) & 2))) goto loc_00010000;"]), False)
    check("bdnz is not a terminator",
          _last_line_is_terminator(["        if (((ctx->ctr = (uint32_t)(ctx->ctr - 1)) != 0)) goto loc_00010000;"]), False)
    check("guarded tail call is not a terminator",
          _last_line_is_terminator(["        if (((ctx->cr >> 0) & 2)) { g_trampoline_fn = (void(*)(void*))func_00010100; return; }"]), False)
    check("unconditional goto is a terminator",
          _last_line_is_terminator(["        goto loc_00010000;"]), True)
    check("return is a terminator", _last_line_is_terminator(["        return;"]), True)
    check("syscall is not a terminator", _last_line_is_terminator(["        lv2_syscall(ctx);"]), False)

    # End to end: `loop: cmpwi r3,0; bne loop` falls through to the next address.
    func = lift([w_cmpwi(0, 3, 0), w_bc(4, 2, -4)])
    check("function ending in bne records its fall-through", func.fallthrough_to, BASE + 8)
    # `li r11,141; sc` (sys_timer_usleep) cut at the instruction after `sc`: the
    # syscall returns, so the fragment must continue at the next address.
    func = lift([w_li(11, 141), w_sc()])
    check("function ending in sc records its fall-through", func.fallthrough_to, BASE + 8)
    # Control: ending in blr needs no continuation.
    func = lift([w_cmpwi(0, 3, 0), w_blr()])
    check("function ending in blr has no fall-through", func.fallthrough_to, 0)

    if FAILS:
        print(f"FAILED: {', '.join(FAILS)}")
        return 1
    print("all fall-through checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
