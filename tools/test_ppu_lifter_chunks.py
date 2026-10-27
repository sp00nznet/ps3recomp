#!/usr/bin/env python3
"""Self-check: a relift rewrites only the source chunks whose content changed.

Drives the lifter's output stage (PPULifter.write_source_files) on 200
synthetic functions with uneven guest sizes:

1. The default line-count mode (--chunk-lines): an identical relift leaves
   every file untouched (content and mtime).
2. --chunks N writes N chunk files plus <base>_table.cpp; every function is
   defined exactly once and the table lists all of them.
3. A relift with one function's body changed rewrites exactly the chunk that
   holds it; every other file keeps its content and its mtime.
4. Chunk membership depends on the guest addresses only: the same functions
   with longer C bodies land in the same files (a line-count cut would move).

Run: python tools/test_ppu_lifter_chunks.py
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ppu_lifter import LiftedFunction, PPULifter  # noqa: E402

N_FUNCS = 200
N_CHUNKS = 8


def make_lifter(bodies=None, long=False):
    lf = PPULifter()
    addr = 0x10000
    for i in range(N_FUNCS):
        size = 0x20 + (i % 7) * 0x40   # uneven guest sizes, as in a real image
        body = [f"ctx->gpr[3] = {i};"]
        if long:
            body += [f"ctx->gpr[{k}] = 0;" for k in range(4, 20)]
        if bodies and i in bodies:
            body = bodies[i]
        lf.functions.append(LiftedFunction(name=f"func_{addr:08X}", start_addr=addr,
                                           end_addr=addr + size, body_lines=body + ["return;"]))
        addr += size
    return lf


def age(out):
    """Backdate every file, so a rewrite shows in the mtime even when it lands
    within the file system's timestamp granularity."""
    for f in os.listdir(out):
        os.utime(os.path.join(out, f), ns=(10**18, 10**18))


def snapshot(out):
    snap = {}
    for f in sorted(os.listdir(out)):
        p = os.path.join(out, f)
        with open(p) as fh:
            snap[f] = (os.stat(p).st_mtime_ns, fh.read())
    return snap


def chunk_of(snap, name):
    return [f for f, (_, txt) in snap.items()
            if f.startswith("ppu_recomp_") and f"void {name}(ppu_context* ctx) {{" in txt]


def main() -> int:
    ok = True

    def check(cond, what):
        nonlocal ok
        print(("ok   " if cond else "FAIL ") + what)
        ok = ok and bool(cond)

    # Default (line-count) mode: an identical relift writes nothing.
    with tempfile.TemporaryDirectory() as out:
        make_lifter().write_source_files(out, base="ppu_recomp", max_lines=400)
        age(out)
        s4 = snapshot(out)
        make_lifter().write_source_files(out, base="ppu_recomp", max_lines=400)
        s5 = snapshot(out)
        check(len(s4) > 1 and s5 == s4, f"--chunk-lines mode: identical relift leaves all {len(s4)} files untouched")

    with tempfile.TemporaryDirectory() as out:
        lf = make_lifter()
        try:
            paths = lf.write_source_files(out, base="ppu_recomp", chunks=N_CHUNKS)
        except TypeError as exc:
            print(f"FAIL write_source_files has no chunks= option ({exc})")
            return 1
        age(out)
        s1 = snapshot(out)
        chunks = [f for f in s1 if f.startswith("ppu_recomp_") and f != "ppu_recomp_table.cpp"]
        check(len(chunks) == N_CHUNKS and "ppu_recomp_table.cpp" in s1,
              f"{N_CHUNKS} chunks + table written ({len(paths)} paths)")
        defs = sum(len(chunk_of(s1, f.name)) for f in lf.functions)
        check(defs == N_FUNCS and all(len(chunk_of(s1, f.name)) == 1 for f in lf.functions),
              f"every function defined exactly once ({defs})")
        table = s1["ppu_recomp_table.cpp"][1]
        check(all(f.name in table for f in lf.functions), "table lists every function")
        check(sorted(os.path.basename(p) for p in paths) == sorted(s1), "returned paths are the files on disk")

        # Relift with one body changed: one chunk rewritten, the rest untouched.
        target = chunk_of(s1, lf.functions[120].name)
        make_lifter(bodies={120: ["ctx->gpr[3] = 999;", "ctx->gpr[4] = 1;"]}).write_source_files(
            out, base="ppu_recomp", chunks=N_CHUNKS)
        s2 = snapshot(out)
        changed = [f for f in s2 if s2[f] != s1.get(f)]
        check(changed == target, f"one body change rewrites only its chunk: changed={changed}, expected={target}")

    # Stability: longer bodies everywhere, same chunk membership.
    with tempfile.TemporaryDirectory() as out:
        make_lifter(long=True).write_source_files(out, base="ppu_recomp", chunks=N_CHUNKS)
        s3 = snapshot(out)
        moved = [f.name for f in lf.functions if chunk_of(s3, f.name) != chunk_of(s1, f.name)]
        check(not moved, f"chunk membership follows addresses only ({len(moved)} functions moved)")

    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
