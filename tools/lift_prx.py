#!/usr/bin/env python3
"""Relocate a decrypted PS3 PRX module for static lifting (firmware LLE).

Takes a decrypted .prx (ELF64 BE, type 0xFFA4), places its LOAD segments at a
chosen guest base, applies the SCE_PPURELA relocations, and emits:

  <name>_image.bin       relocated flat image (load at --base in the runner)
  <name>_functions.json  function seeds for ppu_lifter (--raw --base mode):
                         every exported function's code address, region-bounded;
                         the lifter's discovery pass finds interior functions
  <name>_exports.json    {library: {fnid_hex: opd_ea_hex}} for import binding
  <name>_imports.json    the module's own imports: {library: {fnid_hex:
                         stub_slot_ea_hex}} — slots the runner patches to its
                         HLE bridge descriptors

Relocation semantics follow the PS3 PRX format as documented by RPCS3's
ppu_loader (PPUModule.cpp): 24-byte entries {offset u64, pad u16,
index_value u8, index_addr u8, type u32, ptr u64}; target =
seg[index_addr].base + offset; value = (index_value == 0xFF ? 0 :
seg[index_value].base) + ptr. Types: 1 = ADDR32, 4 = ADDR16_LO, 5 = ADDR16_HI,
6 = ADDR16_HA, 10 = REL24, 44 = REL64, 57 = ADDR16_LO_DS.

Usage:
    py -3 tools/lift_prx.py libsre.prx --base 0x02000000 --output recomp_prx/
"""

import argparse
import json
import os
import struct


def be16(b, o):
    return struct.unpack(">H", b[o:o + 2])[0]


def be32(b, o):
    return struct.unpack(">I", b[o:o + 4])[0]


def be64(b, o):
    return struct.unpack(">Q", b[o:o + 8])[0]


def cstr(b, o):
    return b[o:b.index(0, o)].decode("ascii", "replace")


