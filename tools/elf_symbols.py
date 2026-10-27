"""elf_symbols.py - pull the function symbol table out of an unstripped PS3 ELF.

Debug / prototype builds (see unfself.py) keep their .symtab. That gives the exact
start address, byte size and name of every function in the build -- ground truth the
recomp pipeline otherwise has to *guess* at with .opd seeding and prologue scanning.

Two uses:

  1. Seed the lifter. The emitted JSON is the same shape find_functions.py already
     accepts, so it drops straight in:

         python elf_symbols.py EBOOT.elf -o syms.json
         python find_functions.py EBOOT.elf --seed-json syms.json -o funcs.json

  2. Score the detector. On a title where symbols exist, the symtab tells us how good
     .opd + prologue scanning actually is -- and the answer carries to the stripped
     retail titles where we have no symbols at all:

         python elf_symbols.py EBOOT.elf --score funcs.json

On PPC64 a function has two symbols: `foo` is the OPD descriptor (in .opd) and `.foo`
is the entry point (in .text). We want the code addresses, so a leading '.' is stripped
from the name and descriptor-only symbols are dropped.
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from elf_parser import ELFFile  # noqa: E402

SHT_SYMTAB = 2
STT_FUNC = 2


def load_symbols(path: str) -> list[dict]:
    """Every STT_FUNC symbol with a real code address, as {start,end,size,name}."""
    elf = ELFFile(path)
    elf.load()
    if elf.self_header is not None:
        raise SystemExit(
            f"{path} is still a SELF wrapper. Unwrap it first:\n"
            f"    python unfself.py {path} -o {os.path.splitext(path)[0]}.elf")

    symtab_idx = next((i for i, sh in enumerate(elf.section_headers)
                       if sh.sh_type == SHT_SYMTAB), None)
    if symtab_idx is None:
        raise SystemExit(f"{path} has no .symtab -- it is stripped (a retail build?)")

    sym_sh = elf.section_headers[symtab_idx]
    symdata = elf.get_section_data(symtab_idx)
    strdata = elf.get_section_data(sym_sh.sh_link)
    entsize = sym_sh.sh_entsize or 24

    def name_at(off: int) -> str:
        end = strdata.find(b"\0", off)
        return strdata[off:end if end != -1 else None].decode("utf-8", "replace")

    # Executable ranges, so we keep code symbols and drop the .opd descriptors.
    exec_ranges = [(ph.p_vaddr, ph.p_vaddr + ph.p_filesz)
                   for ph in elf.program_headers
                   if ph.p_type == 1 and (ph.p_flags & 1)]

    def in_text(a: int) -> bool:
        return any(lo <= a < hi for lo, hi in exec_ranges)

    out: dict[int, dict] = {}
    for i in range(len(symdata) // entsize):
        st_name, st_info, _other, _shndx, st_value, st_size = struct.unpack_from(
            ">IBBHQQ", symdata, i * entsize)
        if (st_info & 0xF) != STT_FUNC or not st_value:
            continue
        if exec_ranges and not in_text(st_value):
            continue                      # OPD descriptor, not the entry point
        nm = name_at(st_name).lstrip(".")
        if not nm:
            continue
        # Same address twice: keep the one that carries a size.
        prev = out.get(st_value)
        if prev and prev["size"] >= st_size:
            continue
        out[st_value] = {
            "start": f"0x{st_value:08X}",
            "end": f"0x{st_value + st_size:08X}",
            "size": st_size,
            "name": nm,
        }

    return [out[a] for a in sorted(out)]


def _starts(items) -> set[int]:
    """Function start addresses from either tool's JSON (bare list or {functions:[]})."""
    if isinstance(items, dict):
        items = items.get("functions", [])
    got = set()
    for it in items:
        v = it if not isinstance(it, dict) else it.get("start", it.get("addr"))
        if v is None:
            continue
        got.add(int(v, 16) if isinstance(v, str) else int(v))
    return got


def score(truth: list[dict], detected_path: str) -> None:
    detected = _starts(json.load(open(detected_path, encoding="utf-8")))
    # Only score inside the range the symbols actually cover; a detector finding
    # functions outside it is not necessarily wrong.
    truth_by_start = {int(f["start"], 16): f for f in truth}
    real = set(truth_by_start)
    lo, hi = min(real), max(real)
    det = {a for a in detected if lo <= a <= hi}

    hit = real & det
    missed = real - det
    spurious = det - real

    print(f"ground truth (.symtab) : {len(real)} functions  "
          f"[0x{lo:08X}..0x{hi:08X}]")
    print(f"detected in that range : {len(det)}")
    print()
    print(f"  recall    : {len(hit)}/{len(real)} = {100 * len(hit) / len(real):.1f}%  "
          f"(missed {len(missed)})")
    if det:
        print(f"  precision : {len(hit)}/{len(det)} = {100 * len(hit) / len(det):.1f}%  "
              f"(spurious {len(spurious)})")

    # A start that is real but got swallowed into a preceding function is the
    # failure mode that silently corrupts a lift, so call it out separately.
    if missed:
        print("\n  sample of missed functions (these lift as part of a neighbour):")
        for a in sorted(missed)[:12]:
            f = truth_by_start[a]
            print(f"    0x{a:08X}  size={f['size']:<6} {f['name'][:64]}")


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Extract / score function symbols from an unstripped PS3 ELF")
    ap.add_argument("input", help="Unstripped ELF (run unfself.py on an EBOOT.BIN first)")
    ap.add_argument("--output", "-o", help="Write symbols as JSON (find_functions seed shape)")
    ap.add_argument("--score", metavar="DETECTED.JSON",
                    help="Score a find_functions.py --output file against these symbols")
    args = ap.parse_args()

    syms = load_symbols(args.input)
    print(f"[elf_symbols] {len(syms)} function symbols", file=sys.stderr)

    if args.output:
        with open(args.output, "w", encoding="utf-8") as fh:
            json.dump(syms, fh, indent=1)
        print(f"[elf_symbols] wrote {args.output}", file=sys.stderr)

    if args.score:
        score(syms, args.score)

    if not args.output and not args.score:
        for s in syms[:50]:
            print(f"{s['start']}  size={s['size']:<7} {s['name']}")
        if len(syms) > 50:
            print(f"... and {len(syms) - 50} more")
    return 0


if __name__ == "__main__":
    sys.exit(main())
