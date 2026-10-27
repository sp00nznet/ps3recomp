"""
ingest_native.py -- build a units.json from ps3recomp's OWN tool outputs.

So the PS3 side of the harness doesn't strictly require Ghidra/IDA: we can feed
it the JSON that the existing pipeline already produces:

  * find_functions.py --json   ->  function ranges, calls, is_leaf, stack_size
  * ppu_disasm.py --json       ->  per-instruction {addr, hex, mnemonic, operands}
  * (optional) Ghidra strings.json (addr -> {val, refs:[func_addrs]}) to fill in
    string_refs, which the raw disassembler can't resolve on its own.

The instruction stream is bucketed into the function ranges to compute mnemonic
histograms, instruction counts, and immediate (const) references.

CLI:
  python ingest_native.py \
    --functions functions.json \
    --instructions instructions.json \
    [--strings strings.json] \
    --binary EBOOT.ELF -o ps3.units.json
"""

from __future__ import annotations

import argparse
import json
import re
from bisect import bisect_right

from schema import Module, Unit, ARCH_CELL, PLATFORM_PS3, parse_addr, normalize_string


# immediates in disasm operands, e.g. "r3, r4, 0x1234" or "-0x10(r1)"
_IMM = re.compile(r"(?<![\w.])-?0x[0-9a-fA-F]+")
CONST_MIN = 0x1000


def _load(path):
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)


def _instr_list(raw):
    # ppu_disasm.py may emit a bare list or {"instructions": [...]}
    if isinstance(raw, dict):
        return raw.get("instructions", raw.get("insns", []))
    return raw


def _func_list(raw):
    if isinstance(raw, dict):
        return raw.get("functions", [])
    return raw


def build(functions_path, instructions_path, strings_path, binary, source):
    funcs = _func_list(_load(functions_path))
    insns = _instr_list(_load(instructions_path))

    # sort instructions by addr for range bucketing
    insns = sorted(
        ({"addr": parse_addr(i["addr"]),
          "mnemonic": (i.get("mnemonic") or "").lower(),
          "operands": i.get("operands", "")} for i in insns),
        key=lambda x: x["addr"],
    )
    insn_addrs = [i["addr"] for i in insns]

    # optional: invert ghidra strings.json (string addr -> referencing funcs)
    str_by_func: dict[int, list[str]] = {}
    if strings_path:
        for s in _load(strings_path):
            val = normalize_string(s.get("val", ""))
            for ref in s.get("refs", []):
                str_by_func.setdefault(parse_addr(ref), []).append(val)

    units = []
    for f in funcs:
        start = parse_addr(f["start"])
        end = parse_addr(f.get("end", f["start"]))
        # slice instructions in [start, end)
        lo = bisect_right(insn_addrs, start - 1)
        hi = bisect_right(insn_addrs, end - 1)
        body = insns[lo:hi]

        mnem: dict[str, int] = {}
        consts: list[int] = []
        for ins in body:
            m = ins["mnemonic"]
            if m:
                mnem[m] = mnem.get(m, 0) + 1
            for tok in _IMM.findall(ins["operands"]):
                v = int(tok, 16)
                if abs(v) >= CONST_MIN:
                    consts.append(v & 0xFFFFFFFF)

        units.append(Unit(
            addr=start,
            size=int(f.get("size", end - start)),
            name=f.get("name"),
            insn_count=len(body),
            is_leaf=bool(f.get("is_leaf", len(f.get("calls", [])) == 0)),
            stack_size=int(f.get("stack_size", 0)),
            calls=[parse_addr(c) for c in f.get("calls", [])],
            imports=list(f.get("imports", [])),
            string_refs=str_by_func.get(start, []),
            const_refs=consts,
            mnemonic_hist=mnem,
        ))

    return Module(
        platform=PLATFORM_PS3, binary=binary, arch=ARCH_CELL,
        source=source, units=units,
    )


def main() -> None:
    ap = argparse.ArgumentParser(description="Build units.json from ps3recomp tool outputs")
    ap.add_argument("--functions", required=True, help="find_functions.py --json output")
    ap.add_argument("--instructions", required=True, help="ppu_disasm.py --json output")
    ap.add_argument("--strings", help="optional Ghidra strings.json for string_refs")
    ap.add_argument("--binary", default="EBOOT.ELF")
    ap.add_argument("--source", default="ppu_native")
    ap.add_argument("-o", "--out", required=True)
    args = ap.parse_args()

    mod = build(args.functions, args.instructions, args.strings, args.binary, args.source)
    mod.save(args.out)
    print(f"ingest_native: wrote {len(mod.units)} units -> {args.out}")


if __name__ == "__main__":
    main()
