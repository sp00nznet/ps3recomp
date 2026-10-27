"""unfself.py - rebuild the original ELF from a debug/"fake" SELF (fSELF).

Debug and prototype builds ship EBOOT.BIN as an fSELF: key_revision 0x8000, which
means the payload is *not encrypted* -- segments are stored plain or zlib-deflated.
No keys are needed, only an inflate.

Two things make these worth unwrapping:

  * The PT_LOAD segments give the real .text/.data, so the whole recomp pipeline
    (find_functions -> ppu_disasm -> ppu_lifter) runs on them directly.
  * SDK debug builds are *unstripped*: the ELF keeps .symtab/.strtab and the DWARF
    .debug_* sections. Those live outside any PT_LOAD, so the SELF stores them
    verbatim after the (possibly compressed) segments -- at a constant offset delta
    from where the section headers say they belong. Recovering them yields exact
    address+size+name for every function in the build.

Usage:
    python unfself.py EBOOT.BIN -o EBOOT.elf
    python unfself.py EBOOT.BIN --info        # inspect, write nothing
"""
from __future__ import annotations

import argparse
import struct
import sys
import zlib

SELF_MAGIC = 0x53434500          # "SCE\0"
KEY_REV_DEBUG = 0x8000           # fSELF marker: payload is not encrypted
HDR_TYPE_SELF = 1

SHT_NOBITS = 8
SHF_ALLOC = 0x2

COMPRESSED = 2                   # segment_info.compressed: 1 = plain, 2 = zlib


class NotADebugSelf(Exception):
    pass


def _u(fmt, d, off):
    return struct.unpack_from(">" + fmt, d, off)


class FSelf:
    def __init__(self, data: bytes):
        self.d = data
        if len(data) < 0x70:
            raise NotADebugSelf("file too small to be a SELF")

        magic, version, self.key_rev, hdr_type, _meta, self.header_len, self.elf_filesize = \
            _u("IIHHIQQ", data, 0)
        if magic != SELF_MAGIC:
            raise NotADebugSelf("not a SELF (no SCE\\0 magic) -- already a plain ELF?")
        if hdr_type != HDR_TYPE_SELF:
            raise NotADebugSelf(f"SCE header type {hdr_type} is not a SELF")

        (_ext_ver, self.appinfo_off, self.elf_off, self.phdr_off,
         self.shdr_off, self.seginfo_off, _ver_off, _ctrl_off) = _u("8Q", data, 0x20)

        if data[self.elf_off:self.elf_off + 4] != b"\x7fELF":
            raise NotADebugSelf("no ELF header at the offset the SELF advertises")

        # ELF header fields we need (ELF64 big-endian).
        self.e_phoff, self.e_shoff = _u("QQ", data, self.elf_off + 0x20)
        (self.e_phentsize, self.e_phnum, self.e_shentsize,
         self.e_shnum, self.e_shstrndx) = _u("5H", data, self.elf_off + 0x36)

    @property
    def is_debug(self) -> bool:
        return self.key_rev == KEY_REV_DEBUG

    def segments(self):
        """(index, file_off, file_size, compressed, p_offset, p_filesz) per phdr."""
        for i in range(self.e_phnum):
            o = self.seginfo_off + i * 0x20
            f_off, f_size, comp, _u1, _u2, _enc = _u("QQIIII", self.d, o)
            po = self.phdr_off + i * 0x38
            p_offset, _p_vaddr, _p_paddr, p_filesz = _u("4Q", self.d, po + 8)
            yield i, f_off, f_size, comp, p_offset, p_filesz

    def sections(self):
        """(index, name_off, type, flags, addr, off, size) per shdr, read from the
        SELF's copy of the section header table."""
        for i in range(self.e_shnum):
            o = self.shdr_off + i * self.e_shentsize
            nameo, typ, flags, addr, off, size = _u("IIQQQQ", self.d, o)
            yield i, nameo, typ, flags, addr, off, size

    @property
    def tail_delta(self) -> int:
        """Offset delta between the original ELF's file layout and this SELF's.

        The SELF carries its own copy of the section header table, so the shift
        between "where the ELF says its shdrs live" and "where they actually are
        in this file" gives the delta for every other non-alloc byte too -- the
        tail (symtab/strtab/DWARF) is stored verbatim, just moved.
        """
        return self.shdr_off - self.e_shoff

    def to_elf(self) -> bytes:
        if not self.is_debug:
            raise NotADebugSelf(
                f"key_revision 0x{self.key_rev:04X} != 0x{KEY_REV_DEBUG:04X}: this is a "
                "retail (encrypted) SELF. Decrypting it needs keys this project does "
                "not ship. Only debug/prototype fSELFs can be unwrapped."
            )

        d = self.d
        out = bytearray(self.elf_filesize)

        # ELF header + program headers, from the SELF's own copies.
        out[0:64] = d[self.elf_off:self.elf_off + 64]
        phsz = self.e_phnum * self.e_phentsize
        out[self.e_phoff:self.e_phoff + phsz] = d[self.phdr_off:self.phdr_off + phsz]

        # PT_LOAD (and friends) payloads.
        for i, f_off, f_size, comp, p_offset, p_filesz in self.segments():
            if not f_off or not f_size:
                # Not separately stored -- it lives inside a segment already written
                # (PT_TLS and the SCE PPU segments overlap a PT_LOAD).
                continue
            blob = d[f_off:f_off + f_size]
            if comp == COMPRESSED:
                try:
                    blob = zlib.decompress(blob)
                except zlib.error as exc:
                    raise NotADebugSelf(
                        f"segment {i} failed to inflate ({exc}) -- payload looks "
                        "encrypted despite the debug key revision"
                    ) from exc
            if len(blob) != p_filesz:
                raise NotADebugSelf(
                    f"segment {i}: got {len(blob)} bytes, phdr wants {p_filesz}"
                )
            out[p_offset:p_offset + p_filesz] = blob

        # Non-alloc sections (.symtab/.strtab/.shstrtab/.debug_*) are not in any
        # segment; they sit in the tail at a constant delta.
        delta = self.tail_delta
        recovered = 0
        for i, _nameo, typ, flags, _addr, off, size in self.sections():
            if typ == SHT_NOBITS or not size or (flags & SHF_ALLOC):
                continue
            src = off + delta
            if src < 0 or src + size > len(d):
                print(f"[unfself] warn: section {i} tail range "
                      f"0x{src:X}+0x{size:X} is outside the file; skipped",
                      file=sys.stderr)
                continue
            out[off:off + size] = d[src:src + size]
            recovered += 1

        # Finally the section header table itself.
        if self.e_shnum:
            shsz = self.e_shnum * self.e_shentsize
            out[self.e_shoff:self.e_shoff + shsz] = d[self.shdr_off:self.shdr_off + shsz]

        print(f"[unfself] {self.e_phnum} segments, {recovered} non-alloc sections recovered",
              file=sys.stderr)
        return bytes(out)


