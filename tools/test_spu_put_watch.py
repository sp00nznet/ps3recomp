#!/usr/bin/env python3
"""PS3RECOMP_SPU_PUT_WATCH reports exactly the transfers that write the
watched word (single PUTs, list-element PUTs, SDCRZ), with the right fields,
and nothing when unset. Synthetic; no game files."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SpuPutWatchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which(os.environ.get('CC', 'clang')) or shutil.which('cc')
        if not cc:
            raise unittest.SkipTest('C compiler required')
        if Path(cc).stem.lower() in ('clang-cl', 'cl'):
            raise unittest.SkipTest('GCC-style C compiler required')
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.exe = Path(cls.tmp.name) / ('put-watch.exe' if os.name == 'nt' else 'put-watch')
        cmd = [cc, '-std=gnu17', '-O2', '-I', str(ROOT / 'include'), '-I', str(ROOT / 'runtime/spu'),
               '-I', str(ROOT / 'runtime/platform'), str(ROOT / 'runtime/spu/tests/test_spu_put_watch.c'),
               str(ROOT / 'runtime/platform/win32_compat.c'), '-o', str(cls.exe)]
        if os.name != 'nt':
            cmd.insert(1, '-pthread')
        subprocess.run(cmd, check=True, capture_output=True, text=True)

    def run_fixture(self, watch):
        env = {k: v for k, v in os.environ.items() if not k.startswith(('SPU_', 'PS3RECOMP_SPU_'))}
        if watch is not None:
            env['PS3RECOMP_SPU_PUT_WATCH'] = watch
        p = subprocess.run([str(self.exe)], capture_output=True, text=True, env=env, timeout=30)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIn('OK: put-watch transfers ran', p.stdout)
        return [line for line in p.stderr.splitlines() if line.startswith('[spu-put-watch]')]

    def test_unset_reports_nothing(self):
        self.assertEqual(self.run_fixture(None), [])

    def test_reports_each_write_of_the_word(self):
        lines = self.run_fixture('20044')
        self.assertEqual(len(lines), 3, '\n'.join(lines))
        fields = [dict(re.findall(r'(\w+)=(\S+)', line)) for line in lines]
        single, element, zero = fields
        for record in fields:
            self.assertEqual((record['spu'], record['image'], record['pc'], record['code_image']),
                             ('3', '7', '0x02340', '11'))
            self.assertEqual(record['word'], '0x00020044')
        self.assertEqual((single['cmd'], single['ea'], single['size'], single['before'], single['after']),
                         ('0x20', '0x00020040', '16', '0x00000000', '0x44454647'))
        self.assertEqual((element['ea'], element['lsa'], element['before'], element['after']),
                         ('0x00020040', '0x01010', '0x44454647', '0x54555657'))
        self.assertEqual((zero['cmd'], zero['ea'], zero['size'], zero['before'], zero['after']),
                         ('0x89', '0x00020000', '128', '0x54555657', '0x00000000'))

    def test_unaligned_value_watches_its_word(self):
        self.assertEqual(len(self.run_fixture('20047')), 3)

    def test_other_word_reports_nothing(self):
        self.assertEqual(self.run_fixture('30100'), [])


if __name__ == '__main__':
    unittest.main()
