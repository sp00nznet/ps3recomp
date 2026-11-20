#!/usr/bin/env python3
"""cflts/cfltu lift with a per-lane halt at I8 <= 14 and bare at I8 >= 15.

At I8 <= 14 the conversion can reach a region the model does not cover: with
sh = be + 23 - I8, a lane with sh >= 264 (be >= 241 + I8) leaves the saturating
path of a CBE SPU and no rule reproduces what it returns (console probe,
CECHC04 on FW 4.81; see spu__cfl_lane in runtime/spu/spu_helpers.h). cfltu is
modelled for a negative lane (0) and for sh == 264 (0xFFFFFFFF). Which lanes
reach it depends on the data, so lifted code computes the conversion and first
asks spu_cfl_unmodelled_lane(), the interpreter's check, and halts the same way
(op, scale, lane, input word, word, pc, image) when a lane does. I8 15 is the
lowest scale that cannot reach it (max sh = 255 + 23 - 15 = 263) and lifts to
the bare helper call.

The text tests pin the shape; the run test compiles the emitted statement
against the real helpers and executes it on words below, at and above the
boundary.

Usage:
    python3 tools/test_spu_lifter_cfl_scale.py
"""

import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent
sys.path.insert(0, str(TOOLS))

from spu_disasm import disassemble_spu
from spu_lifter import SPULifter

# RI8 format: opcd(10) | i8(8) | ra(7) | rt(7); cflts 0x1D8, cfltu 0x1D9
# (tools/spu_disasm.py, runtime/spu/spu_interp_tables.inc).
CFLTS, CFLTU = 0x1D8, 0x1D9
PC = 0x12340


def word(op10, i8, ra=2, rt=3):
    return (op10 << 22) | ((i8 & 0xFF) << 14) | (ra << 7) | rt


def lift_lines(w, reloc=False):
    data = struct.pack(">I", w)
    insns = disassemble_spu(data, PC)
    lifter = SPULifter()
    lifter.reloc = reloc
    lifter.func_starts = {PC, PC + 4}
    lifter.lift_function(insns, PC, PC + 4)
    return lifter.functions[0].body_lines


def lift(w, reloc=False):
    return "\n".join(lift_lines(w, reloc))


class CflLaneStop(unittest.TestCase):
    def test_small_scales_check_the_lanes_then_compute(self):
        for mn, op10, uns in (("cflts", CFLTS, 0), ("cfltu", CFLTU, 1)):
            for i8 in (0, 14):
                with self.subTest(op=mn, i8=i8):
                    w = word(op10, i8)
                    body = lift(w)
                    check = f"spu_cfl_unmodelled_lane(ctx->gpr[2], {i8}, {uns})"
                    compute = f"ctx->gpr[3] = spu_{mn}(ctx->gpr[2], {i8});"
                    # The conversion is still emitted (a modelled lane runs), after
                    # the check, so the destination is not written when the halt is taken.
                    self.assertIn(check, body)
                    self.assertIn(compute, body)
                    self.assertLess(body.index(check), body.index(compute))
                    # The halt: at the word's own pc, through the same spu_halt as a
                    # taken halt-assertion, naming op, scale, lane, input word, word.
                    self.assertIn("SPU_STATUS_STOPPED_BY_HALT", body)
                    self.assertIn("spu_halt(ctx); return;", body)
                    self.assertIn(f"ctx->pc = 0x{PC:X};", body)
                    self.assertIn(f"INVALID-OP {mn}", body)
                    self.assertIn(f"unmodelled cflts/cfltu lane ({mn} I8={i8} lane=%d in=0x%08X)", body)
                    self.assertIn(f"0x{w:08X}u", body)
                    self.assertIn("ctx->gpr[2]._u32[cfl_l]", body)

    def test_the_check_reads_the_source_before_the_destination_is_written(self):
        # rt == ra: the halt must see the unconverted source, so the check precedes the write.
        body = lift(word(CFLTS, 3, ra=7, rt=7))
        self.assertLess(body.index("spu_cfl_unmodelled_lane(ctx->gpr[7], 3, 0)"),
                        body.index("ctx->gpr[7] = spu_cflts(ctx->gpr[7], 3);"))

    def test_a_relocated_halt_records_the_placed_pc(self):
        # Under --reloc the code runs at its canonical address plus ctx->reloc_delta, so
        # the halt must record that pc, as every other pc the lifter emits does (_pc).
        body = lift(word(CFLTS, 0), reloc=True)
        self.assertIn(f"ctx->pc = ((0x{PC:X}u + (uint32_t)ctx->reloc_delta) & 0x3FFFFu);", body)
        self.assertNotIn(f"ctx->pc = 0x{PC:X};", body)

    def test_modelled_scales_still_lift_to_their_helper(self):
        for mn, op10, helper in (("cflts", CFLTS, "spu_cflts"),
                                 ("cfltu", CFLTU, "spu_cfltu")):
            for i8 in (15, 173):
                with self.subTest(op=mn, i8=i8):
                    body = lift(word(op10, i8))
                    self.assertIn(f"ctx->gpr[3] = {helper}(ctx->gpr[2], {i8});", body)
                    self.assertNotIn("SPU_STATUS_STOPPED_BY_HALT", body)
                    self.assertNotIn("spu_cfl_unmodelled_lane", body)


