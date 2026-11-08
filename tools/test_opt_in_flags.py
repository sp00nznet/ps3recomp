#!/usr/bin/env python3
"""docs/OPT_IN_FLAGS.md matches the switches the tree reads, and the check
(tools/check_opt_in_flags.py) catches undocumented, stale and unknown dynamic
reads in synthetic trees."""
from pathlib import Path
import sys
import tempfile
import textwrap
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_opt_in_flags as C  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def doc(*rows):
    lines = ['# Opt-in flags', '', '| Switch | Kind | Default | Class | What it does | Read in |',
             '|---|---|---|---|---|---|']
    lines += [f'| `{name}` | {kind} | unset | diagnostic | test | `x` |' for name, kind in rows]
    return '\n'.join(lines) + '\n'


class Tree:
    def __init__(self, files):
        self.tmp = tempfile.TemporaryDirectory(prefix='opt-in-flags-')
        self.root = Path(self.tmp.name)
        for rel, text in files.items():
            path = self.root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(textwrap.dedent(text))

    def check(self, dynamic=None, wrappers=None):
        problems, env, cmake = C.check(self.root, dynamic or {}, wrappers or {})
        self.tmp.cleanup()
        return problems, env, cmake


SOURCE = '''\
    #include <stdlib.h>
    #define WATCH_ENV "MACRO_VAR"
    // getenv("IN_LINE_COMMENT") is not a read
    /* nor is getenv("IN_BLOCK_COMMENT") */
    static int on(void) { return getenv("DIRECT_VAR") != NULL; }
    static const char* once(void) { return ps3_env_once("ONCE_VAR"); }
    static const char* watch(void) { return getenv(WATCH_ENV); }
    static const char* label(void) { return "getenv(\\"IN_STRING\\")"; }
    static int quote(char c) { return c == '"' && getenv("AFTER_QUOTE_CHAR") != 0; }
'''
CMAKE = 'option(FAKE_OPTION "x" OFF)\nset(FAKE_CACHE "" CACHE STRING "x")\n'
ALL = [('DIRECT_VAR', 'env'), ('ONCE_VAR', 'env'), ('MACRO_VAR', 'env'), ('AFTER_QUOTE_CHAR', 'env'),
       ('FAKE_OPTION', 'CMake'), ('FAKE_CACHE', 'CMake')]


class OptInFlagsTests(unittest.TestCase):
    def test_tree_matches_the_document(self):
        problems, env, cmake = C.check(ROOT)
        self.assertEqual(problems, [], '\n'.join(problems))
        self.assertGreater(len(env), 0)
        self.assertIn('PS3RECOMP_BUILD_TESTS', cmake)

    def test_documented_tree_passes(self):
        problems, env, cmake = Tree({'runtime/a.c': SOURCE, 'CMakeLists.txt': CMAKE,
                                     'docs/OPT_IN_FLAGS.md': doc(*ALL)}).check()
        self.assertEqual(problems, [])
        self.assertEqual(sorted(env), ['AFTER_QUOTE_CHAR', 'DIRECT_VAR', 'MACRO_VAR', 'ONCE_VAR'])
        self.assertEqual(sorted(cmake), ['FAKE_CACHE', 'FAKE_OPTION'])

    def test_undocumented_read_fails(self):
        problems, _, _ = Tree({'runtime/a.c': SOURCE + 'int x(void){return !!getenv("NEW_VAR");}\n',
                               'CMakeLists.txt': CMAKE,
                               'docs/OPT_IN_FLAGS.md': doc(*ALL)}).check()
        self.assertEqual(len(problems), 1)
        self.assertIn('NEW_VAR', problems[0])
        self.assertIn('missing', problems[0])

    def test_stale_entry_fails(self):
        problems, _, _ = Tree({'runtime/a.c': SOURCE, 'CMakeLists.txt': CMAKE,
                               'docs/OPT_IN_FLAGS.md': doc(*ALL, ('GONE_VAR', 'env'),
                                                           ('GONE_OPTION', 'CMake'))}).check()
        self.assertEqual(len(problems), 2)
        self.assertTrue(all('nothing reads it' in p or 'no CMake file' in p for p in problems))

    def test_undocumented_option_fails(self):
        problems, _, _ = Tree({'runtime/a.c': SOURCE,
                               'CMakeLists.txt': CMAKE + 'option(NEW_OPTION "x" ON)\n',
                               'docs/OPT_IN_FLAGS.md': doc(*ALL)}).check()
        self.assertEqual(len(problems), 1)
        self.assertIn('NEW_OPTION', problems[0])

    def test_dynamic_read_must_be_listed(self):
        files = {'libs/b.c': 'static const char* n[] = {"ARR_A", "ARR_B"};\n'
                             'int f(int i){ return getenv(n[i]) != 0; }\n',
                 'docs/OPT_IN_FLAGS.md': doc(('ARR_A', 'env'), ('ARR_B', 'env'))}
        problems, _, _ = Tree(files).check()
        self.assertTrue(any('DYNAMIC_READS' in p and 'getenv(n[i])' in p for p in problems), problems)
        problems, env, _ = Tree(files).check(dynamic={('libs/b.c', 'n[i]'): ('ARR_A', 'ARR_B')})
        self.assertEqual(problems, [])
        self.assertEqual(sorted(env), ['ARR_A', 'ARR_B'])

    def test_wrapper_calls_are_reads(self):
        files = {'libs/c.c': 'static int flag(const char* name){ return getenv(name) != 0; }\n'
                             'int g(void){ return flag("WRAPPED_VAR"); }\n',
                 'docs/OPT_IN_FLAGS.md': doc(('WRAPPED_VAR', 'env'))}
        problems, _, _ = Tree(files).check(dynamic={('libs/c.c', 'name'): ()},
                                           wrappers={'libs/c.c': ('flag',)})
        self.assertEqual(problems, [])
        # The same helper name in another file is not a reader there.
        files['libs/d.c'] = 'int h(void){ return flag("ELSEWHERE"); }\n'
        problems, env, _ = Tree(files).check(dynamic={('libs/c.c', 'name'): ()},
                                             wrappers={'libs/c.c': ('flag',)})
        self.assertNotIn('ELSEWHERE', env)

    def test_tests_are_not_scanned(self):
        problems, env, cmake = Tree({
            'runtime/tests/t.c': 'int t(void){ return !!getenv("TEST_ONLY"); }\n',
            'libs/x/test_y.c': 'int u(void){ return !!getenv("TEST_TOO"); }\n',
            'tests/foo/CMakeLists.txt': 'option(TEST_OPTION "x" ON)\n',
            'docs/OPT_IN_FLAGS.md': doc()}).check()
        self.assertEqual((problems, env, cmake), ([], {}, {}))


if __name__ == '__main__':
    unittest.main()
