"""Synthetic strict-lift checks; never reads a game or launches its runtime."""
import contextlib
import io
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import ppu_lifter as lift
from ppu_disasm import decode, disassemble_bytes


class StrictLiftTests(unittest.TestCase):
    def translate(self, raw, strict, address=0x12345678):
        instruction = decode(raw, address)
        lifter = lift.PPULifter(strict_unsupported=strict)
        return instruction, lifter._translate(instruction, lift.LiftedFunction())

    def assert_fatal(self, body, message):
        self.assertIn(message, body)
        self.assertIn('fprintf(stderr, "%s\\n", ', body)
        self.assertIn('fflush(stderr); _Exit(86);', body)
        self.assertTrue(body.startswith('do {'))
        self.assertTrue(body.endswith('} while (0);'))
        self.assertNotIn('if (', body)
        self.assertNotIn('/* TODO:', body)

    def test_unknown_word_emits_exact_site_and_opcode(self):
        instruction, body = self.translate(0, True)
        self.assertEqual(instruction.mnemonic, '.word')
        self.assert_fatal(body, '0x12345678: mnemonic=.word raw=0x00000000')

    def test_recognized_unsupported_rfi_emits_fatal(self):
        # Real disassembler recognizes opcode 19 / XO 50, but lifter has no lowering.
        instruction, body = self.translate(0x4C000064, True, 0x10200)
        self.assertEqual(instruction.mnemonic, 'rfi')
        self.assert_fatal(body, '0x00010200: mnemonic=rfi raw=0x4C000064')

    def test_default_preserves_legacy_todo_exactly(self):
        for raw in (0, 0x4C000064):
            instruction, body = self.translate(raw, False)
            self.assertEqual(body, f'/* TODO: {instruction.mnemonic} {instruction.operands} */;')
        self.assertFalse(lift.PPULifter().strict_unsupported)

    def test_supported_opcode_unchanged(self):
        instruction, strict = self.translate(0x38600007, True)
        _, normal = self.translate(0x38600007, False)
        self.assertEqual(instruction.mnemonic, 'li')
        self.assertEqual(strict, normal)
        self.assertIn('ctx->gpr[3]', strict)
        self.assertNotIn('_Exit', strict)

    def test_header_is_added_only_for_strict_mode(self):
        default = '\n'.join(lift.PPULifter()._preamble_lines())
        strict = '\n'.join(lift.PPULifter(strict_unsupported=True)._preamble_lines())
        self.assertNotIn('#include <stdlib.h>', default)
        self.assertIn('#include <stdlib.h>', strict)

    def test_missing_continuation_is_fatal_only_when_strict(self):
        function = lift.LiftedFunction(name='func_00001000', start_addr=0x1000,
                                      end_addr=0x1004, fallthrough_to=0x1004)
        args = (function, {0x1000: function}, [0x1000], {0x1000: 0})
        with contextlib.redirect_stdout(io.StringIO()):
            default = '\n'.join(lift.PPULifter()._function_def_lines(*args))
            strict = '\n'.join(lift.PPULifter(strict_unsupported=True)._function_def_lines(*args))
        self.assertIn('/* missing continuation 0x00001004: halt */ return;', default)
        self.assertIn('strict missing continuation 0x00001004 from func_00001000', strict)
        self.assertIn('fflush(stderr); _Exit(86);', strict)
        self.assertNotIn('halt */ return;', strict)

    def test_worker_initializer_propagates_and_resets_flag(self):
        previous = dict(lift._WORKER_STATE)
        self.addCleanup(lambda: (lift._WORKER_STATE.clear(), lift._WORKER_STATE.update(previous)))
        blob = struct.pack('>II', 0, 0x4E800020)
        for strict in (True, False):
            lift._worker_init([(0x1000, blob)], True, {}, '', strict_unsupported=strict)
            _, functions, _, _ = lift._worker_lift((0, [(0x1000, 0x1008)]))
            body = '\n'.join(functions[0][3])
            self.assertEqual('_Exit(86)' in body, strict)
            self.assertEqual('/* TODO:' in body, not strict)

    def test_real_multiprocessing_matches_serial_output(self):
        blob = struct.pack('>III', 0, 0x38600007, 0x4E800020)
        bounds = [(0x1000, 0x100c)]
        instructions = disassemble_bytes(blob, 0x1000, True)
        for strict in (False, True):
            with self.subTest(strict=strict):
                serial = lift.PPULifter(strict_unsupported=strict)
                serial.lift_function(instructions, *bounds[0])
                parallel = lift.PPULifter(strict_unsupported=strict)
                with contextlib.redirect_stdout(io.StringIO()):
                    lift._parallel_lift(parallel, bounds, [(0x1000, blob)], True, 2)
                self.assertEqual(serial.emit_source(), parallel.emit_source())


if __name__ == '__main__':
    unittest.main()
