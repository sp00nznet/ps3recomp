"""dwarf_abi.py - authoritative firmware ABI from debug-build DWARF.

Unstripped PS3 debug builds (see unfself.py) compiled the SDK headers with debug info,
so their DWARF carries the real layout of every Cell*/Sce* struct the game used and the
prototype of every function it declared. That is ground truth for the ABI bug class this
toolkit keeps hand-fixing (struct member offsets, field widths, arg counts) -- e.g. the
CellFsStat 52-byte packing and the CellGcmContextData "callback at +0xC" fixes both fall
straight out of the DWARF.

    python dwarf_abi.py EBOOT.elf --struct CellFsStat CellGcmContextData
    python dwarf_abi.py EBOOT.elf --func   cellFsRead cellGcmSetFlip
    python dwarf_abi.py EBOOT.elf --diff-headers        # diff struct layouts vs repo *.h
    python dwarf_abi.py EBOOT.elf --json -o abi.json    # dump everything

--diff-headers parses the toolkit's own `typedef struct { ... }` blocks and reports any
member whose DWARF offset disagrees -- the layout bugs, found automatically.
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from elf_parser import ELFFile
import dwarf_common as dw


# ---------------------------------------------------------------------------
# Extraction
# ---------------------------------------------------------------------------

def _member_offset(v):
    """DW_AT_data_member_location is either a udata offset (DWARF4-ish) or a
    location expression block: DW_OP_plus_uconst <uleb>."""
    if isinstance(v, int):
        return v
    if v and v[0] == 0x23:                 # DW_OP_plus_uconst
        return dw.uleb(v, 1)[0]
    return None


def extract(dinfo: dw.DwarfInfo):
    """Return (structs, funcs, types).

    structs: name -> {"size": int, "members": [(offset, name, type_off)]}
    funcs:   name -> {"return": type_off, "params": [type_off]}
    types:   die_offset -> a resolvable type record (for naming member/param types)
    """
    structs: dict[str, dict] = {}
    funcs: dict[str, dict] = {}
    types: dict[int, dict] = {}

    # A shallow stack of "currently open aggregate" per depth, so members/params
    # attach to the right parent.
    open_at: dict[int, dict | None] = {}

    for off, tag, vals, depth in dinfo.dies():
        # Record every type DIE so member/param type references resolve later.
        if tag in (dw.DW_TAG_base_type, dw.DW_TAG_typedef, dw.DW_TAG_pointer_type,
                   dw.DW_TAG_const_type, dw.DW_TAG_volatile_type, dw.DW_TAG_structure_type,
                   dw.DW_TAG_union_type, dw.DW_TAG_enumeration_type, dw.DW_TAG_array_type):
            types[off] = {"tag": tag, "name": vals.get(dw.DW_AT_name),
                          "ref": vals.get(dw.DW_AT_type), "size": vals.get(dw.DW_AT_byte_size)}

        parent = open_at.get(depth - 1)

        if tag in (dw.DW_TAG_structure_type, dw.DW_TAG_union_type):
            name = vals.get(dw.DW_AT_name)
            size = vals.get(dw.DW_AT_byte_size)
            rec = None
            # Keep the richest definition (declarations carry no size / no members).
            if name and size is not None:
                rec = structs.get(name)
                if rec is None or rec.get("size") is None:
                    rec = {"size": size, "members": []}
                    structs[name] = rec
                elif rec["members"]:
                    rec = None        # already have a populated definition; ignore dup
                else:
                    rec["size"] = size
            open_at[depth] = rec

        elif tag == dw.DW_TAG_subprogram:
            name = vals.get(dw.DW_AT_name)
            rec = None
            if name and name not in funcs:
                rec = {"return": vals.get(dw.DW_AT_type), "params": []}
                funcs[name] = rec
            open_at[depth] = rec

        elif tag == dw.DW_TAG_member and parent is not None and "members" in parent:
            parent["members"].append(
                (_member_offset(vals.get(dw.DW_AT_data_member_location)),
                 vals.get(dw.DW_AT_name), vals.get(dw.DW_AT_type)))
            open_at[depth] = None

        elif tag == dw.DW_TAG_formal_parameter and parent is not None and "params" in parent:
            parent["params"].append(vals.get(dw.DW_AT_type))
            open_at[depth] = None

        else:
            open_at[depth] = None

    return structs, funcs, types


def type_name(types: dict, off, depth=0) -> str:
    """Resolve a DW_AT_type reference to a C-ish type string."""
    if off is None:
        return "void"
    if depth > 12 or off not in types:
        return "?"
    t = types[off]
    tag = t["tag"]
    if tag == dw.DW_TAG_pointer_type:
        return type_name(types, t["ref"], depth + 1) + "*"
    if tag == dw.DW_TAG_const_type:
        return "const " + type_name(types, t["ref"], depth + 1)
    if tag == dw.DW_TAG_volatile_type:
        return type_name(types, t["ref"], depth + 1)
    if tag == dw.DW_TAG_array_type:
        return type_name(types, t["ref"], depth + 1) + "[]"
    return t["name"] or ("struct?" if tag == dw.DW_TAG_structure_type else "?")


# ---------------------------------------------------------------------------
# Diff against the repo's own struct definitions
# ---------------------------------------------------------------------------

_STRUCT_RE = re.compile(
    r"typedef\s+struct\s+(?:\w+\s*)?\{(?P<body>[^}]*)\}\s*(?P<name>\w+)\s*;", re.S)
_MEMBER_RE = re.compile(r"^\s*([A-Za-z_][\w\s\*]*?)\s+([A-Za-z_]\w*)\s*(\[[^\]]*\])?\s*;", re.M)

# width of the leaf C types the headers use, for computing expected offsets
_WIDTH = {"u8": 1, "s8": 1, "char": 1, "u16": 2, "s16": 2, "be_t<u16>": 2,
          "u32": 4, "s32": 4, "int": 4, "float": 4, "f32": 4, "be_t<u32>": 4,
          "u64": 8, "s64": 8, "f64": 8, "double": 8, "be_t<u64>": 8}


def _repo_structs(repo_root: str) -> dict[str, list[tuple[str, str, bool]]]:
    """name -> [(c_type, member_name, is_array)] parsed from the toolkit's headers."""
    out: dict[str, list[tuple[str, str, bool]]] = {}
    for path in glob.glob(os.path.join(repo_root, "**", "*.h"), recursive=True):
        try:
            txt = open(path, encoding="latin1").read()
        except OSError:
            continue
        for m in _STRUCT_RE.finditer(txt):
            members = []
            for mm in _MEMBER_RE.finditer(m.group("body")):
                ctype = mm.group(1).strip().replace("unsigned ", "u").replace("signed ", "s")
                members.append((ctype, mm.group(2), bool(mm.group(3))))
            if members:
                out[m.group("name")] = members
    return out


