#!/usr/bin/env python3
"""
Wrap raw SPU instruction bytes in a minimal valid SPU ELF.

Produces a 32-bit big-endian ELF (EM_SPU=23) with a single PT_LOAD
executable segment, plus a section table containing `.text` and the two
string tables (`.shstrtab`, `.strtab`). If a symbol list is supplied,
an `.symtab` is emitted with STT_FUNC entries — useful both as a
meta-test for `find_spu_functions.py` (it can recover boundaries from
either symbols *or* brsl-target scanning) and as an artifact loadable by
RPCS3.

Usage as a library:
    from wrap_spu_elf import wrap
    elf_bytes = wrap(code_bytes, base=0x0, entry=0x0,
                     symbols=[{"name": "main", "addr": 0, "size": 24}])

Usage as a CLI:
    python wrap_spu_elf.py <raw.bin> --entry 0x0 --base 0x0
                           [--symbols name1:addr:size,name2:addr:size]
                           --out out.elf
"""

import argparse
import struct
import sys

# ELF constants
EM_SPU         = 23
ET_EXEC        = 2
PT_LOAD        = 1
PF_X, PF_W, PF_R = 1, 2, 4
SHT_NULL       = 0
SHT_PROGBITS   = 1
SHT_SYMTAB     = 2
SHT_STRTAB     = 3
SHF_ALLOC      = 2
SHF_EXECINSTR  = 4
STB_GLOBAL     = 1
STT_FUNC       = 2

EHDR_SIZE      = 52
PHDR_SIZE      = 32
SHDR_SIZE      = 40
SYM_SIZE       = 16


def _pack_ehdr(e_entry, e_phoff, e_shoff, e_phnum, e_shnum, e_shstrndx):
    ident  = b"\x7FELF"            # magic
    ident += b"\x01"                # EI_CLASS = 32-bit
    ident += b"\x02"                # EI_DATA  = big-endian
    ident += b"\x01"                # EI_VERSION
    ident += b"\x00" * 9            # OSABI + padding (9 bytes to reach 16)
    assert len(ident) == 16
    return ident + struct.pack(
        ">HHIIIIIHHHHHH",
        ET_EXEC, EM_SPU, 1, e_entry, e_phoff, e_shoff, 0,
        EHDR_SIZE, PHDR_SIZE, e_phnum, SHDR_SIZE, e_shnum, e_shstrndx)


def _pack_phdr(p_offset, p_vaddr, p_filesz, p_flags, p_align=16):
    return struct.pack(">IIIIIIII",
                       PT_LOAD, p_offset, p_vaddr, p_vaddr,
                       p_filesz, p_filesz, p_flags, p_align)


def _pack_shdr(name_off, sh_type, sh_flags, sh_addr, sh_offset,
               sh_size, sh_link=0, sh_info=0, sh_addralign=1, sh_entsize=0):
    return struct.pack(">IIIIIIIIII",
                       name_off, sh_type, sh_flags, sh_addr, sh_offset,
                       sh_size, sh_link, sh_info, sh_addralign, sh_entsize)


def _pack_sym(name_off, value, size, info, shndx):
    return struct.pack(">IIIBBH", name_off, value, size, info, 0, shndx)


def _strtab(strings):
    """Build a strtab. Returns (bytes, {name: offset})."""
    buf = bytearray(b"\x00")     # index 0 is always the empty string
    offs = {"": 0}
    for s in strings:
        if s in offs:
            continue
        offs[s] = len(buf)
        buf.extend(s.encode("ascii"))
        buf.append(0)
    return bytes(buf), offs


