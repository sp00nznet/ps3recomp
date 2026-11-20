#!/usr/bin/env python3
"""Synthetic tests for tools/ppu_function_splice.py.

The lifts are written by the real lifter output stage (PPULifter.write_source_files
and emit_header) from synthetic LiftedFunction bodies, in both chunking modes. A
"lifter fix" changes one body and adds one function; the base lift carries a hand
edit elsewhere that the splice must keep. The spliced lift is compiled with
-fsyntax-only against the runtime headers when a C++ compiler is available.

Run: python tools/test_ppu_function_splice.py
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent
sys.path.insert(0, str(TOOLS))
from ppu_lifter import LiftedFunction, PPULifter  # noqa: E402
import ppu_function_splice as splice_tool  # noqa: E402

ADDRS = [0x10000 + 0x40 * i for i in range(40)]
NEW = 0x20000


def name(addr):
    return f'func_{addr:08X}'


def bodies(fixed=False, todo=False):
    out = {a: [f'ctx->gpr[3] = {i};'] for i, a in enumerate(ADDRS)}
    out[ADDRS[3]] = ['ctx->gpr[3] = ctx->gpr[4] + 1;']          # the bug
    if todo:
        out[ADDRS[3]].append('/* TODO: frob r3 */;')
    if fixed:
        out[ADDRS[3]] = ['ctx->gpr[3] = ctx->gpr[4] + 2;']      # the fix
        if todo:
            out[ADDRS[3]].append('/* TODO: frob r3 */;')
        out[NEW] = ['ctx->gpr[3] = 42;']                        # newly emitted
    return out


def write_lift(directory, functions, chunks):
    lifter = PPULifter()
    for addr in sorted(functions):
        lifter.functions.append(LiftedFunction(name=name(addr), start_addr=addr, end_addr=addr + 0x40,
                                               body_lines=functions[addr] + ['return;']))
    directory.mkdir(parents=True)
    if chunks:
        lifter.write_source_files(str(directory), base='ppu_recomp', chunks=chunks)
    else:
        lifter.write_source_files(str(directory), base='ppu_recomp', max_lines=60)
    (directory / 'ppu_recomp.h').write_text(lifter.emit_header())


def files(directory):
    return {p.name: p.read_text() for p in sorted(Path(directory).iterdir()) if p.is_file()}


class SpliceTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)

    def lifts(self, chunks=0, todo=False, guard=''):
        before, after, base = self.dir / 'before', self.dir / 'after', self.dir / 'base'
        write_lift(before, bodies(todo=todo), chunks)
        write_lift(after, bodies(fixed=True, todo=todo), chunks)
        shutil.copytree(before, base)
        # A reviewed hand edit in a function the fix does not touch.
        edited = splice_tool.functions(base, 'ppu_recomp')[name(ADDRS[7])][0]
        text = (base / edited).read_text()
        (base / edited).write_text(text.replace('ctx->gpr[3] = 7;', 'ctx->gpr[3] = 7; /* reviewed edit */'))
        if guard:
            for path in base.glob('ppu_recomp_*.cpp'):
                path.write_text(path.read_text().replace('/* TODO: frob r3 */;', '/* TODO: frob r3 */;' + guard))
        return base, before, after

    def run_splice(self, base, before, after, **options):
        return splice_tool.splice(base, before, after, self.dir / 'out', 'rev-before', 'rev-after',
                                  'test fix', **options)

    def check_spliced(self, chunks):
        base, before, after = self.lifts(chunks)
        result = self.run_splice(base, before, after)
        self.assertEqual(result['changed'], [name(ADDRS[3])])
        self.assertEqual(result['added'], [name(NEW)])
        out = self.dir / 'out'
        spliced = splice_tool.functions(out, 'ppu_recomp')
        self.assertIn('+ 2;', spliced[name(ADDRS[3])][1])
        self.assertIn('/* reviewed edit */', spliced[name(ADDRS[7])][1])
        self.assertEqual(spliced[name(NEW)][0], result['new_translation_unit'])
        # Untouched files stay byte-identical; only the changed body's file,
        # the table file, the header and the new unit differ.
        old, new = files(base), files(out)
        _, table_file = splice_tool.table(base, 'ppu_recomp')
        differing = {f for f in old if old[f] != new.get(f)}
        self.assertEqual(differing, {spliced[name(ADDRS[3])][0], table_file, 'ppu_recomp.h'})
        self.assertEqual(set(new) - set(old), {result['new_translation_unit'], 'function_splices.json'})
        self.assertIn(f'void {name(NEW)}(ppu_context* ctx);', new['ppu_recomp.h'])
        rows, _ = splice_tool.table(out, 'ppu_recomp')
        self.assertEqual(set(rows), {name(a) for a in ADDRS + [NEW]})
        self.assertIn(f'function_table_count = {len(ADDRS) + 1};', new[table_file])
        record = json.loads(new['function_splices.json'])
        self.assertEqual(len(record), 1)
        self.assertEqual((record[0]['before_revision'], record[0]['after_revision']), ('rev-before', 'rev-after'))
        self.assertEqual(record[0]['lifter_footprint'], {'changed': [name(ADDRS[3])], 'added': [name(NEW)]})
        self.assertEqual(set(record[0]['base_sha256']), set(old))
        if len(list(base.glob('ppu_recomp_*.cpp'))) < 3:
            self.fail('the synthetic lift should span several translation units')
        self.compile(out)
        # A second splice appends to the record.
        second = self.dir / 'out2'
        os.rename(out, self.dir / 'stage')
        splice_tool.splice(self.dir / 'stage', after, after, second, 'a', 'a', 'no-op')
        self.assertEqual(len(json.loads((second / 'function_splices.json').read_text())), 2)

    def test_line_count_chunks(self):
        self.check_spliced(chunks=0)

    def test_address_chunks_with_table_file(self):
        self.check_spliced(chunks=3)

    def test_refuses_when_the_base_differs_from_before(self):
        base, before, after = self.lifts()
        unit = splice_tool.functions(base, 'ppu_recomp')[name(ADDRS[3])][0]
        text = (base / unit).read_text()
        (base / unit).write_text(text.replace('+ 1;', '+ 1; /* transformed */'))
        with self.assertRaisesRegex(splice_tool.SpliceError, 'differs from the --before lift'):
            self.run_splice(base, before, after)
        self.assertFalse((self.dir / 'out').exists())

    def test_refuses_existing_output_and_removed_functions(self):
        base, before, after = self.lifts()
        (self.dir / 'out').mkdir()
        with self.assertRaisesRegex(splice_tool.SpliceError, 'must not exist'):
            self.run_splice(base, before, after)
        with self.assertRaisesRegex(splice_tool.SpliceError, 'removes functions'):
            splice_tool.splice(base, after, before, self.dir / 'other', 'a', 'b', 'reverse')

    def test_only_limits_the_splice(self):
        base, before, after = self.lifts()
        result = self.run_splice(base, before, after, only=[name(ADDRS[3])])
        self.assertEqual((result['changed'], result['added'], result['new_translation_unit']),
                         ([name(ADDRS[3])], [], None))
        with self.assertRaisesRegex(splice_tool.SpliceError, 'does not change'):
            splice_tool.splice(base, before, after, self.dir / 'x', 'a', 'b', 'r', only=[name(ADDRS[1])])

    def test_todo_guard_is_matched_and_applied(self):
        guard = ' abort();'
        base, before, after = self.lifts(todo=True, guard=guard)
        with self.assertRaisesRegex(splice_tool.SpliceError, 'differs'):
            self.run_splice(base, before, after)
        self.run_splice(base, before, after, todo_guard=guard)
        body = splice_tool.functions(self.dir / 'out', 'ppu_recomp')[name(ADDRS[3])][1]
        self.assertIn('+ 2;', body)
        self.assertIn('/* TODO: frob r3 */; abort();', body)

    def test_command_line(self):
        base, before, after = self.lifts()
        args = ['--base', str(base), '--before', str(before), '--after', str(after),
                '--output', str(self.dir / 'out'), '--before-revision', 'b', '--after-revision', 'a',
                '--reason', 'cli']
        self.assertEqual(splice_tool.main(args), 0)
        self.assertEqual(splice_tool.main(args), 1)  # output now exists

    def compile(self, directory):
        compiler = os.environ.get('CXX') or shutil.which('c++') or shutil.which('clang++') or shutil.which('g++')
        if not compiler or Path(compiler).stem.lower() in ('cl', 'clang-cl'):
            return
        self.compiled = 0
        for unit in sorted(Path(directory).glob('ppu_recomp_*.cpp')):
            result = subprocess.run([compiler, '-std=c++17', '-fsyntax-only', '-w',
                                     '-I', str(directory), '-I', str(ROOT / 'runtime/ppu'),
                                     '-I', str(ROOT / 'include'), str(unit)],
                                    capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, unit.name + '\n' + result.stderr[-3000:])
            self.compiled += 1
        self.assertGreater(self.compiled, 0)


if __name__ == '__main__':
    unittest.main()
