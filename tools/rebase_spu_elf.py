#!/usr/bin/env python3
"""Rebase a PIC SPU overlay ELF (SCE .fixup scheme) to its runtime load base.

FMOD's SPU mixer imports plugin ELFs (linked at va 0x80) and loads them at a
runtime LS base, applying .fixup itself in LS. To lift such a plugin, the
lifter needs an ELF whose addresses ARE the runtime addresses (so relative
branches disassemble to the right targets). This shifts e_entry, p_vaddr,
p_paddr and sh_addr by the base delta, and adds the delta to words in the
pointer-carrying data sections (.ctors/.dtors/.data.rel.ro/.data) that look
like in-image addresses -- the same net effect as the loader's .fixup pass
for the words the lifted code's callers will read from LS.

Usage: rebase_spu_elf.py in.elf out.elf 0x250B0
"""
import struct, sys

def be32(b, o): return struct.unpack_from('>I', b, o)[0]
def wbe32(b, o, v): struct.pack_into('>I', b, o, v & 0xFFFFFFFF)
def be16(b, o): return struct.unpack_from('>H', b, o)[0]

def main():
    src, dst, base = sys.argv[1], sys.argv[2], int(sys.argv[3], 0)
    b = bytearray(open(src, 'rb').read())
    assert b[:4] == b'\x7fELF'
    e_entry, e_phoff, e_shoff = be32(b,0x18), be32(b,0x1C), be32(b,0x20)
    phentsz, phnum = be16(b,0x2A), be16(b,0x2C)
    shentsz, shnum, shstrndx = be16(b,0x2E), be16(b,0x30), be16(b,0x32)
    # image extent at link addresses (for the address-looking heuristic)
    lo, hi = 0xFFFFFFFF, 0
    for i in range(phnum):
        ph = e_phoff + i*phentsz
        if be32(b, ph) != 1: continue
        va, msz = be32(b, ph+8), be32(b, ph+0x14)
        lo, hi = min(lo, va), max(hi, va+msz)
    wbe32(b, 0x18, e_entry + base)
    for i in range(phnum):
        ph = e_phoff + i*phentsz
        if be32(b, ph) != 1: continue
        wbe32(b, ph+8,  be32(b, ph+8)  + base)   # p_vaddr
        wbe32(b, ph+12, be32(b, ph+12) + base)   # p_paddr
    # section string table for names
    sh0 = e_shoff + shstrndx*shentsz
    strs = bytes(b[be32(b,sh0+16):be32(b,sh0+16)+be32(b,sh0+20)])
    def shname(i):
        sh = e_shoff + i*shentsz
        n = be32(b, sh)
        end = strs.find(b'\0', n)
        return strs[n:end].decode('ascii','replace')
    PTR_SECTIONS = {'.ctors', '.dtors', '.data.rel.ro', '.data'}
    for i in range(shnum):
        sh = e_shoff + i*shentsz
        addr = be32(b, sh+12)
        if addr:
            wbe32(b, sh+12, addr + base)         # sh_addr
        nm = shname(i)
        if nm in PTR_SECTIONS:
            off, size = be32(b, sh+16), be32(b, sh+20)
            for o in range(off, off+size-3, 4):
                v = be32(b, o)
                if lo <= v < hi and (v & 3) == 0 and v != 0:
                    wbe32(b, o, v + base)
    open(dst, 'wb').write(b)
    print(f"rebased {src} -> {dst} (+{base:#x}, image {lo:#x}..{hi:#x})")

if __name__ == '__main__':
    main()
