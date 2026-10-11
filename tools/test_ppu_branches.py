#!/usr/bin/env python3
"""Branch-dispatch emission checks for the PPU lifter.

Complements test_ppu_lift.py: that suite compiles emitted statements and runs
them against register inputs, which can't cover branch dispatch (it needs the
runtime's ps3_indirect_call + trampoline). These assert the emission SHAPE --
call vs return -- which is where the class of bug below lives.

The bug this pins: blrl (branch to LR *with link*) is an indirect CALL, the
LR-based twin of `mtctr;bctrl`, emitted for function-descriptor calls as
`lwz r12,0(rN); mtlr r12; lwz r2,4(rN); blrl`. It was lumped with blr (return)
and emitted a bare `return;`, which dropped the call AND skipped the frame
epilogue -> r1 leaked the frame size on every such call.

Usage:  py -3 tools\\test_ppu_branches.py
"""

import os
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)

from ppu_disasm import Instruction                  # noqa: E402
from ppu_lifter import PPULifter, LiftedFunction, _last_line_is_terminator  # noqa: E402


def emit(mnemonic: str, operands: str = "") -> str:
    lifter = PPULifter(prefix="")
    func = LiftedFunction(name="f", start_addr=0x1000, end_addr=0x2000)
    return lifter._translate_op(Instruction(0x1000, 0, mnemonic, operands), func)


def main() -> int:
    blr, blrl = emit("blr"), emit("blrl")
    bctrl, bnelrl = emit("bctrl"), emit("bnelrl", "cr7")

    # blr is the only real return of the four.
    assert blr == "return;", blr

    # blrl: dispatch through LR, then CONTINUE (link = call, so no return).
    assert "ps3_indirect_call" in blrl, blrl
    assert "ctx->lr" in blrl, blrl
    assert "return;" not in blrl, f"blrl must not return -- link = call: {blrl}"

    # b<cond>lrl: the conditional LR twin of b<cond>ctrl.
    assert "ps3_indirect_call" in bnelrl, bnelrl
    assert "ctx->lr" in bnelrl, bnelrl
    # LK=1 writes LR = CIA + 4 whether or not the branch is taken (PowerISA
    # Book I 2.4), so the LR update precedes the condition; the call is guarded.
    assert "if (" in bnelrl and bnelrl.index("ctx->lr = ") < bnelrl.index("if ("), bnelrl

    # The CTR-based call it mirrors, unchanged.
    # bctrl links too (LR = CIA + 4) before calling through CTR.
    assert bctrl == "ctx->lr = 0x00001004; ps3_indirect_call(ctx); DRAIN_TRAMPOLINE(ctx);", bctrl

    # A chunk that ends on a conditional branch falls through when it is not
    # taken, so it needs the fall-through tail; an unconditional one does not.
    bdnz = emit("bdnz", "0x1000")
    assert bdnz.startswith("if ("), bdnz
    assert not _last_line_is_terminator([f"    {bdnz}"]), bdnz
    assert _last_line_is_terminator(["    goto loc_00001000;"])
    assert _last_line_is_terminator(["    return;"])

    print("ok: blr returns; blrl/bnelrl dispatch via LR and continue; "
          "conditional branches fall through")
    return 0


if __name__ == "__main__":
    sys.exit(main())
