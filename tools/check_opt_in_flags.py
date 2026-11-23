#!/usr/bin/env python3
"""Keep docs/OPT_IN_FLAGS.md in step with the switches the code reads.

Scans the runtime and library sources for environment variables they read and
the CMake files for option() and CACHE switches, then compares the names with
the ones listed in docs/OPT_IN_FLAGS.md. Exits 1 if a switch the code reads is
missing from the document, or if the document lists a switch nothing reads any
more.

    python tools/check_opt_in_flags.py           # check the tree
    python tools/check_opt_in_flags.py --list    # print every name found

What counts as a read:
  * a direct reader called with a string literal: getenv, secure_getenv,
    ps3_env_once, GetEnvironmentVariableA/W, _wgetenv and _dupenv_s;
  * a call to one of the WRAPPERS below (a helper whose own body reads the
    variable it is given) with a string literal name;
  * a macro that names its variable, `#define X "NAME"`, passed to a reader;
  * Python tools: os.environ.get / os.environ[...] / os.getenv with a literal.

A reader called with anything else (a variable, an array element) is a
dynamic read. Each one must be listed in DYNAMIC_READS with the names it can
read, so a new indirect read cannot slip past the check unnoticed.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOC = Path('docs/OPT_IN_FLAGS.md')

C_DIRS = ('runtime', 'libs', 'include', 'templates')
C_SUFFIXES = {'.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.inc', '.m', '.mm'}
PY_DIRS = ('tools',)
CMAKE_GLOBS = ('CMakeLists.txt', 'cmake/*.cmake', 'templates/**/CMakeLists.txt',
               'templates/**/*.cmake')

# Readers that take the variable name as their first argument.
DIRECT_READERS = ('getenv', 'secure_getenv', 'ps3_env_once', '_wgetenv',
                  'GetEnvironmentVariableA', 'GetEnvironmentVariableW')

# Helpers that read the environment variable named by their first argument,
# by the file that defines them (all are file-local). A literal-name call to
# one of these is a read of that name; the getenv inside each helper's body is
# a dynamic read listed in DYNAMIC_READS with no names of its own.
WRAPPERS: dict[str, tuple[str, ...]] = {
    'runtime/platform/ps3_stdio.c': ('ps3_stdio_env_ms',),
    'libs/codec/ffmpeg_runtime.c': ('env_or',),
    'libs/video/rsx_d3d12_backend.c': ('env_on', 'env_str'),
    'libs/video/rsx_draw_engine.c': ('eng_parse_hash_list',),
    'libs/video/rsx_live_draw.c': ('LD_DIAG_ENABLED',),
    'libs/video/rsx_vulkan_backend.c': ('vk_env_on',),
}

# Dynamic reads: (path, the reader call's first argument) -> the names it can
# read. A helper's own body maps to () because its callers are scanned instead.
DYNAMIC_READS: dict[tuple[str, str], tuple[str, ...]] = {
    ('include/ps3emu/ps3_env_once.h', 'name'): (),           # ps3_env_once itself
    ('runtime/platform/win32_compat.h', 'name'): (),         # POSIX GetEnvironmentVariableA
    ('runtime/platform/ps3_stdio.c', 'name'): (),            # ps3_stdio_env_ms
    ('libs/codec/ffmpeg_runtime.c', 'name'): (),             # env_or
    ('libs/video/rsx_d3d12_backend.c', 'name'): (),          # env_on, env_str
    ('libs/video/rsx_draw_engine.c', 'name'): (),            # eng_parse_hash_list
    ('libs/video/rsx_live_draw.c', 'name'): (),              # LD_DIAG_ENABLED
    ('libs/video/rsx_vulkan_backend.c', 'name'): (),         # vk_env_on
    # Any of these keeps lifted loads (vars) or stores (svars) out of line.
    ('runtime/ppu/ppu_loader.cpp', 'vars[i]'): (
        'PPU_RWATCH', 'PPU_FORCE_READ_ADDR', 'YDKJ_RWATCH', 'PPU_RVAL', 'PPU_RVAL64',
        'PPU_HOTMAP', 'PPU_HOTREAD', 'PPU_SPINBT'),
    ('runtime/ppu/ppu_loader.cpp', 'svars[i]'): ('PPU_WVAL', 'PPU_WWATCH', 'PT'),
    # Any of these diagnostics turns direct SPU execution off.
    ('runtime/spu/spu_drain.c', 'diag[i]'): (
        'SPU_LOCKSTEP', 'SPU_TAILRET', 'SPU_PCHIST', 'SPU_NO_LS0_END', 'SPU_EXITTRACE',
        'SPU_STEPTRACE', 'SPU_JOBDRAIN', 'SPURS_PM_FLOW'),
    # The host locale, in POSIX precedence order.
    ('libs/system/cellSysutil.c', 'vars[i]'): ('LC_ALL', 'LC_MESSAGES', 'LANG'),
}

# Python tools read these toolchain locations; they are not switches.
PY_IGNORED = {'CC', 'CXX', 'ProgramFiles', 'ProgramFiles(x86)', 'PATH', 'HOME',
              'TMPDIR', 'SYSTEMROOT'}

NAME = r'[A-Za-z_][A-Za-z0-9_]*'


def strip_c_comments(text: str) -> str:
    """Blank C/C++ comments, keeping string and character literals and line
    numbers intact."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            j = text.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i))
            i = j
        elif c == '/' and i + 1 < n and text[i + 1] == '*':
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(''.join(ch if ch == '\n' else ' ' for ch in text[i:j]))
            i = j
        elif c in '"\'':
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == '\\' else 1
            out.append(text[i:j + 1])
            i = j + 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def mask_strings(text: str) -> str:
    """The same text with the inside of every string literal blanked, so a
    reader's name inside a string is not mistaken for a call."""
    text = re.sub(r"'(?:\\.|[^'\\\n])'", lambda m: ' ' * len(m.group(0)), text)   # '"' and friends
    return re.sub(r'"(?:\\.|[^"\\\n])*"', lambda m: '"' + ' ' * (len(m.group(0)) - 2) + '"', text)


