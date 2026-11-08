#!/usr/bin/env python3
"""Synthetic checks for tools/lift_hooks.py. No game files: the lifted
sources are small hand-written files in the lifter's function shape."""
import json
import subprocess
import sys
import tempfile
import types
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
import lift_hooks  # noqa: E402
from lift_hooks import HookError  # noqa: E402

FILE_A = '''#include "ppu_recomp.h"

void func_0000A000(ppu_context* ctx) {
    ctx->gpr[3] = 1;
    ctx->gpr[4] = ctx->gpr[3] + 2;
    return;
}

__attribute__((weak)) void func_0000A100(ppu_context* ctx) {
    ctx->gpr[5] = 7;
}
'''
FILE_B = '''#include "ppu_recomp.h"

void func_0000B000(ppu_context* ctx) {
    ctx->gpr[6] = 9;
}
'''


def body(source, name):
    return lift_hooks.bodies(source, {name})[name][0]


def make_stage(name, tag, pins, dependencies=None, shared=(), seam='    ctx->gpr[3] = 1;\n'):
    patch = (seam, seam + lift_hooks.guarded(f'HOOK_{tag}', f'    hook_{tag.lower()}(ctx);').lstrip('\n'))
    return types.SimpleNamespace(
        NAME=name, INCLUDE=f'#include "hook_{tag.lower()}.h"\n', PINS=pins,
        DEPENDENCIES=dependencies or {}, SHARED=tuple(shared),
        instrument_body=lambda text, fn: lift_hooks.apply_patches(text, [patch], f'{name} {fn}'),
        strip_body=lambda text, fn: lift_hooks.remove_patches(text, [patch]))


def pin(source, name, file='ppu_recomp_000.cpp'):
    return {'sha256': lift_hooks.sha256_text(body(source, name)), 'file': file}