def wrap(code: bytes, *, base: int = 0, entry: int = 0,
         symbols: list[dict] | None = None) -> bytes:
    """Produce an SPU ELF wrapping `code` at virtual address `base`,
    starting execution at `entry` (default: same as base).

    `symbols` is an optional list of {"name": str, "addr": int, "size": int}.
    Addresses are virtual (i.e. include `base`).
    """
    symbols = symbols or []
    if entry == 0 and base != 0:
        entry = base

    # ---- layout ----
    # [ EHDR | PHDR | code | symtab? | strtab? | shstrtab | SHDRs ]
    phoff = EHDR_SIZE
    code_off = phoff + PHDR_SIZE
    code_off = (code_off + 15) & ~15            # align to 16
    pad_to_code = code_off - (phoff + PHDR_SIZE)
    cursor = code_off + len(code)

    # Symbol + symbol string tables (only if symbols supplied)
    sym_names = [s["name"] for s in symbols]
    strtab_buf, str_offs = _strtab(sym_names)

    if symbols:
        symtab_off = cursor
        # Emit a leading STN_UNDEF entry, then one per symbol.
        sym_bytes = bytearray(_pack_sym(0, 0, 0, 0, 0))
        for s in symbols:
            info = (STB_GLOBAL << 4) | STT_FUNC
            sym_bytes += _pack_sym(str_offs[s["name"]],
                                   s["addr"], s.get("size", 0),
                                   info, 1)        # shndx=1 (.text)
        cursor += len(sym_bytes)
        strtab_off = cursor
        cursor += len(strtab_buf)
    else:
        symtab_off = strtab_off = 0
        sym_bytes = b""

    # Section name string table
    section_names = [".text", ".shstrtab"]
    if symbols:
        section_names += [".symtab", ".strtab"]
    shstrtab_buf, shstr_offs = _strtab(section_names)
    shstrtab_off = cursor
    cursor += len(shstrtab_buf)

    # Section headers
    cursor = (cursor + 3) & ~3                  # mild alignment for SHDRs
    shdr_off = cursor

    # ---- build section header table ----
    shdrs = bytearray()
    # [0] SHT_NULL
    shdrs += _pack_shdr(0, SHT_NULL, 0, 0, 0, 0)
    # [1] .text
    shdrs += _pack_shdr(shstr_offs[".text"], SHT_PROGBITS,
                        SHF_ALLOC | SHF_EXECINSTR,
                        base, code_off, len(code), sh_addralign=16)
    next_idx = 2
    shstrtab_idx = next_idx
    # [N] .shstrtab
    shdrs += _pack_shdr(shstr_offs[".shstrtab"], SHT_STRTAB, 0,
                        0, shstrtab_off, len(shstrtab_buf))
    next_idx += 1
    if symbols:
        symtab_idx = next_idx
        strtab_idx = next_idx + 1
        # [N] .symtab    sh_link -> strtab idx;  sh_info -> 1 + index of last
        #                local symbol (we emit only globals after STN_UNDEF,
        #                so sh_info=1 is correct).
        shdrs += _pack_shdr(shstr_offs[".symtab"], SHT_SYMTAB, 0,
                            0, symtab_off, len(sym_bytes),
                            sh_link=strtab_idx, sh_info=1,
                            sh_addralign=4, sh_entsize=SYM_SIZE)
        # [N+1] .strtab
        shdrs += _pack_shdr(shstr_offs[".strtab"], SHT_STRTAB, 0,
                            0, strtab_off, len(strtab_buf))
    e_shnum = 2 + (3 if symbols else 1)

    # ---- assemble file ----
    out = bytearray()
    out += _pack_ehdr(entry, phoff, shdr_off, 1, e_shnum, shstrtab_idx)
    out += _pack_phdr(code_off, base, len(code), PF_X | PF_R)
    out += b"\x00" * pad_to_code
    out += code
    if symbols:
        out += sym_bytes
        out += strtab_buf
    out += shstrtab_buf
    # pad to shdr alignment
    pad = shdr_off - len(out)
    if pad > 0:
        out += b"\x00" * pad
    out += shdrs
    return bytes(out)


def _parse_symbols_arg(text: str) -> list[dict]:
    """`name:addr:size,name:addr:size` -> list of symbol dicts."""
    out = []
    if not text:
        return out
    for ent in text.split(","):
        parts = ent.split(":")
        if len(parts) != 3:
            raise SystemExit(f"--symbols: malformed entry {ent!r}")
        name, addr, size = parts
        out.append({"name": name.strip(),
                    "addr": int(addr, 0),
                    "size": int(size, 0)})
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", help="Raw SPU instruction bytes")
    ap.add_argument("--out", "-o", required=True)
    ap.add_argument("--base", type=lambda x: int(x, 0), default=0,
                    help="Virtual base of .text (default 0)")
    ap.add_argument("--entry", type=lambda x: int(x, 0), default=None,
                    help="Entry point (default: base)")
    ap.add_argument("--symbols", default="",
                    help='Comma-separated name:addr:size triples')
    args = ap.parse_args()

    with open(args.input, "rb") as f:
        code = f.read()
    elf = wrap(code,
               base=args.base,
               entry=args.entry if args.entry is not None else args.base,
               symbols=_parse_symbols_arg(args.symbols))
    with open(args.out, "wb") as f:
        f.write(elf)
    print(f"Wrote {args.out} ({len(elf)} bytes, "
          f"code {len(code)} B @ va 0x{args.base:X}, "
          f"entry 0x{(args.entry if args.entry is not None else args.base):X}, "
          f"{len(_parse_symbols_arg(args.symbols))} symbol(s))")


if __name__ == "__main__":
    sys.exit(main() or 0)
