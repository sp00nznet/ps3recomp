#!/usr/bin/env python3
"""
Compile-check the PPU boot scaffold, on whatever platform you are standing on.

runtime/ppu/ is the one part of the runtime nothing in this repository builds.
CMakeLists.txt excludes it from the library because those files #include
ppu_recomp.h, which the lifter generates per game -- so the scaffold is only
ever compiled inside somebody's game port, on their machine, on Windows. That
is exactly the shape of a claim that rots: docs/BUILDING.md advertised macOS as
"Full support" for months while the tree did not compile there at all, and it
took CI to find out.

This script closes that hole without needing a game. It synthesises a stub
ppu_recomp.h from the lifter's OWN HEADER_PREAMBLE -- the same string
ppu_lifter.py writes into the real header, imported rather than copied, so the
stub cannot drift from what games actually get -- and compiles the scaffold
against it. What it proves is that every declaration the scaffold reaches for
exists on this platform. It does not link, and it cannot run anything.

It is a ratchet, not a pass/fail gate, because the POSIX port is not finished:
the baseline in ppu_scaffold_baseline.json records how many errors each file
currently produces per toolchain, and the check fails if a number goes UP. That
makes every step of the port measurable, and makes a regression on the platform
that already works (Windows, where the baseline is 0) a hard failure today.

  python tools/check_ppu_scaffold.py            # check against the baseline
  python tools/check_ppu_scaffold.py --update   # re-record it after improving

A toolchain with no baseline entry reports its numbers and passes, so a new
platform's first CI run tells you what to record rather than failing on
arrival.
"""

import argparse
import importlib.util
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINE = os.path.join(ROOT, "tools", "ppu_scaffold_baseline.json")

# The scaffold translation units.
#
# boot_main.cpp used to be excluded here on the grounds that it needed the
# per-game SPU recomp header too. It does not -- ppu_recomp.h is the only
# generated header it wants, and the stub covers that. Leaving it out meant
# the file the frame clock lives in was the one file nothing checked.
SOURCES = [
    "runtime/ppu/ppu_loader.cpp",
    "runtime/ppu/ppu_hle.cpp",
    "runtime/ppu/ppu_fs.cpp",
    "runtime/ppu/ppu_sysprx.cpp",
    "runtime/ppu/tests/boot_main.cpp",
]

INCLUDE_DIRS = [
    "include",
    "runtime/ppu",
    "runtime/spu",
    "runtime/memory",
    "runtime/syscalls",
    "libs/video",
]


def stub_header() -> str:
    """The lifter's real header preamble, plus the few externs the generated
    header carries that the scaffold calls into.

    Imported from ppu_lifter.py rather than copied: if the ppu_context layout
    changes, this stub changes with it and the check keeps testing the truth.
    PPU_THREAD_LOCAL arrives the same way, now that it lives in the header
    rather than the generated source -- so this check also verifies the
    scaffold and the lifted code agree on how to spell thread-local.
    """
    spec = importlib.util.spec_from_file_location(
        "ppu_lifter", os.path.join(ROOT, "tools", "ppu_lifter.py"))
    lifter = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(lifter)
    return lifter.HEADER_PREAMBLE + (
        '\n#ifdef __cplusplus\nextern "C" {\n#endif\n'
        "void ppu_recomp_register(void);\n"
        '#ifdef __cplusplus\n}\n#endif\n'
    )


def compiler_family(cxx: str) -> str:
    """Ask the compiler what it is, rather than guessing from its filename.

    macOS's `c++` is Apple Clang, and a name-based guess called it gcc -- so the
    first macOS CI run recorded itself under a key that names the wrong
    compiler. `cc` on Linux is ambiguous the same way.
    """
    try:
        out = subprocess.run([cxx, "--version"], capture_output=True, text=True,
                             timeout=15)
        text = (out.stdout + out.stderr).lower()
        if "clang" in text:
            return "clang"
        if "gcc" in text or "free software foundation" in text:
            return "gcc"
    except Exception:
        pass
    return "clang" if "clang" in os.path.basename(cxx) else "gcc"


