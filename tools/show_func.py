#!/usr/bin/env python3
"""
Print a lifted function's C code and/or its original PowerPC disassembly.

The lifted chunks are too big to browse comfortably in an editor, so this
pulls out just the function you care about.

Usage:
    py -3 tools\\show_func.py 937840            # lifted C
    py -3 tools\\show_func.py 0x00937840 --asm  # original PowerPC
    py -3 tools\\show_func.py 937840 --both
    py -3 tools\\show_func.py 937840 --max 0    # no length cap

Defaults assume the repo layout (recomp\\ chunks, game\\EBOOT.elf,
functions.json); override with --dir / --elf / --functions.
"""

import argparse
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, HERE)


def norm_addr(text: str) -> int:
    return int(text, 16)


def find_lifted(addr: int, recomp_dir: str, max_lines: int) -> bool:
    name = f"func_{addr:08X}"
    pat = re.compile(rf"^(/\* .* \*/\n)?void {name}\(ppu_context\* ctx\) \{{",
                     re.M)
    paths = sorted(glob.glob(os.path.join(recomp_dir, "ppu_recomp_*.cpp")))
    single = os.path.join(recomp_dir, "ppu_recomp.c")
    if os.path.exists(single):
        paths.append(single)
    for path in paths:
        text = open(path, errors="replace").read()
        m = pat.search(text)
        if not m:
            continue
        # function body ends at the first '}' on its own line
        end = text.find("\n}", m.start())
        body = text[m.start():end + 2] if end != -1 else text[m.start():m.start() + 8000]
        lines = body.splitlines()
        print(f"--- {name} in {os.path.basename(path)} "
              f"({len(lines)} lines) ---")
        shown = lines if max_lines <= 0 else lines[:max_lines]
        print("\n".join(shown))
        if max_lines > 0 and len(lines) > max_lines:
            print(f"... ({len(lines) - max_lines} more lines; use --max 0 for all)")
        return True
    print(f"{name}: not found in {recomp_dir} "
          f"(not lifted, or it's a no-op stub in the last chunk)")
    return False


def find_asm(addr: int, elf_path: str, functions_path: str, max_lines: int) -> None:
    from ppu_disasm import disassemble_bytes

    end = None
    if os.path.exists(functions_path):
        for e in json.load(open(functions_path)):
            if int(str(e["start"]), 0) == addr:
                end = int(str(e["end"]), 0)
                break
    size = (end - addr) if end else 0x200

    data = open(elf_path, "rb").read()
    # text segment: vaddr 0x10000 at file offset 0 (PS3 EBOOT layout)
    off = addr - 0x10000
    if off < 0 or off + size > len(data):
        print(f"address 0x{addr:08X} is outside the text segment")
        return
    insns = disassemble_bytes(data[off:off + size], addr)
    print(f"--- 0x{addr:08X} original PowerPC "
          f"({len(insns)} instructions{'' if end else ', end unknown, showing 0x200 bytes'}) ---")
    shown = insns if max_lines <= 0 else insns[:max_lines]
    for i in shown:
        print(i)
    if max_lines > 0 and len(insns) > max_lines:
        print(f"... ({len(insns) - max_lines} more; use --max 0 for all)")


def main() -> None:
    ap = argparse.ArgumentParser(description="Show a lifted function / its disassembly")
    ap.add_argument("addr", help="function address, hex (with or without 0x)")
    ap.add_argument("--asm", action="store_true", help="show original PowerPC instead")
    ap.add_argument("--both", action="store_true", help="show both")
    ap.add_argument("--max", type=int, default=120, help="max lines to print (0 = all)")
    ap.add_argument("--dir", default=os.path.join(ROOT, "recomp"))
    ap.add_argument("--elf", default=os.path.join(ROOT, "game", "EBOOT.elf"))
    ap.add_argument("--functions", default=os.path.join(ROOT, "functions.json"))
    args = ap.parse_args()

    addr = norm_addr(args.addr)
    if args.both or not args.asm:
        find_lifted(addr, args.dir, args.max)
    if args.both or args.asm:
        find_asm(addr, args.elf, args.functions, args.max)


if __name__ == "__main__":
    main()
