#!/usr/bin/env python3
"""The five CBE double-precision compare/test words must not lift to helpers.

A Cell BE SPU does not implement dfceq/dfcmeq/dfcgt/dfcmgt/dftsv: executing one
raises an SPU invalid-instruction error. Console probe (CECHC04 on FW 4.81): each
word run alone in its own thread group did not execute, the group ended with join
cause 4 and the stopped thread's exit status read 0x8001000F (CELL_ESTAT). The
interpreter decodes them as `.word` and halts at the word; the lifter must emit
the same halt at the word's own pc, never a helper call, so a guest that reaches
one stops where the console stops.

Usage:
    python3 tools/test_spu_lifter_invalid_dp.py
"""

import struct
import sys
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))

from spu_disasm import disassemble_spu
from spu_lifter import SPULifter

# RR format: opcd(11) rb(7) ra(7) rt(7). dfceq 0x3C3, dfcmeq 0x3CB, dfcgt 0x2C3,
# dfcmgt 0x2CB, dftsv 0x3BF (tools/spu_disasm.py SPU_RR).
INVALID_OPS = {"dfceq": 0x3C3, "dfcmeq": 0x3CB, "dfcgt": 0x2C3,
               "dfcmgt": 0x2CB, "dftsv": 0x3BF}
HELPERS = {"spu_dfceq", "spu_dfcmeq", "spu_dfcgt", "spu_dfcmgt", "spu_dftsv"}
PC = 0x12340


def lift(word, reloc=False):
    data = struct.pack(">I", word)
    insns = disassemble_spu(data, PC)
    lifter = SPULifter()
    lifter.reloc = reloc
    lifter.func_starts = {PC, PC + 4}
    lifter.lift_function(insns, PC, PC + 4)
    return "\n".join(lifter.functions[0].body_lines)


class InvalidDpStops(unittest.TestCase):
    def test_each_word_halts_and_names_itself(self):
        for mn, op11 in INVALID_OPS.items():
            with self.subTest(op=mn):
                word = (op11 << 21) | (0x33 << 14) | (0x22 << 7) | 0x11
                body = lift(word)
                for h in HELPERS:
                    self.assertNotIn(h, body)
                # The halt: at the word's own pc, through the same spu_halt the
                # lifter emits for a taken halt-assertion, naming the word.
                self.assertIn("SPU_STATUS_STOPPED_BY_HALT", body)
                self.assertIn("spu_halt(ctx); return;", body)
                self.assertIn(f"ctx->pc = 0x{PC:X};", body)
                self.assertIn(f"INVALID-OP {mn}", body)
                self.assertIn(f"0x{word:08X}u", body)

    def test_a_relocated_halt_records_the_placed_pc(self):
        # Under --reloc the code runs at its canonical address plus ctx->reloc_delta, so
        # the halt must record that pc, as every other pc the lifter emits does (_pc).
        body = lift((INVALID_OPS["dfceq"] << 21) | (0x33 << 14) | (0x22 << 7) | 0x11, reloc=True)
        self.assertIn(f"ctx->pc = ((0x{PC:X}u + (uint32_t)ctx->reloc_delta) & 0x3FFFFu);", body)
        self.assertNotIn(f"ctx->pc = 0x{PC:X};", body)

    def test_neighbouring_dp_op_still_lifts_to_its_helper(self):
        # dfa is implemented: it must keep lifting to spu_dfa, not to the halt.
        body = lift((0x2CC << 21) | (3 << 14) | (2 << 7) | 4)
        self.assertIn("spu_dfa(ctx->gpr[2], ctx->gpr[3]);", body)
        self.assertNotIn("SPU_STATUS_STOPPED_BY_HALT", body)


if __name__ == "__main__":
    unittest.main()