def first_argument(text: str, start: int) -> str:
    """The first argument of the call whose '(' is at text[start]."""
    depth, i = 0, start
    while i < len(text):
        c = text[i]
        if c in '([{':
            depth += 1
        elif c in ')]}':
            depth -= 1
            if depth == 0:
                return text[start + 1:i].strip()
        elif c == ',' and depth == 1:
            return text[start + 1:i].strip()
        elif c == '"':
            i += 1
            while i < len(text) and text[i] != '"':
                i += 2 if text[i] == '\\' else 1
        i += 1
    return text[start + 1:].strip()


def literal(arg: str, macros: dict[str, str]) -> str | None:
    arg = arg.strip()
    m = re.fullmatch(r'L?"(' + NAME + r')"', arg)
    if m:
        return m.group(1)
    return macros.get(arg)


def is_test_path(rel: str) -> bool:
    parts = rel.split('/')
    return 'tests' in parts or parts[-1].startswith('test_')


def c_sources(root: Path):
    for top in C_DIRS:
        base = root / top
        if not base.is_dir():
            continue
        for path in sorted(base.rglob('*')):
            if path.suffix in C_SUFFIXES and path.is_file():
                rel = path.relative_to(root).as_posix()
                if not is_test_path(rel):
                    yield rel, path


def scan_env(root: Path, dynamic=None, wrappers=None):
    """Return ({name: sorted files}, [unlisted dynamic reads])."""
    dynamic = DYNAMIC_READS if dynamic is None else dynamic
    wrappers = WRAPPERS if wrappers is None else wrappers
    found: dict[str, set[str]] = {}
    unlisted = []
    define = re.compile(r'^\s*#\s*define\s+(' + NAME + r')\s+"(' + NAME + r')"\s*$', re.M)
    sources = [(rel, strip_c_comments(path.read_text(encoding='utf-8', errors='replace')))
               for rel, path in c_sources(root)]
    macros: dict[str, str] = {}          # name-only macros are visible tree-wide
    for _, text in sources:
        macros.update(define.findall(text))
    for rel, text in sources:
        readers = DIRECT_READERS + tuple(wrappers.get(rel, ()))
        call = re.compile(r'\b(' + '|'.join(map(re.escape, readers)) + r'|_dupenv_s)\s*\(')
        for m in call.finditer(mask_strings(text)):
            reader = m.group(1)
            before = text[max(0, m.start() - 40):m.start()]
            # Skip the helper's own definition or declaration.
            if re.search(r'(\b(?:char|DWORD|int|errno_t|void|bool|u32|s32|wchar_t)\s*\**\s*|#\s*define\s+)$', before):
                continue
            paren = m.end() - 1
            arg = first_argument(text, paren)
            if re.search(r'\b(?:char|wchar_t|LPCSTR|LPCWSTR)\b', arg):
                continue                      # a declaration's parameter list
            if reader == '_dupenv_s':
                args = text[paren + 1:text.find(')', paren)].split(',')
                arg = args[2] if len(args) > 2 else ''
            name = literal(arg, macros)
            if name:
                found.setdefault(name, set()).add(rel)
                continue
            key = (rel, arg)
            if key in dynamic:
                for name in dynamic[key]:
                    found.setdefault(name, set()).add(rel)
            else:
                line = text.count('\n', 0, m.start()) + 1
                unlisted.append(f'{rel}:{line}: {reader}({arg})')
    for top in PY_DIRS:
        base = root / top
        if not base.is_dir():
            continue
        for path in sorted(base.glob('*.py')):
            rel = path.relative_to(root).as_posix()
            if is_test_path(rel) or path.name == Path(__file__).name:
                continue
            text = path.read_text(encoding='utf-8', errors='replace')
            for name in re.findall(
                    r'''os\.(?:environ\.get\(|environ\[|getenv\()\s*['"]([A-Za-z_][A-Za-z0-9_()]*)['"]''', text):
                if name not in PY_IGNORED:
                    found.setdefault(name, set()).add(rel)
    return {k: sorted(v) for k, v in found.items()}, unlisted


