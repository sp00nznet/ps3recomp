"""dwarf_line.py - resolve a code address to source file:line from debug-build DWARF.

When a recompiled proto crashes at guest address 0x004F4A98, .debug_line turns that into
`scene/scene.cpp:1204` -- the DWARF2 line-number program maps every instruction address
back to the source line it came from. Only useful on the unstripped debug builds, but for
those it makes a recomp crash address point straight at the offending source line.

    python dwarf_line.py EBOOT.elf 0x4F4A98 0x662100     # addr -> file:line
    python dwarf_line.py EBOOT.elf --dump | head          # full address->line table
"""
from __future__ import annotations

import argparse
import bisect
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from elf_parser import ELFFile
import dwarf_common as dw
from dwarf_common import uleb, sleb, cstr


def parse_one_unit(data: bytes, o: int):
    """Yield (address, file_path, line) rows for the line program starting at *o*.

    Driven by DW_AT_stmt_list offsets rather than by walking unit_length -- the SNC
    linker leaves padding between units, so `o += unit_length` lands in a gap.
    """
    if o + 4 <= len(data):
        unit_len = struct.unpack_from(">I", data, o)[0]
        end = o + 4 + unit_len
        version = struct.unpack_from(">H", data, o + 4)[0]
        header_len = struct.unpack_from(">I", data, o + 6)[0]
        prog_start = o + 10 + header_len
        p = o + 10
        min_inst = data[p]; p += 1
        if version >= 4:
            p += 1                              # max_ops_per_inst
        default_is_stmt = data[p]; p += 1
        line_base = struct.unpack_from(">b", data, p)[0]; p += 1
        line_range = data[p]; p += 1
        opcode_base = data[p]; p += 1
        std_lengths = list(data[p:p + opcode_base - 1]); p += opcode_base - 1

        include_dirs = [""]
        while data[p] != 0:
            s, p = cstr(data, p); include_dirs.append(s)
        p += 1
        files = [("", 0)]                       # (name, dir_index)
        while data[p] != 0:
            name, p = cstr(data, p)
            dir_idx, p = uleb(data, p)
            _mtime, p = uleb(data, p)
            _size, p = uleb(data, p)
            files.append((name, dir_idx))
        p += 1

        def filepath(idx):
            if idx >= len(files):
                return f"<file{idx}>"
            name, di = files[idx]
            d = include_dirs[di] if di < len(include_dirs) else ""
            return f"{d}/{name}" if d and not name.startswith(("/", "\\")) and ":" not in name else name

        # state machine
        addr = 0; file = 1; line = 1; p = prog_start
        addr_size = 4
        while p < end:
            op = data[p]; p += 1
            if op >= opcode_base:               # special opcode
                adj = op - opcode_base
                addr += (adj // line_range) * min_inst
                line += line_base + (adj % line_range)
                yield (addr, filepath(file), line)
            elif op == 0:                       # extended
                length, p = uleb(data, p)
                sub = data[p]
                if sub == 1:                    # end_sequence
                    yield (addr, None, 0)       # marks the end of covered range
                    addr = 0; file = 1; line = 1
                elif sub == 2:                  # set_address
                    addr = struct.unpack_from(">I" if addr_size == 4 else ">Q", data, p + 1)[0]
                p += length
            else:                               # standard opcode
                if op == 1:                     # copy
                    yield (addr, filepath(file), line)
                elif op == 2:                   # advance_pc
                    v, p = uleb(data, p); addr += v * min_inst
                elif op == 3:                   # advance_line
                    v, p = sleb(data, p); line += v
                elif op == 4:                   # set_file
                    file, p = uleb(data, p)
                elif op == 5:                   # set_column
                    _v, p = uleb(data, p)
                elif op == 8:                   # const_add_pc
                    addr += ((255 - opcode_base) // line_range) * min_inst
                elif op == 9:                   # fixed_advance_pc
                    addr += struct.unpack_from(">H", data, p)[0]; p += 2
                elif op in (6, 7, 10, 11):      # negate_stmt/basic_block/prologue/epilogue
                    pass
                else:                           # unknown std opcode: skip its uleb args
                    for _ in range(std_lengths[op - 1]):
                        _v, p = uleb(data, p)


def _stmt_list_offsets(elf) -> list[int]:
    """Every CU's DW_AT_stmt_list (its line-program start in .debug_line)."""
    dinfo = dw.open_dwarf(elf)
    if dinfo is None:
        return []
    offs = []
    for _off, tag, vals, _depth in dinfo.dies():
        if tag == dw.DW_TAG_compile_unit and dw.DW_AT_stmt_list in vals:
            offs.append(vals[dw.DW_AT_stmt_list])
    return offs


def build_table(elf):
    sec = {s.name_str: i for i, s in enumerate(elf.section_headers) if s.name_str}
    if ".debug_line" not in sec:
        raise SystemExit("no .debug_line (stripped / retail build?)")
    data = elf.get_section_data(sec[".debug_line"])
    rows = set()
    for o in _stmt_list_offsets(elf):
        try:
            rows.update(parse_one_unit(data, o))
        except (struct.error, IndexError):
            continue                            # one malformed unit shouldn't sink the rest
    # sort by address; end_sequence markers (file None) sort after real rows at the
    # same address so a real line start still wins there.
    return sorted(rows, key=lambda r: (r[0], r[1] is None, r[1] or "", r[2]))


def resolve(rows, addr):
    """The (addr, file, line) row covering *addr*, or None if it falls in a gap.

    A row covers [its address, the next row's address). end_sequence markers carry a
    None file, so if the covering row is one, *addr* is past the end of that sequence
    -- in a hole with no line info (e.g. a prebuilt lib linked without DWARF)."""
    addrs = [r[0] for r in rows]
    i = bisect.bisect_right(addrs, addr) - 1
    if i < 0 or rows[i][1] is None:
        return None
    return rows[i]


def main() -> int:
    ap = argparse.ArgumentParser(description="Resolve code addresses to source:line via DWARF")
    ap.add_argument("input", help="Unstripped ELF (run unfself.py on an EBOOT.BIN first)")
    ap.add_argument("addrs", nargs="*", help="Addresses to resolve (hex, e.g. 0x4F4A98)")
    ap.add_argument("--dump", action="store_true", help="Print the whole address->line table")
    args = ap.parse_args()

    elf = ELFFile(args.input); elf.load()
    rows = build_table(elf)
    print(f"[dwarf_line] {len(rows)} address rows", file=sys.stderr)

    if args.dump:
        for addr, fp, line in rows:
            if fp is not None:
                print(f"0x{addr:08X}  {fp}:{line}")
        return 0

    for a in args.addrs:
        addr = int(a, 16)
        r = resolve(rows, addr)
        if r:
            print(f"0x{addr:08X} -> {r[1]}:{r[2]}")
        else:
            print(f"0x{addr:08X} -> (no line info)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
