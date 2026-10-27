#!/usr/bin/env python3
"""cellSaveDataUserAutoSave keeps its ninth argument (userdata).

UserAutoSave and UserAutoLoad take nine arguments; the ninth is on the
guest stack. Only UserAutoLoad had a context handler, and nothing in the
runtime called cellSaveData_register_ctx_handlers(), so both went through
the generic eight-argument binding and the title's callbacks saw
cbResult->userdata == 0.

Two checks:
  1. libs/system/tests/test_savedata_user_autosave.c registers cellSaveData's
     context handlers and calls both handlers; each must hand the stack
     userdata to the stat callback.
  2. ppu_sysprx_register() (runtime/ppu/ppu_sysprx.cpp) calls
     cellSaveData_register_ctx_handlers(), so a project cannot forget it.

It needs a host C compiler (under CI=1 a missing one fails instead of
skipping).

Usage:
    python3 tools/test_savedata_user_autosave.py
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
SOURCE = os.path.join(ROOT, "libs", "system", "tests", "test_savedata_user_autosave.c")
SYSPRX = os.path.join(ROOT, "runtime", "ppu", "ppu_sysprx.cpp")


def _cc():
    return (shutil.which(os.environ.get("CC", "cc")) or shutil.which("clang")
            or shutil.which("gcc"))


def _wiring():
    with open(SYSPRX, encoding="utf-8", errors="replace") as f:
        src = f.read()
    m = re.search(r'extern\s+"C"\s+void\s+ppu_sysprx_register\s*\(\s*void\s*\)\s*\{', src)
    if not m:
        return False
    # Body of ppu_sysprx_register: up to the matching closing brace.
    depth, i = 1, m.end()
    while i < len(src) and depth:
        depth += {"{": 1, "}": -1}.get(src[i], 0)
        i += 1
    body = re.sub(r"/\*.*?\*/|//[^\n]*", "", src[m.end():i], flags=re.S)
    return re.search(r"\bcellSaveData_register_ctx_handlers\s*\(\s*\)\s*;", body) is not None


def main():
    failed = 0
    ok = _wiring()
    print("%s: ppu_sysprx_register() calls cellSaveData_register_ctx_handlers()"
          % ("ok  " if ok else "FAIL"))
    failed += not ok

    cc = _cc()
    if cc is None:
        if os.environ.get("CI"):
            print("FAIL: no C compiler found")
            return 1
        print("skip test_savedata_user_autosave.c: no C compiler")
        return 1 if failed else 0
    with tempfile.TemporaryDirectory() as td:
        exe = os.path.join(td, "test_savedata_user_autosave")
        cmd = [cc, "-std=gnu17", "-w", "-I" + os.path.join(ROOT, "include"),
               "-o", exe, SOURCE]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print("FAIL: fixture did not build")
            print((r.stdout + r.stderr)[-4000:])
            return 1
        scratch = os.path.join(td, "saves")
        os.mkdir(scratch)
        r = subprocess.run([exe, scratch], capture_output=True, text=True)
        lines = [l for l in r.stdout.splitlines() if not l.startswith(("[cellSaveData]", "    SP+"))]
        print("\n".join(lines))
        if r.returncode != 0:
            failed += 1
    print("PASS" if not failed else "FAIL")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