def scan_cmake(root: Path):
    found: dict[str, set[str]] = {}
    option = re.compile(r'^\s*option\s*\(\s*(' + NAME + r')\b', re.M | re.I)
    cache = re.compile(r'^\s*set\s*\(\s*(' + NAME + r')\b[^)]*?\bCACHE\s+(?:BOOL|STRING|PATH|FILEPATH)\b',
                       re.M | re.I | re.S)
    seen = set()
    for pattern in CMAKE_GLOBS:
        for path in sorted(root.glob(pattern)):
            if path in seen or not path.is_file():
                continue
            seen.add(path)
            rel = path.relative_to(root).as_posix()
            if is_test_path(rel):
                continue
            text = re.sub(r'#[^\n]*', '', path.read_text(encoding='utf-8', errors='replace'))
            for name in option.findall(text) + cache.findall(text):
                found.setdefault(name, set()).add(rel)
    return {k: sorted(v) for k, v in found.items()}


def documented(root: Path):
    """Names in the document's tables: rows whose first cell is `NAME`, with
    kind `env` or `CMake` in the second cell."""
    env, cmake = set(), set()
    path = root / DOC
    if not path.is_file():
        return env, cmake
    row = re.compile(r'^\|\s*`([^`]+)`\s*\|\s*(env|CMake)\s*\|', re.M)
    for name, kind in row.findall(path.read_text(encoding='utf-8')):
        (env if kind == 'env' else cmake).add(name)
    return env, cmake


def check(root: Path, dynamic=None, wrappers=None):
    env, unlisted = scan_env(root, dynamic, wrappers)
    cmake = scan_cmake(root)
    doc_env, doc_cmake = documented(root)
    problems = [f'dynamic environment read not listed in DYNAMIC_READS: {u}' for u in unlisted]
    for name in sorted(set(env) - doc_env):
        problems.append(f'env {name} (read in {", ".join(env[name])}) is missing from {DOC.as_posix()}')
    for name in sorted(doc_env - set(env)):
        problems.append(f'env {name} is listed in {DOC.as_posix()} but nothing reads it')
    for name in sorted(set(cmake) - doc_cmake):
        problems.append(f'CMake {name} ({", ".join(cmake[name])}) is missing from {DOC.as_posix()}')
    for name in sorted(doc_cmake - set(cmake)):
        problems.append(f'CMake {name} is listed in {DOC.as_posix()} but no CMake file defines it')
    return problems, env, cmake


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--root', type=Path, default=ROOT)
    ap.add_argument('--list', action='store_true', help='print every switch found and where')
    args = ap.parse_args(argv)
    problems, env, cmake = check(args.root)
    if args.list:
        for name in sorted(env):
            print(f'env\t{name}\t{" ".join(env[name])}')
        for name in sorted(cmake):
            print(f'CMake\t{name}\t{" ".join(cmake[name])}')
        return 0
    for p in problems:
        print('FAIL:', p)
    if problems:
        print(f'{len(problems)} problem(s). Update {DOC.as_posix()} (see its header).')
        return 1
    print(f'OK: {len(env)} environment and {len(cmake)} CMake switches match {DOC.as_posix()}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
