"""Self-check for unfself.py + elf_symbols.py.

No game binary needed: build a tiny unstripped PPC64 ELF, wrap it as a compressed
debug fSELF exactly the way the SDK does (key_revision 0x8000, zlib segment, non-alloc
tail stored at a constant offset delta), then assert unfself reconstructs the ELF
byte-for-byte and elf_symbols recovers the one function symbol.

Run: python tools/test_unfself.py
"""
import os
import struct
import sys
import tempfile
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from unfself import FSelf
import elf_symbols


def build_elf() -> bytes:
    """A minimal ELF64-BE with one exec PT_LOAD and a .symtab naming one function."""
    code = b"\x4e\x80\x00\x20" * 8          # 8x `blr`, our "function" body
    vaddr = 0x10000

    strtab = b"\x00" + b".foo\x00"           # index 1 = ".foo"
    name_off = 1
    # one real symbol: STT_FUNC (info=2), value=vaddr, size=len(code)
    symtab = struct.pack(">IBBHQQ", 0, 0, 0, 0, 0, 0)  # index 0 is reserved/null
    symtab += struct.pack(">IBBHQQ", name_off, 2, 0, 1, vaddr, len(code))
    shstr = b"\x00.text\x00.symtab\x00.strtab\x00.shstrtab\x00"
    def sh_name(s):
        return shstr.index(b"\x00" + s + b"\x00") + 1

    # Layout: [ehdr 64][phdr 56][code][symtab][strtab][shstrtab][shdr]
    off_ph = 64
    off_code = off_ph + 56
    off_sym = off_code + len(code)
    off_str = off_sym + len(symtab)
    off_shstr = off_str + len(strtab)
    off_sh = off_shstr + len(shstr)

    ehdr = bytearray(64)
    ehdr[0:4] = b"\x7fELF"
    ehdr[4] = 2      # ELF64
    ehdr[5] = 2      # big-endian
    ehdr[6] = 1
    struct.pack_into(">H", ehdr, 16, 2)       # e_type ET_EXEC
    struct.pack_into(">H", ehdr, 18, 21)      # e_machine PPC64
    struct.pack_into(">I", ehdr, 20, 1)
    struct.pack_into(">Q", ehdr, 24, vaddr)   # e_entry
    struct.pack_into(">Q", ehdr, 32, off_ph)  # e_phoff
    struct.pack_into(">Q", ehdr, 40, off_sh)  # e_shoff
    struct.pack_into(">H", ehdr, 52, 64)      # e_ehsize
    struct.pack_into(">H", ehdr, 54, 56)      # e_phentsize
    struct.pack_into(">H", ehdr, 56, 1)       # e_phnum
    struct.pack_into(">H", ehdr, 58, 64)      # e_shentsize
    struct.pack_into(">H", ehdr, 60, 5)       # e_shnum
    struct.pack_into(">H", ehdr, 62, 4)       # e_shstrndx

    phdr = struct.pack(">IIQQQQQQ", 1, 5, off_code, vaddr, vaddr,
                       len(code), len(code), 0x10000)  # PT_LOAD, R+X

    def shdr(name, typ, flags, addr, off, size, link, entsz):
        return struct.pack(">IIQQQQIIQQ", name, typ, flags, addr, off, size,
                           link, 0, 0, entsz)

    shtab = b"".join([
        shdr(0, 0, 0, 0, 0, 0, 0, 0),                                  # NULL
        shdr(sh_name(b".text"), 1, 0x6, vaddr, off_code, len(code), 0, 0),
        shdr(sh_name(b".symtab"), 2, 0, 0, off_sym, len(symtab), 3, 24),  # link->strtab(3)
        shdr(sh_name(b".strtab"), 3, 0, 0, off_str, len(strtab), 0, 0),
        shdr(sh_name(b".shstrtab"), 3, 0, 0, off_shstr, len(shstr), 0, 0),
    ])
    return bytes(ehdr) + phdr + code + symtab + strtab + shstr + shtab


def build_fself(elf: bytes) -> bytes:
    """Wrap `elf` as a compressed debug fSELF (key_revision 0x8000)."""
    # ELF layout we depend on.
    e_phoff, e_shoff = struct.unpack(">QQ", elf[0x20:0x30])
    p_offset, p_vaddr = struct.unpack(">QQ", elf[e_phoff + 8:e_phoff + 24])
    p_filesz = struct.unpack(">Q", elf[e_phoff + 32:e_phoff + 40])[0]
    tail_start = p_offset + p_filesz          # everything after the segment is verbatim

    seg_blob = zlib.compress(elf[p_offset:p_offset + p_filesz])

    # SELF layout: [sce 0x20][ext 0x40][elfhdr 64][phdr 56][seginfo 32][seg_blob][tail]
    off_elf = 0x60
    off_phdr = off_elf + 64
    off_seg = off_phdr + 56
    off_blob = off_seg + 32
    off_tail = off_blob + len(seg_blob)
    delta = off_tail - tail_start
    shdr_off_self = e_shoff + delta

    out = bytearray()
    out += struct.pack(">IIHHIQQ", 0x53434500, 2, 0x8000, 1, 0x60, off_elf, len(elf))
    out += struct.pack(">8Q", 3, off_elf, off_elf, off_phdr, shdr_off_self, off_seg, 0, 0)
    assert len(out) == off_elf
    out += elf[0:64]                          # elf header copy
    out += elf[e_phoff:e_phoff + 56]          # phdr copy
    out += struct.pack(">QQIIII", off_blob, len(seg_blob), 2, 0, 0, 0)  # seginfo: zlib
    out += seg_blob
    out += elf[tail_start:]                    # symtab/strtab/shstrtab/shdr, verbatim
    return bytes(out)


def main() -> int:
    elf = build_elf()
    fself = build_fself(elf)

    # 1. round-trip: rebuilt ELF must be byte-identical.
    rebuilt = FSelf(fself).to_elf()
    assert rebuilt == elf, (
        f"round-trip mismatch: {len(rebuilt)} vs {len(elf)} bytes, "
        f"first diff at {next((i for i in range(min(len(rebuilt), len(elf))) if rebuilt[i] != elf[i]), 'n/a')}"
    )

    # 2. a retail SELF (non-0x8000 key) must be refused, not silently mis-decoded.
    retail = bytearray(fself)
    struct.pack_into(">H", retail, 8, 0x0001)
    try:
        FSelf(bytes(retail)).to_elf()
        assert False, "retail SELF was not rejected"
    except Exception as e:
        assert "retail" in str(e).lower() or "key" in str(e).lower(), e

    # 3. elf_symbols recovers the one function from the rebuilt ELF.
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "t.elf")
        open(p, "wb").write(rebuilt)
        syms = elf_symbols.load_symbols(p)
    assert len(syms) == 1, syms
    assert syms[0]["name"] == "foo", syms          # leading '.' stripped
    assert syms[0]["start"] == "0x00010000", syms
    assert syms[0]["size"] == 32, syms

    print("ok: fSELF round-trip, retail rejection, and symbol recovery all pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