C_DRIVER = r"""
#include "spu_helpers.h"
#include "spu_ls_watch_fixture.h"
#include <stdio.h>
#include <stdlib.h>
static int g_halted;
void spu_halt(spu_context* ctx) { (void)ctx; g_halted = 1; }
static spu_context g_ctx;
static void lifted(spu_context* ctx) {
    %(stmt)s
}
int main(int argc, char** argv) {
    spu_context* ctx = &g_ctx;
    for (int k = 0; k < 4 && k + 1 < argc; k++) ctx->gpr[2]._u32[k] = (uint32_t)strtoul(argv[k + 1], 0, 16);
    ctx->image_id = 9;
    lifted(ctx);
    printf("halted=%%d status=%%d out=%%08X %%08X %%08X %%08X\n", g_halted,
           (ctx->status & SPU_STATUS_STOPPED_BY_HALT) != 0,
           ctx->gpr[3]._u32[0], ctx->gpr[3]._u32[1], ctx->gpr[3]._u32[2], ctx->gpr[3]._u32[3]);
    return 0;
}
"""


@unittest.skipUnless(shutil.which("cc"), "no C compiler")
class CflLaneStopRuns(unittest.TestCase):
    """The emitted statement against the real helpers, on a host build."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def build(self, w):
        stmt = lift_lines(w)[0]
        src = Path(self.tmp.name) / f"drv_{w:08X}.c"
        src.write_text(C_DRIVER % {"stmt": stmt})
        exe = Path(self.tmp.name) / f"drv_{w:08X}"
        r = subprocess.run(["cc", "-std=gnu17", "-O1", "-I", str(ROOT / "include"), "-I", str(ROOT / "runtime/spu"),
                            "-I", str(ROOT / "tests"), str(src), "-lm", "-o", str(exe)],
                           capture_output=True, text=True, check=False)
        self.assertEqual(r.returncode, 0, r.stderr)
        return exe

    def run_words(self, exe, words):
        r = subprocess.run([str(exe)] + [f"{x:X}" for x in words], capture_output=True, text=True, check=False)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout, r.stderr

    # 0x3F800000 is 1.0 (modelled at every scale), 0x7F800000 / 0xFFFFFFFF exponent-255
    # words of either sign, 0x7F5BAACA / 0xFF66344D exponent 254.
    def test_a_lane_in_the_unmodelled_region_halts_and_names_itself(self):
        for mn, op10, i8, words, lane in (
                ("cflts", CFLTS, 14, (0x3F800000, 0x7F800000, 0x3F800000, 0xFFFFFFFF), 1),
                ("cflts", CFLTS, 0, (0x3F800000, 0x7878EB45, 0x3F800000, 0xFFABDE3D), 3),
                ("cfltu", CFLTU, 13, (0x3F800000, 0x7F800000, 0x7F5BAACA, 0xFFFFFFFF), 1),
                ("cfltu", CFLTU, 0, (0x79607710, 0x3F800000, 0x3F800000, 0x3F800000), 0)):
            with self.subTest(op=mn, i8=i8):
                w = word(op10, i8)
                out, err = self.run_words(self.build(w), words)
                self.assertIn("halted=1 status=1 out=00000000 00000000 00000000 00000000", out)   # no result written
                self.assertIn(f"unmodelled cflts/cfltu lane ({mn} I8={i8} lane={lane} in=0x{words[lane]:08X}) "
                              f"(0x{w:08X}) at LS 0x{PC:05X} img=9", err)

    def test_modelled_lanes_run_below_the_boundary(self):
        for mn, op10, i8, words, want in (
                # sh 263: both ops model it, sh 264 for cfltu (positive 0xFFFFFFFF, negative 0)
                ("cflts", CFLTS, 14, (0x3F800000, 0x3F800000, 0x7F5BAACA, 0xFF66344D),
                 (0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF, 0x80000000)),
                ("cfltu", CFLTU, 14, (0x3F800000, 0x7F800000, 0x7F5BAACA, 0xFFFFFFFF),
                 (0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0)),
                ("cfltu", CFLTU, 13, (0x3F800000, 0xFFFFFFFF, 0x7F5BAACA, 0xFF66344D),
                 (0xFFFFFFFF, 0, 0xFFFFFFFF, 0)),
                ("cflts", CFLTS, 0, (0x7878EB45, 0xF84DEADE, 0x3F800000, 0x00000000),
                 (0x7FFFFFFF, 0x80000000, 0x7FFFFFFF, 0x7FFFFFFF))):
            with self.subTest(op=mn, i8=i8):
                out, err = self.run_words(self.build(word(op10, i8)), words)
                self.assertIn("halted=0 status=0 out=" + " ".join(f"{x:08X}" for x in want), out)
                self.assertEqual(err, "")


if __name__ == "__main__":
    unittest.main()