class LiftHookTests(unittest.TestCase):
    def test_hooks_are_guarded_idempotent_and_reversible(self):
        stage = make_stage('trace', 'TRACE', {'func_0000A000': pin(FILE_A, 'func_0000A000')},
                           {'func_0000A100': pin(FILE_A, 'func_0000A100')})
        hooked = lift_hooks.instrument_source(FILE_A, stage)
        self.assertTrue(hooked.startswith('#include "hook_trace.h"\n'))
        self.assertIn('#if HOOK_TRACE\n    hook_trace(ctx);\n#endif\n', hooked)
        self.assertEqual(lift_hooks.instrument_source(hooked, stage), hooked)      # idempotent
        restored = stage.strip_body(hooked.replace(stage.INCLUDE, ''), 'func_0000A000')
        self.assertEqual(restored, FILE_A)                                         # byte for byte
        self.assertEqual(lift_hooks.instrument_source(FILE_B, stage), FILE_B)      # nothing to do

    def test_changed_body_or_dependency_is_refused(self):
        stage = make_stage('trace', 'TRACE', {'func_0000A000': pin(FILE_A, 'func_0000A000')},
                           {'func_0000A100': pin(FILE_A, 'func_0000A100')})
        with self.assertRaisesRegex(HookError, 'does not match its pin'):
            lift_hooks.instrument_source(FILE_A.replace('+ 2', '+ 3'), stage)
        with self.assertRaisesRegex(HookError, 'dependency func_0000A100'):
            lift_hooks.instrument_source(FILE_A.replace('= 7', '= 8'), stage)

    def test_duplicate_function_and_ambiguous_seam_are_refused(self):
        stage = make_stage('trace', 'TRACE', {'func_0000A000': pin(FILE_A, 'func_0000A000')})
        with self.assertRaisesRegex(HookError, 'duplicate lifted function'):
            lift_hooks.instrument_source(FILE_A + '\n' + body(FILE_A, 'func_0000A000') + '\n', stage)
        doubled = FILE_A.replace('    return;\n', '    ctx->gpr[3] = 1;\n    return;\n')
        loose = make_stage('trace', 'TRACE', {'func_0000A000': pin(doubled, 'func_0000A000')})
        with self.assertRaisesRegex(HookError, 'seam is not unique'):
            lift_hooks.instrument_source(doubled, loose)

    def test_malformed_pins_are_refused(self):
        good = pin(FILE_A, 'func_0000A000')
        for bad in ({'sha256': 'x' * 64, 'file': 'ppu_recomp_000.cpp'},
                    {'sha256': good['sha256'], 'file': '../ppu_recomp_000.cpp'},
                    {'sha256': good['sha256']}):
            with self.subTest(pin=bad), self.assertRaisesRegex(HookError, 'malformed pin'):
                lift_hooks.instrument_source(FILE_A, make_stage('s', 'S', {'func_0000A000': bad}))
        both = make_stage('s', 'S', {'func_0000A000': good}, {'func_0000A000': good})
        with self.assertRaisesRegex(HookError, 'both instrumented and a dependency'):
            lift_hooks.instrument_source(FILE_A, both)

    def test_shared_function_carries_both_stages(self):
        name = 'func_0000A000'
        first = make_stage('first', 'ONE', {name: pin(FILE_A, name)}, shared=[name])
        second = make_stage('second', 'TWO', {name: pin(FILE_A, name)}, shared=[name],
                            seam='    ctx->gpr[4] = ctx->gpr[3] + 2;\n')
        once = lift_hooks.instrument_source(FILE_A, first)
        both = lift_hooks.instrument_source(once, second, others=[first])
        self.assertIn('hook_one(ctx);', both)
        self.assertIn('hook_two(ctx);', both)
        self.assertEqual(lift_hooks.instrument_source(both, second, others=[first]), both)
        undeclared = make_stage('third', 'THREE', {name: pin(FILE_A, name)},
                                seam='    return;\n')
        with self.assertRaisesRegex(HookError, 'not declared shared'):
            lift_hooks.instrument_source(both, undeclared, others=[first])

    def test_locate_and_current_pins(self):
        with tempfile.TemporaryDirectory() as temp:
            lift = Path(temp)
            (lift / 'ppu_recomp_000.cpp').write_text(FILE_A)
            (lift / 'ppu_recomp_001.cpp').write_text(FILE_B)
            pins = lift_hooks.current_pins(lift, ['func_0000A000', 'func_0000B000'])
            self.assertEqual(pins['func_0000B000']['file'], 'ppu_recomp_001.cpp')
            self.assertEqual(pins['func_0000A000'], pin(FILE_A, 'func_0000A000'))
            stage = make_stage('s', 'S', {'func_0000A000': pins['func_0000A000']},
                               {'func_0000B000': pins['func_0000B000']})
            self.assertEqual(lift_hooks.locate(lift, stage), ['ppu_recomp_000.cpp', 'ppu_recomp_001.cpp'])
            moved = make_stage('s', 'S', {'func_0000A000': dict(pins['func_0000A000'], file='ppu_recomp_001.cpp')})
            with self.assertRaisesRegex(HookError, 'moved to ppu_recomp_000.cpp'):
                lift_hooks.locate(lift, moved)
            with self.assertRaisesRegex(HookError, 'missing lifted functions func_0000C000'):
                lift_hooks.current_pins(lift, ['func_0000C000'])

    def test_command_line_apply_and_pins(self):
        with tempfile.TemporaryDirectory() as temp:
            work = Path(temp)
            lift = work / 'lift'
            lift.mkdir()
            (lift / 'ppu_recomp_000.cpp').write_text(FILE_A)
            (lift / 'ppu_recomp_001.cpp').write_text(FILE_B)
            digest = lift_hooks.sha256_text(body(FILE_A, 'func_0000A000'))
            (work / 'probe_hook.py').write_text(
                'import lift_hooks, types\n'
                "SEAM = '    ctx->gpr[3] = 1;\\n'\n"
                "PATCH = (SEAM, SEAM + '#if HOOK_PROBE\\n    probe(ctx);\\n#endif\\n')\n"
                "STAGE = types.SimpleNamespace(NAME='probe', INCLUDE='', DEPENDENCIES={},\n"
                f"    PINS={{'func_0000A000': {{'sha256': '{digest}', 'file': 'ppu_recomp_000.cpp'}}}},\n"
                '    instrument_body=lambda b, n: lift_hooks.apply_patches(b, [PATCH], n),\n'
                '    strip_body=lambda b, n: lift_hooks.remove_patches(b, [PATCH]))\n'
                'def instrument_source(source):\n'
                '    return lift_hooks.instrument_source(source, STAGE)\n')
            (work / 'broken_hook.py').write_text('def instrument_source(source):\n    return None\n')
            env = {'PYTHONPATH': f'{TOOLS}{":" if sys.platform != "win32" else ";"}{work}'}
            run = lambda *args: subprocess.run([sys.executable, str(TOOLS / 'lift_hooks.py'), *args],
                                               capture_output=True, text=True, env=env, timeout=60)
            result = run('pins', str(lift), 'func_0000A000')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout)['func_0000A000']['sha256'], digest)

            result = run('apply', str(lift), '--hook', 'broken_hook')
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('returned NoneType', result.stderr)
            self.assertEqual((lift / 'ppu_recomp_000.cpp').read_text(), FILE_A)   # nothing written
            self.assertFalse((lift / 'lift_hooks.json').exists())

            result = run('apply', str(lift), '--hook', 'probe_hook')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('probe(ctx);', (lift / 'ppu_recomp_000.cpp').read_text())
            self.assertEqual((lift / 'ppu_recomp_001.cpp').read_text(), FILE_B)
            record = json.loads((lift / 'lift_hooks.json').read_text())
            self.assertEqual([h['module'] for h in record['hooks']], ['probe_hook'])
            self.assertEqual(list(record['changed']), ['ppu_recomp_000.cpp'])
            self.assertEqual(record['changed']['ppu_recomp_000.cpp']['before_sha256'],
                             lift_hooks.sha256_text(FILE_A))

            hooked = (lift / 'ppu_recomp_000.cpp').read_text()
            result = run('apply', str(lift), '--hook', 'probe_hook')                   # idempotent
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((lift / 'ppu_recomp_000.cpp').read_text(), hooked)

            (lift / 'ppu_recomp_000.cpp').write_text(FILE_A.replace('+ 2', '+ 3'))
            result = run('apply', str(lift), '--hook', 'probe_hook')
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('does not match its pin', result.stderr)


if __name__ == '__main__':
    unittest.main()
