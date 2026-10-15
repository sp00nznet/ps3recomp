"""Synthetic emitted-C tests, with real runtime headers and strict recorder.

Run: python -B tests/test_spu_strict_lifter.py
Optional SPU_LIFTER_TEST_CC selects a compiler executable (clang or GCC style).
No game image, generated title code, GPU or SPU interpreter is used.
"""
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from spu_disasm import spu_decode
from spu_lifter import SPULifter


def il(register, value):
    return 0x40800000 | ((value & 0xffff) << 7) | register


class StrictSpuLifterTests(unittest.TestCase):
    def generate(self, directory, words, strict, bounds=None):
        directory = Path(directory)
        image = directory / 'synthetic.bin'
        image.write_bytes(b''.join(struct.pack('>I', word) for word in words))
        command = [sys.executable, '-B', str(ROOT/'tools/spu_lifter.py'), str(image),
                   '--output', str(directory), '--header-name', 'generated.h',
                   '--source-name', 'generated.c']
        if strict:
            command.append('--strict-unsupported')
        if bounds is not None:
            manifest = directory / 'functions.json'
            manifest.write_text(json.dumps(bounds))
            command += ['--functions', str(manifest)]
        result = subprocess.run(command, text=True, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return (directory / 'generated.c').read_text()

    def compile_and_run(self, words, strict, expected_check='ctx->gpr[3]._u32[0] == 7',
                        strict_environment=True, bounds=None):
        compiler = os.environ.get('SPU_LIFTER_TEST_CC') or shutil.which('clang') or shutil.which('gcc')
        if not compiler:
            self.skipTest('No C compiler found; emitted execution remains unverified')
        # Two levels below the repository root preserves the emitter's real
        # ../../runtime and ../../include paths without rewriting generated C.
        with tempfile.TemporaryDirectory(prefix='spu_strict_', dir=ROOT/'tests') as directory:
            folder = Path(directory)
            self.generate(folder, words, strict, bounds=bounds)
            harness = folder/'main.c'
            harness.write_text('''#include "generated.h"
#include <stdio.h>
#include <stdlib.h>
void spu_register_function(uint32_t address, void (*fn)(spu_context*)) { (void)address; (void)fn; }
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);
void spu_indirect_branch(spu_context* ctx) { (void)ctx; _Exit(93); }
void spu_stop(spu_context* ctx) { (void)ctx; }
void spu_halt(spu_context* ctx) { ctx->status = SPU_STATUS_STOPPED_BY_HALT; }
int main(void) {
    spu_context* ctx = (spu_context*)calloc(1, sizeof *ctx);
    if (!ctx) return 90;
    for (int i=0;i<4;i++) ctx->gpr[79]._u32[i] = 0xFFFFFFFFu;
    spu_func_00000000(ctx);
    unsigned transfers = 0;
    while (g_spu_trampoline_fn && transfers++ < 16) {
        void (*next)(spu_context*) = g_spu_trampoline_fn;
        g_spu_trampoline_fn = 0;
        next(ctx);
    }
    if (g_spu_trampoline_fn) return 94;
    int okay = (''' + expected_check + ''');
    puts("AFTER_GENERATED_CALL");
    free(ctx);
    return okay ? 0 : 91;
}
''')
            executable = folder/('probe.exe' if os.name == 'nt' else 'probe')
            command = [compiler, '-std=c17', '-O0', str(folder/'generated.c'),
                       str(harness), '-o', str(executable)]
            if os.name != 'nt':
                command += ['-D_GNU_SOURCE', '-lm']
            build = subprocess.run(command, text=True, capture_output=True, timeout=60)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            environment = os.environ.copy()
            environment['PS3_STRICT_BOOT'] = '1' if strict_environment else '0'
            return subprocess.run([str(executable)], env=environment, text=True,
                                  capture_output=True, timeout=10)

    def assert_fatal(self, result, reason, pc):
        self.assertEqual(result.returncode, 86, result.stdout + result.stderr)
        self.assertIn('FATAL category=spu-lift-unsupported', result.stderr)
        self.assertIn(reason, result.stderr)
        self.assertIn(f'pc=0x{pc:05X}', result.stderr)
        self.assertNotIn('AFTER_GENERATED_CALL', result.stdout)

    def test_cli_opt_in_preserves_default_generation(self):
        with tempfile.TemporaryDirectory(prefix='spu_strict_', dir=ROOT/'tests') as directory:
            legacy = self.generate(directory, [0x01000000, il(3,7), 0], False)
            self.assertIn('spu_unsupported(', legacy)
            self.assertNotIn('spu_lift_strict_failure', legacy)
            strict = self.generate(directory, [0x01000000, il(3,7), 0], True)
            self.assertIn('ps3_strict_boot_guard(', strict)
            self.assertNotIn('spu_unsupported(', strict)

    def test_call_next_is_link_and_internal_transfer(self):
        lifter = SPULifter(strict_unsupported=True)
        # brsl r79, PC+4 is a PIC getter, not a nested host call.
        instruction = spu_decode(0x33000000 | (1 << 7) | 79, 4)
        function = lifter.lift_function([spu_decode(il(3,7),0), instruction,
                                        spu_decode(0,8)], 0, 12)
        self.assertFalse(lifter.call_targets)
        self.assertNotIn('host_depth++', '\n'.join(function.body_lines))
        self.assertEqual(lifter.unsupported, {})

    def test_supported_emitted_code_executes(self):
        result = self.compile_and_run([il(3,7),0], True)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('AFTER_GENERATED_CALL',result.stdout)

    def test_call_next_executes_link_lane_contract(self):
        result = self.compile_and_run([il(3,7),0x33000000 | (1 << 7) | 79,0], True,
            'ctx->gpr[3]._u32[0] == 7 && ctx->gpr[79]._u32[0] == 8 && '
            'ctx->gpr[79]._u32[1] == 0 && ctx->gpr[79]._u32[2] == 0 && ctx->gpr[79]._u32[3] == 0')
        self.assertEqual(result.returncode,0,result.stderr)

    def test_unknown_emitted_site_uses_real_strict_recorder(self):
        self.assertEqual(spu_decode(0x01000000).mnemonic,'.word')
        result = self.compile_and_run([0x01000000,il(3,7),0],True)
        self.assert_fatal(result,'unsupported-instruction:.word',0)
        self.assertIn('[strict-boot]',result.stderr)

    def test_generation_opt_in_is_binding_without_environment(self):
        result = self.compile_and_run([0x01000000,il(3,7),0],True,strict_environment=False)
        self.assert_fatal(result,'unsupported-instruction:.word',0)

    def test_nonstrict_unknown_retains_warning_and_continuation(self):
        result = self.compile_and_run([0x01000000,il(3,7),0],False)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('UNSUPPORTED',result.stderr)
        self.assertIn('AFTER_GENERATED_CALL',result.stdout)

    def test_self_loop_cannot_be_reported_as_completed(self):
        self.assert_fatal(self.compile_and_run([0x32000000],True),
                          'self-loop-halt-substitution',0)

    def test_past_end_is_fatal(self):
        self.assert_fatal(self.compile_and_run([il(3,7)],True),
                          'missing-continuation',4)

    def test_missing_native_target_is_fatal(self):
        self.assert_fatal(self.compile_and_run([0x32000000 | (16 << 7)],True),
                          'missing-native-target',0x40)

    def test_strict_fscrwr_does_not_silently_ignore_fp_control(self):
        instruction = 0x3BA << 21
        self.assertEqual(spu_decode(instruction).mnemonic,'fscrwr')
        self.assert_fatal(self.compile_and_run([instruction,il(3,7),0],True),
                          'unsupported-instruction:fscrwr',0)

    def test_strict_mtspr_does_not_silently_ignore_spr(self):
        instruction = 0x10C << 21
        self.assertEqual(spu_decode(instruction).mnemonic,'mtspr')
        self.assert_fatal(self.compile_and_run([instruction,il(3,7),0],True),
                          'unsupported-instruction:mtspr',0)

    def test_nonstrict_placeholders_preserve_legacy_behavior(self):
        result = self.compile_and_run([0x3BA << 21,0x10C << 21,il(3,7),0],False)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('AFTER_GENERATED_CALL',result.stdout)

    def test_conditional_not_taken_cannot_silently_fall_past_end(self):
        branch_to_zero_if_r3_zero = 0x20000000 | (0xFFFF << 7) | 3
        self.assertEqual(spu_decode(branch_to_zero_if_r3_zero,4).mnemonic,'brz')
        self.assert_fatal(self.compile_and_run([il(3,7),branch_to_zero_if_r3_zero],True),
                          'missing-continuation',8)

    def test_strict_escaping_branch_is_not_a_dead_tail_self_loop(self):
        # Reachable first branch escapes to a valid generated function. The
        # later br4 is unreachable; it must not cause a whole-function rewrite.
        program = [0x32000000 | (3 << 7),0x4020007F,
                   0x32000000 | (0xFFFF << 7),il(3,7),0]
        bounds = [{'start':0,'end':12},{'start':12,'end':20}]
        result = self.compile_and_run(program,True,
            expected_check='ctx->gpr[3]._u32[0] == 7 && ctx->status == SPU_STATUS_STOPPED_BY_STOP',
            bounds=bounds)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('AFTER_GENERATED_CALL',result.stdout)
        self.assertNotIn('FATAL',result.stderr)


if __name__ == '__main__':
    unittest.main()