def _describe(f: FSelf) -> None:
    print(f"key_revision : 0x{f.key_rev:04X} "
          f"({'debug/fSELF - no keys needed' if f.is_debug else 'RETAIL - encrypted'})")
    print(f"elf_filesize : {f.elf_filesize}")
    print(f"phnum/shnum  : {f.e_phnum} / {f.e_shnum}")
    print(f"tail delta   : {f.tail_delta}")
    print("\n idx  file_off    file_size   comp  p_offset    p_filesz")
    for i, f_off, f_size, comp, p_off, p_fsz in f.segments():
        kind = "zlib" if comp == COMPRESSED else "plain"
        print(f"  {i}   0x{f_off:08X}  0x{f_size:08X}  {kind:5} 0x{p_off:08X}  0x{p_fsz:08X}")


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Rebuild the original ELF from a debug/prototype fSELF (no keys)")
    ap.add_argument("input", help="EBOOT.BIN / *.self / *.sprx (debug fSELF)")
    ap.add_argument("--output", "-o", help="Where to write the rebuilt ELF")
    ap.add_argument("--info", action="store_true",
                    help="Describe the SELF and exit without writing")
    args = ap.parse_args()

    data = open(args.input, "rb").read()
    try:
        f = FSelf(data)
    except NotADebugSelf as exc:
        print(f"error: {args.input}: {exc}", file=sys.stderr)
        return 1

    if args.info:
        _describe(f)
        return 0

    if not args.output:
        print("error: --output is required (or use --info)", file=sys.stderr)
        return 2

    try:
        elf = f.to_elf()
    except NotADebugSelf as exc:
        print(f"error: {args.input}: {exc}", file=sys.stderr)
        return 1

    with open(args.output, "wb") as fh:
        fh.write(elf)
    print(f"[unfself] wrote {args.output} ({len(elf)} bytes)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
