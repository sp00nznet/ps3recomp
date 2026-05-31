#!/usr/bin/env python3
"""
PS3 PPU loader (stage 1: image + OPD + TOC + function table).

Turns a decrypted PS3 PPU ELF (ET_EXEC EBOOT, or an ET_DYN PRX) into the data
a static recompilation needs to actually run:

  * the guest memory image  -- which PT_LOAD segments go where in `vm_base`,
    including zero-filled BSS (memsz > filesz);
  * the OPD function table   -- PS3 uses 8-byte function descriptors
    {u32 func, u32 toc}; a function POINTER in the binary is an OPD address,
    so every indirect call must resolve OPD -> code entry. The OPD also gives
    us the authoritative list of function entry points (far better than
    heuristic scanning) and the module TOC (r2);
  * the entry point          -- e_entry is itself an OPD address; we resolve it
    to (code entry, toc).

Outputs (to --output dir):
  * <name>.functions.json  -- [{start,end,toc,opd}] for ppu_lifter.py --functions
  * <name>.image.json      -- segment manifest {vaddr,filesz,memsz,flags,file_offset}
  * <name>.loader.json     -- {entry, toc, opd_base, opd_count, ...}

This stage is game-agnostic. Relocations (PRX/ET_DYN) and import-stub
resolution (NID -> HLE) are the next stages; ET_EXEC EBOOTs have no
relocations (fixed load address) so the image is loaded verbatim.

Usage:
    python ppu_loader.py <elf> [--output dir]
"""

import argparse
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from elf_parser import ELFFile, PT_LOAD


def code_ranges(elf):
    """[(lo, hi)] of executable PT_LOAD segments (flag bit 0 = X)."""
    rs = []
    for ph in elf.program_headers:
        if ph.p_type == PT_LOAD and (ph.p_flags & 0x1) and ph.p_filesz:
            rs.append((ph.p_vaddr, ph.p_vaddr + ph.p_filesz))
    return rs


def in_ranges(addr, rs):
    return any(lo <= addr < hi for lo, hi in rs)


def read_vaddr(elf, vaddr, size):
    """Read `size` bytes at guest vaddr out of the loaded segments."""
    for i, ph in enumerate(elf.program_headers):
        if ph.p_type == PT_LOAD and ph.p_filesz and ph.p_vaddr <= vaddr < ph.p_vaddr + ph.p_filesz:
            data = elf.get_segment_data(i)
            off = vaddr - ph.p_vaddr
            if off + size <= len(data):
                return data[off:off + size]
    return None


def find_opd(elf, entry):
    """Locate the OPD section: the one section that contains e_entry.
    Section names are usually stripped on retail EBOOTs, so we go by address.
    Returns (opd_vaddr, opd_size) or (None, None)."""
    best = None
    for s in getattr(elf, "section_headers", []) or []:
        a, sz = getattr(s, "sh_addr", 0), getattr(s, "sh_size", 0)
        if sz and a <= entry < a + sz:
            # prefer the tightest section containing entry
            if best is None or sz < best[1]:
                best = (a, sz)
    return best if best else (None, None)


def parse_opd(elf, opd_va, opd_size, crange):
    """Walk 8-byte {func,toc} descriptors. Stop when an entry stops looking
    like a valid descriptor (func not in code, or toc inconsistent)."""
    blob = read_vaddr(elf, opd_va, opd_size)
    if blob is None:
        return [], 0
    entries = []
    toc_hist = {}
    for off in range(0, len(blob) - 7, 8):
        func, toc = struct.unpack_from(">II", blob, off)
        if func == 0 and toc == 0:
            continue
        if not in_ranges(func, crange):
            continue
        entries.append({"opd": opd_va + off, "func": func, "toc": toc})
        toc_hist[toc] = toc_hist.get(toc, 0) + 1
    module_toc = max(toc_hist, key=toc_hist.get) if toc_hist else 0
    return entries, module_toc


def _u32(elf, va):
    b = read_vaddr(elf, va, 4)
    return struct.unpack(">I", b)[0] if b else None


def _cstr(elf, va, maxlen=64):
    out = b""
    for _ in range(maxlen):
        c = read_vaddr(elf, va, 1)
        if not c or c == b"\0":
            break
        out += c
        va += 1
    return out.decode("latin1")


def parse_imports(elf):
    """Parse ET_EXEC firmware imports via the proc_prx_param (PT 0x60000002) ->
    libstub table. Returns [{library, nid, stub}]. PRX (ET_DYN) modules use the
    module-info import table instead (handled by elf_parser); not covered here."""
    pp = None
    for ph in elf.program_headers:
        if getattr(ph, "p_type", 0) == 0x60000002:
            pp = ph.p_vaddr
            break
    if pp is None:
        return []
    hdr = read_vaddr(elf, pp, 0x20)
    if not hdr or struct.unpack_from(">I", hdr, 4)[0] != 0x1B434CEC:
        return []
    libstub_start = struct.unpack_from(">I", hdr, 24)[0]
    libstub_end = struct.unpack_from(">I", hdr, 28)[0]
    imports = []
    va = libstub_start
    while va < libstub_end:
        s_size = read_vaddr(elf, va, 1)[0]
        if s_size == 0:
            break
        nfunc = struct.unpack(">H", read_vaddr(elf, va + 6, 2))[0]
        modname = _cstr(elf, _u32(elf, va + 0x10) or 0)
        nid_tbl = _u32(elf, va + 0x14)
        stub_tbl = _u32(elf, va + 0x18)
        for i in range(nfunc):
            nid = _u32(elf, nid_tbl + i * 4)
            stub = _u32(elf, stub_tbl + i * 4)
            if nid is not None and stub is not None:
                imports.append({"library": modname, "nid": hex(nid), "stub": hex(stub)})
        va += s_size
    return imports