def pick_toolchain(explicit=None):
    """Return (key, argv-prefix). The key names the row in the baseline."""
    if explicit:
        cxx = explicit
    else:
        cxx = os.environ.get("CXX")

    if sys.platform == "win32":
        if not cxx:
            for cand in ("clang-cl",
                         r"C:\Program Files\LLVM\bin\clang-cl.exe"):
                if shutil.which(cand) or os.path.exists(cand):
                    cxx = cand
                    break
        if not cxx:
            return None, None
        # clang-cl, not cl: the scaffold uses __builtin_bswap*,
        # __builtin_return_address and __int128, none of which MSVC has.
        # -ferror-limit=0: clang stops at 20 errors by default, which would
        # silently cap the ratchet's counts and hide regressions above it.
        return "windows-clang-cl", [cxx, "/std:c++20", "/c", "/W0", "/EHsc",
                                    "/D_CRT_SECURE_NO_WARNINGS",
                                    "/clang:-ferror-limit=0"]

    if not cxx:
        cxx = "c++"
    family = compiler_family(cxx)
    osname = "darwin" if sys.platform == "darwin" else "linux"
    argv = [cxx, "-std=c++20", "-fsyntax-only", "-w"]
    if family == "clang":
        argv.append("-ferror-limit=0")   # see the note above
    return f"{osname}-{family}", argv


def count_errors(text: str) -> int:
    """Diagnostic lines only.

    Matching a bare "error" also caught clang's "1 error generated." trailer and
    double-counted every file that failed, so the colon is load-bearing.
    """
    return len(re.findall(r"(?m)^[^\n]*?\berror:", text))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--update", action="store_true",
                    help="re-record the baseline for this toolchain")
    ap.add_argument("--cxx", help="compiler to use (default: $CXX, or the "
                                  "platform's usual)")
    ap.add_argument("--verbose", action="store_true",
                    help="print the compiler diagnostics")
    args = ap.parse_args()

    key, prefix = pick_toolchain(args.cxx)
    if not key:
        print("[ppu-scaffold] no usable C++ compiler found; skipping")
        return 0

    tmp = tempfile.mkdtemp(prefix="ppu_scaffold_")
    try:
        with open(os.path.join(tmp, "ppu_recomp.h"), "w") as fh:
            fh.write(stub_header())

        inc_flag = "/I" if key.startswith("windows") else "-I"
        incs = [inc_flag + tmp]
        incs += [inc_flag + os.path.join(ROOT, d) for d in INCLUDE_DIRS]

        print(f"[ppu-scaffold] toolchain: {key}")
        print(f"[ppu-scaffold] {' '.join(os.path.basename(p) for p in prefix[:1])}"
              f" {platform.machine()}")

        results, total = {}, 0
        for src in SOURCES:
            cmd = list(prefix) + incs
            if key.startswith("windows"):
                cmd += [f"/Fo{os.path.join(tmp, os.path.basename(src))}.obj"]
            cmd += [os.path.join(ROOT, src)]
            proc = subprocess.run(cmd, capture_output=True, text=True)
            n = count_errors(proc.stdout + proc.stderr)
            results[src] = n
            total += n
            print(f"  {n:4d}  {src}")
            if args.verbose and n:
                print((proc.stdout + proc.stderr).strip()[:4000])
        print(f"  {total:4d}  TOTAL")

        base = {}
        if os.path.exists(BASELINE):
            with open(BASELINE) as fh:
                base = json.load(fh)

        if args.update:
            base[key] = results
            with open(BASELINE, "w") as fh:
                json.dump(base, fh, indent=2, sort_keys=True)
                fh.write("\n")
            print(f"[ppu-scaffold] baseline recorded for {key}")
            return 0

        if key not in base:
            print(f"[ppu-scaffold] no baseline for {key} yet -- reporting only.")
            print(f"[ppu-scaffold] record it with: "
                  f"python tools/check_ppu_scaffold.py --update")
            return 0

        want, bad, better = base[key], [], []
        for src in SOURCES:
            got, exp = results[src], want.get(src)
            if exp is None:
                print(f"[ppu-scaffold] {src} is new; run --update")
                continue
            if got > exp:
                bad.append(f"{src}: {exp} -> {got}")
            elif got < exp:
                better.append(f"{src}: {exp} -> {got}")

        for line in better:
            print(f"[ppu-scaffold] IMPROVED  {line}")
        if better and not bad:
            print("[ppu-scaffold] the port moved forward -- re-record with "
                  "--update so the ratchet holds the new ground")
        if bad:
            print("\n[ppu-scaffold] REGRESSION: the scaffold got worse on "
                  f"{key}")
            for line in bad:
                print(f"  {line}")
            print("\nRe-run with --verbose to see the diagnostics.")
            return 1

        print(f"[ppu-scaffold] OK -- no regression against the {key} baseline")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
