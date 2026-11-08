#!/usr/bin/env python3
"""Synthetic tests for tools/fetch_sdk_pins.py against a local throwaway repository."""
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fetch_sdk_pins as pins  # noqa: E402

IDENTITY = ['-c', 'user.name=test', '-c', 'user.email=test@example.invalid']


class FetchTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        base = Path(self.tmp.name)
        self.origin = base / 'origin'
        self.root = base / 'port'
        self.root.mkdir()
        subprocess.run(['git', 'init', '-q', str(self.origin)], check=True)
        self.commits = {}
        for name in ('one', 'two'):
            (self.origin / 'f').write_text(name)
            self.git('add', 'f')
            self.git(*IDENTITY, 'commit', '-q', '-m', name)
            self.git(*IDENTITY, 'tag', '-a', f'v-{name}', '-m', name)
            self.commits[name] = self.git('rev-parse', 'HEAD')
        # A commit no branch or tag reaches until it is asked for by SHA.
        self.git('checkout', '-q', '-b', 'side')
        (self.origin / 'f').write_text('side')
        self.git(*IDENTITY, 'commit', '-q', '-am', 'side')
        self.commits['side'] = self.git('rev-parse', 'HEAD')
        self.git('checkout', '-q', '-')
        self.git('update-ref', '-d', 'refs/heads/side')
        self.git('config', 'uploadpack.allowAnySHA1InWant', 'true')

    def git(self, *args):
        return subprocess.run(['git', '-C', str(self.origin), *args], check=True,
                              capture_output=True, text=True).stdout.strip()

    def pin(self, **fields):
        return {'path': 'third_party/sdk', **fields}

    def test_checks_out_the_pinned_tagged_commit(self):
        path = pins.fetch(self.root, str(self.origin), 'sdk', self.pin(tag='v-one', commit=self.commits['one']))
        self.assertEqual((path / 'f').read_text(), 'one')
        # Re-running moves an existing checkout to a new pin.
        path = pins.fetch(self.root, str(self.origin), 'sdk', self.pin(tag='v-two', commit=self.commits['two']))
        self.assertEqual((path / 'f').read_text(), 'two')

    def test_refuses_a_tag_that_names_another_commit(self):
        with self.assertRaisesRegex(ValueError, 'tag v-two is'):
            pins.fetch(self.root, str(self.origin), 'sdk', self.pin(tag='v-two', commit=self.commits['one']))
        with self.assertRaisesRegex(ValueError, 'tag v-none is missing'):
            pins.fetch(self.root, str(self.origin), 'sdk', self.pin(tag='v-none', commit=self.commits['one']))

    def test_untagged_commit_is_fetched_by_sha(self):
        path = pins.fetch(self.root, str(self.origin), 'sdk', self.pin(commit=self.commits['side']))
        self.assertEqual((path / 'f').read_text(), 'side')

    def test_refuses_a_modified_checkout(self):
        path = pins.fetch(self.root, str(self.origin), 'sdk', self.pin(commit=self.commits['one']))
        (path / 'f').write_text('local edit')
        with self.assertRaises((ValueError, subprocess.CalledProcessError)):
            pins.fetch(self.root, str(self.origin), 'sdk', self.pin(commit=self.commits['one']))

    def test_command_line_uses_the_pin_file_directory(self):
        (self.root / 'sdk-pins.json').write_text(json.dumps({
            'format': 1, 'repository': str(self.origin),
            'pins': {'runtime': self.pin(tag='v-two', commit=self.commits['two']),
                     'lifter': {'path': 'third_party/lifter', 'commit': self.commits['one']}}}))
        self.assertEqual(pins.main([str(self.root / 'sdk-pins.json'), '--role', 'lifter']), 0)
        self.assertEqual((self.root / 'third_party/lifter/f').read_text(), 'one')
        self.assertFalse((self.root / 'third_party/sdk').exists())


class PinFileTests(unittest.TestCase):
    GOOD = {'format': 1, 'repository': 'https://example.invalid/sdk',
            'pins': {'sdk': {'commit': 'a' * 40, 'tag': 'v1', 'path': 'third_party/sdk'}}}

    def bad(self, **change):
        document = json.loads(json.dumps(self.GOOD))
        document['pins']['sdk'].update(change)
        return document

    def test_accepts_a_complete_pin(self):
        self.assertEqual(list(pins.load_pins(self.GOOD)), ['sdk'])

    def test_rejects_incomplete_or_unsafe_pins(self):
        for document in (self.bad(commit='abc123'), self.bad(commit='A' * 40), self.bad(path=''),
                         self.bad(path='/abs/sdk'), self.bad(path='../outside'), self.bad(tag=''),
                         {**self.GOOD, 'format': 2}, {**self.GOOD, 'pins': {}},
                         {'format': 1, 'pins': self.GOOD['pins']}):
            with self.assertRaises(ValueError, msg=document):
                pins.load_pins(document)


if __name__ == '__main__':
    unittest.main()