def main():
    ap = argparse.ArgumentParser(description="PS3 PPU loader (image + OPD + TOC + functions)")
    ap.add_argument("input")
    ap.add_argument("--output", "-o", default="ppu_loader_out")
    args = ap.parse_args()

    elf = ELFFile(args.input)
    elf.load()
    h = elf.elf_header
    entry = h.e_entry
    crange = code_ranges(elf)

    # Resolve the entry OPD -> (code entry, toc).
    entry_code, entry_toc = entry, 0
    od = read_vaddr(elf, entry, 8)
    if od and in_ranges(struct.unpack(">I", od[:4])[0], crange):
        entry_code, entry_toc = struct.unpack(">II", od)

    opd_va, opd_size = find_opd(elf, entry)
    opd_entries, module_toc = ([], 0)
    if opd_va is not None:
        opd_entries, module_toc = parse_opd(elf, opd_va, opd_size, crange)
    if not entry_toc:
        entry_toc = module_toc

    # Function boundaries from OPD: unique func addrs, end = next start in the
    # same code segment (the lifter refines via blr).
    func_addrs = sorted({e["func"] for e in opd_entries})
    func_toc = {e["func"]: e["toc"] for e in opd_entries}
    opd_of = {e["func"]: e["opd"] for e in opd_entries}
    funcs = []
    for i, start in enumerate(func_addrs):
        end = None
        for lo, hi in crange:
            if lo <= start < hi:
                end = hi
                break
        if i + 1 < len(func_addrs) and end and func_addrs[i + 1] <= end:
            end = func_addrs[i + 1]
        funcs.append({"start": hex(start), "end": hex(end or start),
                      "toc": hex(func_toc[start]), "opd": hex(opd_of[start])})

    image = []
    for ph in elf.program_headers:
        if ph.p_type == PT_LOAD and ph.p_memsz:
            image.append({"vaddr": hex(ph.p_vaddr), "filesz": hex(ph.p_filesz),
                          "memsz": hex(ph.p_memsz), "flags": ph.p_flags,
                          "file_offset": hex(ph.p_offset)})

    os.makedirs(args.output, exist_ok=True)
    name = os.path.splitext(os.path.basename(args.input))[0]
    with open(os.path.join(args.output, name + ".functions.json"), "w") as f:
        json.dump(funcs, f, indent=1)
    with open(os.path.join(args.output, name + ".image.json"), "w") as f:
        json.dump(image, f, indent=1)
    loader = {"entry_opd": hex(entry), "entry_code": hex(entry_code),
              "entry_toc": hex(entry_toc), "module_toc": hex(module_toc),
              "opd_base": hex(opd_va) if opd_va else None,
              "opd_size": hex(opd_size) if opd_size else None,
              "opd_count": len(opd_entries), "function_count": len(func_addrs),
              "e_type": h.e_type, "load_segments": len(image)}
    with open(os.path.join(args.output, name + ".loader.json"), "w") as f:
        json.dump(loader, f, indent=1)

    imports = parse_imports(elf)
    with open(os.path.join(args.output, name + ".imports.json"), "w") as f:
        json.dump(imports, f, indent=1)
    by_lib = {}
    for im in imports:
        by_lib[im["library"]] = by_lib.get(im["library"], 0) + 1

    print(f"=== PPU loader: {args.input} ===")
    print(f"e_type           : {h.e_type} ({'EXEC' if h.e_type==2 else 'DYN' if h.e_type==3 else '?'})")
    print(f"entry (OPD)      : {hex(entry)} -> code {hex(entry_code)}, toc {hex(entry_toc)}")
    print(f"module TOC (r2)  : {hex(module_toc)}")
    print(f"OPD region       : {hex(opd_va) if opd_va else '?'} size {hex(opd_size) if opd_size else '?'}")
    print(f"OPD descriptors  : {len(opd_entries)}")
    print(f"unique functions : {len(func_addrs)}")
    print(f"load segments    : {len(image)}")
    for seg in image:
        bss = int(seg['memsz'], 16) - int(seg['filesz'], 16)
        print(f"  seg vaddr={seg['vaddr']:>10} filesz={seg['filesz']:>9} "
              f"memsz={seg['memsz']:>9} bss={hex(bss):>9} flags={seg['flags']:#x}")
    print(f"firmware imports : {len(imports)} across {len(by_lib)} libraries")
    for lib in sorted(by_lib, key=lambda k: -by_lib[k]):
        print(f"    {lib:<24} {by_lib[lib]:3} funcs")
    print(f"wrote {name}.functions.json / .image.json / .loader.json / .imports.json -> {args.output}/")


if __name__ == "__main__":
    main()
