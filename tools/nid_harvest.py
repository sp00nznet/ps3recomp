"""nid_harvest.py - name unresolved firmware NIDs using debug-build symbol tables.

A NID is SHA-1(name + suffix)[:4]; you cannot invert it, so an import NID is only
resolvable if you already have a candidate *name* to hash. Debug/prototype builds
(see unfself.py) hand us tens of thousands of real SDK function names in their
.symtab. Hashing every firmware-shaped name gives a NID->name gazetteer; intersecting
it with the NIDs those same builds actually *import* (their proc_prx_param -> libstub
tables, via ppu_loader.parse_imports) yields definitive (library, name, NID) triples:
compute_nid(name) == the imported NID, and the library string comes from the import
table. No guessing.

    python nid_harvest.py <ELF-or-dir> [more...] -o tools/nid_from_protos.json

Feed it the ELFs produced by unfself.py. Only matches whose NID is both imported and
named are written; the gazetteer names that are never imported are reported but not
emitted (probably-correct, but unvalidated here).
"""
from __future__ import annotations

import argparse
import glob
import io
import json
import os
import sys
import contextlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from elf_parser import ELFFile
from ppu_loader import parse_imports
from nid_database import compute_nid, get_default_db
import elf_symbols

# Names in .symtab that plausibly denote a firmware export. Game-internal symbols
# also start "cell"/"sce" occasionally, but a false name is harmless unless its NID
# collides with a real import NID for a different function -- a 1-in-2^32 event we
# additionally gate away by only emitting NIDs that are actually imported.
FW_PREFIXES = ("cell", "sce", "sys_", "sysPrx", "_cell", "_sys")


def iter_elfs(paths):
    for p in paths:
        if os.path.isdir(p):
            yield from sorted(glob.glob(os.path.join(p, "**", "*.elf"), recursive=True))
        else:
            yield p


def build_gazetteer(elfs) -> dict[int, set[str]]:
    """NID -> {names} for every firmware-shaped symbol across the ELFs."""
    gaz: dict[int, set[str]] = {}
    for p in elfs:
        try:
            with contextlib.redirect_stderr(io.StringIO()):
                syms = elf_symbols.load_symbols(p)
        except SystemExit:
            continue                       # stripped ELF: no symbols to harvest
        for s in syms:
            nm = s["name"]
            if nm.startswith(FW_PREFIXES):
                gaz.setdefault(compute_nid(nm), set()).add(nm)
    return gaz


def collect_imports(elfs) -> dict[int, set[str]]:
    """NID -> {library names} across every import table found."""
    imports: dict[int, set[str]] = {}
    for p in elfs:
        try:
            e = ELFFile(p); e.load()
            ims = parse_imports(e)
        except Exception:
            continue
        for im in ims:
            imports.setdefault(int(im["nid"], 16), set()).add(im["library"])
    return imports


def harvest(paths):
    elfs = list(iter_elfs(paths))
    gaz = build_gazetteer(elfs)
    imports = collect_imports(elfs)

    # Collisions would make a name non-definitive; report if any ever appear.
    collisions = {n: v for n, v in gaz.items() if len(v) > 1}

    db = get_default_db()
    already = sum(1 for n in imports if db.lookup_nid(n))

    # (library, name, nid) for every imported NID we can now name and the DB can't.
    out: dict[str, dict[str, str]] = {}
    named = 0
    for nid, libs in sorted(imports.items()):
        if db.lookup_nid(nid) or nid not in gaz:
            continue
        name = sorted(gaz[nid])[0]         # unique unless collisions (reported above)
        lib = sorted(libs)[0]
        out.setdefault(lib, {})[name] = f"0x{nid:08X}"
        named += 1

    stats = dict(elfs=len(elfs), import_nids=len(imports), gaz_nids=len(gaz),
                 already=already, named=named, collisions=len(collisions))
    return out, stats, collisions


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Name unresolved firmware NIDs from debug-build symbol tables")
    ap.add_argument("paths", nargs="+", help="Unwrapped ELF files or dirs (see unfself.py)")
    ap.add_argument("--output", "-o", help="Write validated NID map (nid_database load_json shape)")
    args = ap.parse_args()

    out, st, collisions = harvest(args.paths)

    total = st["import_nids"] or 1
    print(f"ELFs scanned            : {st['elfs']}", file=sys.stderr)
    print(f"distinct import NIDs    : {st['import_nids']}", file=sys.stderr)
    print(f"named symbols (gazetteer): {st['gaz_nids']} distinct NIDs", file=sys.stderr)
    print(f"resolved by current DB  : {st['already']}/{total} "
          f"({100*st['already']/total:.1f}%)", file=sys.stderr)
    print(f"newly named here        : {st['named']} "
          f"-> {100*(st['already']+st['named'])/total:.1f}%", file=sys.stderr)
    if collisions:
        print(f"WARNING: {len(collisions)} NID collisions in the gazetteer "
              f"(names non-definitive):", file=sys.stderr)
        for n, v in list(collisions.items())[:10]:
            print(f"  0x{n:08X}: {sorted(v)}", file=sys.stderr)

    if args.output:
        with open(args.output, "w", encoding="utf-8") as fh:
            json.dump(out, fh, indent=2, sort_keys=True)
            fh.write("\n")
        print(f"wrote {st['named']} entries across {len(out)} libraries to {args.output}",
              file=sys.stderr)
    else:
        json.dump(out, sys.stdout, indent=2, sort_keys=True)
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