def diff_headers(structs: dict, repo_root: str):
    """Return (mismatches, uncertain). Only structs whose every member is a known
    scalar (no arrays, no nested aggregates) are checked, so a reported mismatch is
    a real repo-header-vs-ABI disagreement -- not an artifact of the header regex
    being unable to size an array or nested struct."""
    repo = _repo_structs(repo_root)
    mismatches: list[str] = []
    uncertain: list[str] = []
    for name, layout in sorted(structs.items()):
        if name not in repo:
            continue
        # bail on anything the scalar model can't size exactly
        if any(is_arr or ctype not in _WIDTH for ctype, _, is_arr in repo[name]):
            uncertain.append(name)
            continue
        dwarf_off = {mn: off for off, mn, _ in layout["members"] if off is not None}
        off = 0
        bad = []
        for ctype, mn, _ in repo[name]:
            w = _WIDTH[ctype]
            if off % w:                     # natural alignment
                off += w - (off % w)
            d = dwarf_off.get(mn)
            if d is not None and d != off:
                bad.append(f"{mn}: repo +0x{off:X} vs DWARF +0x{d:X}")
            off += w
        if bad:
            mismatches.append(f"{name}: " + "; ".join(bad))
    return mismatches, uncertain


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description="Extract firmware ABI (structs, signatures) "
                                             "from debug-build DWARF")
    ap.add_argument("input", help="Unstripped ELF (run unfself.py on an EBOOT.BIN first)")
    ap.add_argument("--struct", nargs="*", metavar="NAME", help="Print these struct layouts")
    ap.add_argument("--func", nargs="*", metavar="NAME", help="Print these function signatures")
    ap.add_argument("--diff-headers", action="store_true",
                    help="Diff struct layouts against the repo's *.h and report mismatches")
    ap.add_argument("--repo-root", help="Repo root for --diff-headers (default: this repo)")
    ap.add_argument("--json", action="store_true", help="Emit everything as JSON")
    ap.add_argument("--output", "-o", help="Write JSON to a file")
    args = ap.parse_args()

    elf = ELFFile(args.input); elf.load()
    if elf.self_header is not None:
        raise SystemExit(f"{args.input} is still a SELF; unwrap with unfself.py first")
    dinfo = dw.open_dwarf(elf)
    if dinfo is None:
        raise SystemExit(f"{args.input} has no DWARF (stripped / retail build?)")

    structs, funcs, types = extract(dinfo)
    print(f"[dwarf_abi] {len(structs)} structs, {len(funcs)} functions", file=sys.stderr)

    if args.diff_headers:
        root = args.repo_root or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        mismatches, uncertain = diff_headers(structs, root)
        if not mismatches:
            print("no scalar-struct layout mismatches vs repo headers")
        for r in mismatches:
            print(r)
        if uncertain:
            print(f"\n{len(uncertain)} struct(s) have arrays/nested members - not "
                  f"auto-checkable; verify by hand with --struct:")
            print("  " + " ".join(uncertain))
        return 0

    if args.struct is not None:
        for name in (args.struct or sorted(structs)):
            s = structs.get(name)
            if not s:
                print(f"{name}: not in DWARF"); continue
            print(f"\n=== {name}  ({s['size']} bytes) ===")
            for off, mn, tref in s["members"]:
                offs = f"+0x{off:<4X}" if off is not None else "+????"
                print(f"  {offs} {type_name(types, tref):24} {mn}")
        return 0

    if args.func is not None:
        for name in (args.func or []):
            f = funcs.get(name)
            if not f:
                print(f"{name}: not in DWARF"); continue
            ps = ", ".join(type_name(types, p) for p in f["params"]) or "void"
            print(f"{type_name(types, f['return'])} {name}({ps})")
        return 0

    if args.json or args.output:
        out = {
            "structs": {n: {"size": s["size"],
                            "members": [{"offset": o, "name": mn,
                                         "type": type_name(types, t)}
                                        for o, mn, t in s["members"]]}
                        for n, s in structs.items()},
            "functions": {n: {"return": type_name(types, f["return"]),
                              "params": [type_name(types, p) for p in f["params"]]}
                          for n, f in funcs.items()},
        }
        if args.output:
            json.dump(out, open(args.output, "w", encoding="utf-8"), indent=1)
            print(f"[dwarf_abi] wrote {args.output}", file=sys.stderr)
        else:
            json.dump(out, sys.stdout, indent=1)
            print()
        return 0

    # default: summary of the firmware-shaped names present
    fw = sorted(n for n in structs if n.startswith(("Cell", "Sce", "cell", "sce")))
    print(f"firmware structs with layouts: {len(fw)}")
    for n in fw[:40]:
        print(f"  {n} ({structs[n]['size']} B, {len(structs[n]['members'])} members)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
