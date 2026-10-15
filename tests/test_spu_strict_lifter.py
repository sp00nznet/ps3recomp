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
    def generate(self, directory, words, strict, bounds=None, base=0):
        directory = Path(directory)
        image = directory / 'synthetic.bin'
        image.write_bytes(b''.join(struct.pack('>I', word) for word in words))
        command = [sys.executable, '-B', str(ROOT/'tools/spu_lifter.py'), str(image),
                   '--output', str(directory), '--header-name', 'generated.h',
                   '--source-name', 'generated.c']
        command += ['--base', hex(base)]
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

    def position_run(self, mapping, bounds, entry, check, setup='', base=0):
        """Compile unchanged emitted C with bounded synthetic dispatch/returns."""
        compiler = os.environ.get('SPU_LIFTER_TEST_CC') or shutil.which('clang') or shutil.which('gcc')
        if not compiler:
            self.skipTest('No C compiler found; emitted execution remains unverified')
        with tempfile.TemporaryDirectory(prefix='spu_position_',dir=ROOT/'tests') as directory:
            folder=Path(directory)
            words=[0]*((max(mapping)-base)//4+1)
            for address,word in mapping.items(): words[(address-base)//4]=word
            self.generate(folder,words,True,bounds=bounds,base=base)
            dispatch='\n'.join(f'case {b["start"]}u: spu_func_{b["start"]:08X}(ctx); return;' for b in bounds)
            harness='''#include "generated.h"
#include <stdlib.h>
#include <stdio.h>
static unsigned indirect_calls,drain_calls;
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);
void spu_register_function(uint32_t a,void (*f)(spu_context*)) {(void)a;(void)f;_Exit(96);}
void spu_stop(spu_context* ctx) {(void)ctx;}
void spu_halt(spu_context* ctx) {(void)ctx;_Exit(95);}
void spu_img_restore(spu_context* ctx,int32_t id) {ctx->image_id=(uint32_t)id;}
void spu_indirect_branch(spu_context* ctx) {
    ++indirect_calls;
    switch ((uint32_t)ctx->pc & 0x3FFFCu) {
@DISPATCH@
    default:_Exit(93);
    }
}
void spu_drain_call(spu_context* ctx,uint32_t expected) {
    ++drain_calls;
    for (unsigned n=0;n<16;++n) {
        if (((uint32_t)ctx->pc & 0x3FFFCu)==expected &&
            (!g_spu_trampoline_fn || g_spu_trampoline_fn==spu_indirect_branch)) {
            g_spu_trampoline_fn=NULL;return;
        }
        if (!g_spu_trampoline_fn) {spu_indirect_branch(ctx);continue;}
        void (*next)(spu_context*)=g_spu_trampoline_fn;g_spu_trampoline_fn=NULL;next(ctx);
    }
    _Exit(94);
}
int main(void) {
    spu_context* ctx=(spu_context*)calloc(1,sizeof *ctx);if(!ctx)return90;
    ctx->ls=(uint8_t*)calloc(1,0x40000);if(!ctx->ls)return90;
    for(int i=0;i<4;++i)ctx->gpr[5]._u32[i]=0xFFFFFFFFu;
    @SETUP@
    spu_func_@ENTRY@(ctx);
    unsigned n=0;
    while(g_spu_trampoline_fn && n++<16) {
        void (*next)(spu_context*)=g_spu_trampoline_fn;g_spu_trampoline_fn=NULL;next(ctx);
    }
    int okay=!g_spu_trampoline_fn && (@CHECK@);
    free(ctx->ls);free(ctx);puts("AFTER_POSITION_CALL");return okay?0:91;
}
'''.replace('return90','return 90').replace('@DISPATCH@',dispatch)
            harness=harness.replace('@SETUP@',setup).replace('@ENTRY@',f'{entry:08X}').replace('@CHECK@',check)
            (folder/'main.c').write_text(harness)
            executable=folder/('probe.exe' if os.name=='nt' else 'probe')
            command=[compiler,'-std=c17','-O0',str(folder/'generated.c'),str(folder/'main.c'),'-o',str(executable)]
            if os.name!='nt': command+=['-D_GNU_SOURCE','-lm']
            result=subprocess.run(command,text=True,capture_output=True,timeout=60)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            environment=os.environ.copy();environment['PS3_STRICT_BOOT']='1'
            return subprocess.run([str(executable)],env=environment,text=True,capture_output=True,timeout=10)

    def test_bisl_alias_preserves_old_masked_target(self):
        mapping={0x8000:(0x1A9<<21)|(5<<7)|5,0x8004:il(3,7),0x8008:0,
                 0x8020:il(9,11),0x8024:(0x1A8<<21)|(5<<7)}
        result=self.position_run(mapping,[{'start':0x8000,'end':0x800C},{'start':0x8020,'end':0x8028}],0x8000,
            'ctx->gpr[3]._u32[0]==7 && ctx->gpr[9]._u32[0]==11 && ctx->gpr[5]._u32[0]==0x8004 && '
            'ctx->gpr[5]._u32[1]==0 && ctx->gpr[5]._u32[2]==0 && ctx->gpr[5]._u32[3]==0 && '
            'indirect_calls==1 && drain_calls==1 && ctx->host_depth==0',
            setup='ctx->gpr[5]._u32[0]=0x8023;',base=0x8000)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_boundary_call_next_wraps_without_host_call(self):
        for instruction in (0x33000000|(1<<7)|5,0x31000000|5,
                            (0x1A9<<21)|(5<<7)|5|0x40000):
            with self.subTest(instruction=hex(instruction)):
                result=self.position_run({0:0x1C000000|(1<<14)|(3<<7)|3,4:0,0x3FFFC:instruction},
                    [{'start':0,'end':8},{'start':0x3FFFC,'end':0x40000}],0x3FFFC,
                    'ctx->gpr[3]._u32[0]==1 && ctx->gpr[5]._u32[0]==0 && ctx->gpr[5]._u32[1]==0 && '
                    'ctx->gpr[5]._u32[2]==0 && ctx->gpr[5]._u32[3]==0 && drain_calls==0 && indirect_calls==0 && ctx->host_depth==0 && '
                    f'ctx->int_enable=={1 if instruction & 0x40000 else 0}',
                    setup='ctx->gpr[5]._u32[0]=3;')
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_bisl_internal_call_next_has_no_host_frame(self):
        # The continuation is inside the only generated function, so dispatch
        # to a separate function entry would be wrong even without wrapping.
        for control,initial,expected in ((0x40000,0,1),(0x80000,1,0)):
            with self.subTest(control=hex(control)):
                result=self.position_run({0x8000:(0x1A9<<21)|(5<<7)|5|control,
                    0x8004:0x1C000000|(1<<14)|(3<<7)|3,0x8008:0},
                    [{'start':0x8000,'end':0x800C}],0x8000,
                    'ctx->gpr[3]._u32[0]==1 && ctx->gpr[5]._u32[0]==0x8004 && '
                    'ctx->gpr[5]._u32[1]==0 && ctx->gpr[5]._u32[2]==0 && ctx->gpr[5]._u32[3]==0 && '
                    f'ctx->int_enable=={expected} && drain_calls==0 && indirect_calls==0 && ctx->host_depth==0 && '
                    'ctx->status==SPU_STATUS_STOPPED_BY_STOP',
                    setup=f'ctx->gpr[5]._u32[0]=0x8007;ctx->int_enable={initial};',base=0x8000)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_boundary_real_calls_return_through_wrapped_continuation(self):
        for instruction,indirect in ((0x33000000|(65<<7)|5,0),
                                     (0x31000000|(0x40<<7)|5,0),
                                     ((0x1A9<<21)|(5<<7)|5,1)):
            with self.subTest(instruction=hex(instruction)):
                result=self.position_run({0:0x1C000000|(1<<14)|(3<<7)|3,4:0,0x100:il(9,11),0x104:(0x1A8<<21)|(5<<7),0x3FFFC:instruction},
                    [{'start':0,'end':8},{'start':0x100,'end':0x108},{'start':0x3FFFC,'end':0x40000}],0x3FFFC,
                    'ctx->gpr[3]._u32[0]==1 && ctx->gpr[9]._u32[0]==11 && ctx->gpr[5]._u32[0]==0 && '
                    'ctx->gpr[5]._u32[1]==0 && ctx->gpr[5]._u32[2]==0 && ctx->gpr[5]._u32[3]==0 && '
                    f'drain_calls==1 && indirect_calls=={indirect} && ctx->host_depth==0 && ctx->status==SPU_STATUS_STOPPED_BY_STOP',
                    setup='ctx->gpr[5]._u32[0]=0x103;')
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_boundary_fallthrough_and_missing_continuation_wrap(self):
        result=self.position_run({0:il(3,7),4:0,0x3FFFC:il(9,11)},
            [{'start':0,'end':8},{'start':0x3FFFC,'end':0x40000}],0x3FFFC,
            'ctx->gpr[3]._u32[0]==7 && ctx->gpr[9]._u32[0]==11 && ctx->status==SPU_STATUS_STOPPED_BY_STOP')
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        result=self.position_run({0x3FFFC:il(3,7)},[{'start':0x3FFFC,'end':0x40000}],0x3FFFC,'0',base=0x3FFFC)
        self.assert_fatal(result,'missing-continuation',0)

    def test_auto_elf_base_override_rejects_before_output(self):
        raw=bytearray(0x108);raw[:16]=b'\x7fELF\x01\x02\x01'+bytes(9)
        struct.pack_into('>HHIIIIIHHHHHH',raw,16,2,23,1,0x1000,52,0,0,52,32,1,40,0,0)
        struct.pack_into('>IIIIIIII',raw,52,1,0x100,0x1000,0x1000,8,8,5,16)
        struct.pack_into('>II',raw,0x100,il(3,7),0)
        with tempfile.TemporaryDirectory(prefix='spu_auto_',dir=ROOT/'tests') as directory:
            folder=Path(directory);image=folder/'synthetic.elf';image.write_bytes(raw)
            for override in ('0x8000','0'):
                output=folder/('rejected_'+override)
                result=subprocess.run([sys.executable,'-B',str(ROOT/'tools/spu_lifter.py'),
                    '--auto-functions',str(image),'--base',override,'--output',str(output)],capture_output=True,text=True,timeout=30)
                self.assertNotEqual(result.returncode,0)
                self.assertIn('relocated discovery is unsupported',result.stderr)
                self.assertFalse(output.exists())
            for label,options in (('default',[]),('same',['--base','0x1000'])):
                output=folder/label
                result=subprocess.run([sys.executable,'-B',str(ROOT/'tools/spu_lifter.py'),
                    '--auto-functions',str(image),'--output',str(output),*options],capture_output=True,text=True,timeout=30)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                self.assertIn('spu_func_00001000',(output/'spu_recomp.h').read_text())


if __name__ == '__main__':
    unittest.main()
