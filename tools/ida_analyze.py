#!/usr/bin/env python3
"""ida_analyze.py -- headless IDA Pro function export for ps3recomp.

The IDA-side sibling of ghidra_analyze.py: opens a (decrypted) PS3 PPU ELF in
IDA via idalib (no GUI), runs auto-analysis, and writes a functions.json in the
*same* schema Ghidra's exporter uses:

    functions.json   [{addr, size, name, thunk}]

so the harness cross-check (tier 6) and `find_functions --seed-json` consume an
IDA export and a Ghidra export interchangeably -- two independent oracles for
the same question ("which function starts did our detector miss?").

MUST run under the Python that idalib is installed in (IDA Pro 9.1 -> Python
3.11). The harness invokes it with that interpreter; if you run it by hand:

    py -3.11 tools/ida_analyze.py game/EBOOT.elf -o ida_out

idalib locks the input's .idb while open, so don't point two analyses at the
same file at once.
"""
import argparse
import json
import os
import sys

import idapro  # MUST be imported before any ida_* module

import ida_funcs
import ida_auto
import idautils


def main():
    ap = argparse.ArgumentParser(description="Headless IDA function export for ps3recomp")
    ap.add_argument("elf", help="Path to decrypted EBOOT.elf")
    ap.add_argument("-o", "--output", default="ida_out", help="Output dir for functions.json")
    args = ap.parse_args()

    elf = os.path.abspath(args.elf)
    if not os.path.isfile(elf):
        print(f"ERROR: ELF not found: {elf}", file=sys.stderr)
        sys.exit(1)
    out_dir = os.path.abspath(args.output)
    os.makedirs(out_dir, exist_ok=True)

    print(f"[ida] opening {elf}", flush=True)
    if idapro.open_database(elf, run_auto_analysis=True) != 0:
        print("ERROR: idapro.open_database failed", file=sys.stderr)
        sys.exit(2)
    try:
        ida_auto.auto_wait()  # ensure analysis settled
        funcs = []
        for ea in idautils.Functions():
            f = ida_funcs.get_func(ea)
            if not f:
                continue
            funcs.append({
                "addr": f"0x{ea:08X}",
                "size": int(f.end_ea - f.start_ea),
                "name": ida_funcs.get_func_name(ea) or "",
                "thunk": bool(f.flags & ida_funcs.FUNC_THUNK),
            })
        path = os.path.join(out_dir, "functions.json")
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(funcs, fh)
        print(f"[ida] functions: {len(funcs)} -> {path}", flush=True)
    finally:
        idapro.close_database(save=False)


if __name__ == "__main__":
    main()