class Prx:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        d = self.data
        if d[:4] != b"\x7fELF" or d[4] != 2 or d[5] != 2:
            raise SystemExit("not an ELF64 big-endian file (decrypted .prx expected)")
        if be16(d, 0x12) != 0x15:
            raise SystemExit("not a PPC64 module")
        e_phoff = be64(d, 0x20)
        phnum = be16(d, 0x38)
        self.loads = []        # (p_offset, p_vaddr, p_filesz, p_memsz)
        self.rela = None       # (p_offset, p_filesz)
        for i in range(phnum):
            o = e_phoff + i * 0x38
            p_type = be32(d, o)
            p_offset = be64(d, o + 0x08)
            p_vaddr = be64(d, o + 0x10)
            p_paddr = be64(d, o + 0x18)
            p_filesz = be64(d, o + 0x20)
            p_memsz = be64(d, o + 0x28)
            if p_type == 1:
                self.loads.append((p_offset, p_vaddr, p_filesz, p_memsz))
            elif p_type == 0x700000A4:
                self.rela = (p_offset, p_filesz)
            if p_type == 1 and not hasattr(self, "modinfo_off"):
                # module info: paddr of the first LOAD segment = file offset
                self.modinfo_off = p_paddr

    def build_image(self, base):
        """Place LOAD segments at base+vaddr, zero-fill bss, relocate."""
        top = max(v + m for _, v, _, m in self.loads)
        img = bytearray(top)
        seg_bases = []
        for off, vaddr, filesz, memsz in self.loads:
            img[vaddr:vaddr + filesz] = self.data[off:off + filesz]
            seg_bases.append(base + vaddr)

        ro, rsz = self.rela
        counts = {}
        for i in range(rsz // 24):
            o = ro + i * 24
            offset = be64(self.data, o)
            index_value = self.data[o + 10]
            index_addr = self.data[o + 11]
            rtype = be32(self.data, o + 12)
            ptr = be64(self.data, o + 16)

            target = (seg_bases[index_addr] - base) + offset   # image-relative
            value = (0 if index_value == 0xFF else seg_bases[index_value]) + ptr
            counts[rtype] = counts.get(rtype, 0) + 1

            if rtype == 1:        # ADDR32
                img[target:target + 4] = struct.pack(">I", value & 0xFFFFFFFF)
            elif rtype == 4:      # ADDR16_LO
                img[target:target + 2] = struct.pack(">H", value & 0xFFFF)
            elif rtype == 5:      # ADDR16_HI
                img[target:target + 2] = struct.pack(">H", (value >> 16) & 0xFFFF)
            elif rtype == 6:      # ADDR16_HA
                img[target:target + 2] = struct.pack(
                    ">H", ((value + 0x8000) >> 16) & 0xFFFF)
            elif rtype == 44:     # REL64 (rare)
                img[target:target + 8] = struct.pack(">Q", value & (2**64 - 1))
            else:
                raise SystemExit(f"unhandled relocation type {rtype} "
                                 f"(entry {i}) — extend lift_prx.py")
        return bytes(img), counts

    def parse_modinfo(self, img, base):
        """Exports/imports from the RELOCATED image (tables hold real EAs)."""
        d = self.data
        mi = self.modinfo_off
        name = d[mi + 4:mi + 32].split(b"\0")[0].decode("ascii", "replace")
        toc = be32(d, mi + 0x20)
        exp_start, exp_end = be32(d, mi + 0x24), be32(d, mi + 0x28)
        imp_start, imp_end = be32(d, mi + 0x2C), be32(d, mi + 0x30)

        def img32(va):
            return be32(img, va)

        exports = {}
        o = exp_start
        while o < exp_end:
            size = img[o]
            nfunc = be16(img, o + 6)
            libname_ptr = img32(o + 16)
            nid_tbl = img32(o + 20)
            add_tbl = img32(o + 24)
            lib = (cstr(img, libname_ptr - base) if libname_ptr else "!syslib")
            if nfunc:
                entries = {}
                for i in range(nfunc):
                    fnid = img32(nid_tbl - base + i * 4)
                    opd_ea = img32(add_tbl - base + i * 4)
                    entries[f"0x{fnid:08X}"] = f"0x{opd_ea:08X}"
                exports[lib] = entries
            o += size if size else 0x1C

        imports = {}
        o = imp_start
        while o < imp_end:
            size = img[o]
            nfunc = be16(img, o + 6)
            libname_ptr = img32(o + 16)
            nid_tbl = img32(o + 20)
            stub_tbl = img32(o + 24)
            lib = cstr(img, libname_ptr - base) if libname_ptr else "?"
            entries = {}
            for i in range(nfunc):
                fnid = img32(nid_tbl - base + i * 4)
                slot_ea = img32(stub_tbl - base + i * 4)
                entries[f"0x{fnid:08X}"] = f"0x{slot_ea:08X}"
            imports[lib] = entries
            o += size if size else 0x2C

        return name, toc, exports, imports

    def text_extent(self):
        """(vaddr, size) of the first (text) LOAD segment."""
        off, vaddr, filesz, memsz = self.loads[0]
        return vaddr, filesz


def main():
    ap = argparse.ArgumentParser(description="Relocate a PRX for static lifting")
    ap.add_argument("input", help="decrypted .prx")
    ap.add_argument("--base", type=lambda x: int(x, 0), default=0x02000000)
    ap.add_argument("--output", "-o", default=".")
    args = ap.parse_args()

    prx = Prx(args.input)
    img, counts = prx.build_image(args.base)
    name, toc, exports, imports = prx.parse_modinfo(img, args.base)
    stem = os.path.splitext(os.path.basename(args.input))[0]
    os.makedirs(args.output, exist_ok=True)

    # function seeds: every exported function's CODE address (read from its
    # relocated OPD), region-bounded by the next seed / end of text. Interior
    # functions are found by the lifter's discovery pass (bl targets, tail
    # entries); unreachable tail code inside a region lifts harmlessly.
    text_va, text_sz = prx.text_extent()
    code_addrs = set()
    opd_map = {}
    for lib, entries in exports.items():
        for fnid, opd in entries.items():
            opd_ea = int(opd, 16)
            code = be32(img, opd_ea - args.base)
            code_addrs.add(code)
            opd_map[opd] = f"0x{code:08X}"

    # ALSO seed every internal OPD: scan the image for {code, toc} pairs
    # with toc == the module TOC and code inside text. Functions referenced
    # only through data OPDs (e.g. SPURS passes its PPU handler-thread
    # entries to sys_ppu_thread_create) are invisible to the lifter's
    # branch discovery — without these seeds they lift as nothing and the
    # threads exit instantly (observed live with pxd::SpursHdlr0/1).
    text_lo = args.base + text_va
    text_hi = args.base + text_va + text_sz
    toc_ea = toc + args.base
    n_opd_seeds = 0
    for off in range(0, len(img) - 7, 4):
        if be32(img, off + 4) != toc_ea:
            continue
        code = be32(img, off)
        if text_lo <= code < text_hi and (code & 3) == 0 and code not in code_addrs:
            code_addrs.add(code)
            n_opd_seeds += 1
    seeds = sorted(code_addrs)
    text_end = args.base + text_va + text_sz
    funcs = []
    for i, s in enumerate(seeds):
        e = seeds[i + 1] if i + 1 < len(seeds) else text_end
        funcs.append({"start": f"0x{s:08X}", "end": f"0x{e:08X}"})

    img_path = os.path.join(args.output, f"{stem}_image.bin")
    open(img_path, "wb").write(img)
    json.dump(funcs, open(os.path.join(args.output, f"{stem}_functions.json"), "w"),
              indent=1)
    json.dump({"module": name, "base": f"0x{args.base:08X}", "toc": f"0x{toc + args.base:08X}",
               "exports": exports, "opd_to_code": opd_map},
              open(os.path.join(args.output, f"{stem}_exports.json"), "w"), indent=1)
    json.dump(imports, open(os.path.join(args.output, f"{stem}_imports.json"), "w"),
              indent=1)

    n_exp = sum(len(v) for v in exports.values())
    n_imp = sum(len(v) for v in imports.values())
    print(f"module '{name}'  base 0x{args.base:08X}  TOC 0x{toc + args.base:08X}")
    print(f"image {len(img)} bytes -> {img_path}")
    print(f"relocations applied: " + ", ".join(f"type{k} x{v}" for k, v in sorted(counts.items())))
    print(f"exports: {n_exp} funcs in {len(exports)} libs; imports: {n_imp} funcs "
          f"in {len(imports)} libs")
    print(f"function seeds: {len(funcs)} ({n_opd_seeds} from internal OPD scan; "
          f"region-bounded; lifter discovery fills interiors)")


if __name__ == "__main__":
    main()
