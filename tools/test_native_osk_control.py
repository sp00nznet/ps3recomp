#!/usr/bin/env python3
"""The command writer must publish one complete bounded private command."""
from pathlib import Path
import os
import stat
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NativeOskControlWriterTests(unittest.TestCase):
    def run_writer(self, path, token):
        return subprocess.run([sys.executable, str(ROOT / 'tools/native_osk_confirm.py'),
                               str(path), token], capture_output=True, text=True)

    def test_atomic_private_command_replaces_previous_content(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'control'
            path.write_text('malformed old command')
            result = self.run_writer(path, '18446744073709551615')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(path.read_bytes(), b'confirm 18446744073709551615\n')
            if os.name != 'nt':
                self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
            self.assertEqual(sorted(p.name for p in Path(directory).iterdir()), ['control'])

    def test_invalid_tokens_leave_existing_command_untouched(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'control'
            path.write_bytes(b'original')
            for token in ('0', '-1', '+1', '1\nconfirm 2', '1 x',
                          '18446744073709551616', 'x', '9' * 10000):
                with self.subTest(token=token[:30]):
                    result = self.run_writer(path, token)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertEqual(path.read_bytes(), b'original')
            self.assertEqual(sorted(p.name for p in Path(directory).iterdir()), ['control'])


if __name__ == '__main__':
    unittest.main()
