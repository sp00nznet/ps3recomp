"""Bounded ELF32 big-endian SPU validation and embedded-image discovery.

The file extent and FNV offset basis intentionally match spu_workload.c,
including non-LOAD segments, non-NOBITS sections, and section headers.
"""
import struct

MAGIC = b'\x7fELF'
LS_SIZE = 256 * 1024


class InvalidSPUELF(ValueError):
    pass


def validate_spu_elf(buf, offset=0):
    available = len(buf) - offset

    def check(condition, message):
        if not condition:
            raise InvalidSPUELF(message)

    def extent(start, size, label):
        check(0 <= start <= available and 0 <= size <= available - start,
              label + ' exceeds available file bytes')
        return start + size

    extent(0, 52, 'ELF header')
    fields = struct.unpack_from('>16sHHIIIIIHHHHHH', buf, offset)
    ident, kind, machine, version, entry, phoff, shoff, _, ehsize, phes, phnum, shes, shnum, shstr = fields
    check(ident[:7] == MAGIC + b'\x01\x02\x01' and machine == 23,
          'expected ELF32 big-endian EM_SPU version 1')
    check(version == 1 and ehsize == 52, 'invalid ELF header version/size')
    check(kind in (2, 3), 'expected executable or shared SPU ELF')
    check(phnum > 0 and phes == 32 and phoff >= 52, 'invalid program header table')
    extent(phoff, phes * phnum, 'program header table')
    check(shnum != 0xffff and phnum != 0xffff and shstr != 0xffff,
          'extended ELF numbering is unsupported')
    if shnum:
        check(shes == 40 and shoff >= 52, 'invalid section header table')
        extent(shoff, shes * shnum, 'section header table')
        check(shstr == 0 or shstr < shnum, 'invalid section-name table index')
    else:
        check(shoff == 0 and shstr == 0, 'section table offset/index without sections')
    end = shoff + shes * shnum
    loads, executable, sections = [], [], []
    for i in range(phnum):
        typ, off, addr, _, filesz, memsz, flags, align = struct.unpack_from(
            '>IIIIIIII', buf, offset + phoff + i * phes)
        end = max(end, extent(off, filesz, 'segment'))
        if typ == 1:
            check(filesz <= memsz and addr <= LS_SIZE and memsz <= LS_SIZE - addr,
                  'PT_LOAD exceeds local store or has filesz > memsz')
            check(align in (0, 1) or (align & (align - 1)) == 0, 'invalid segment alignment')
            ph = dict(off=off, addr=addr, filesz=filesz, memsz=memsz, flags=flags)
            loads.append(ph)
            if flags & 1 and filesz:
                check(addr % 4 == 0 and filesz % 4 == 0, 'unaligned executable segment')
                executable.append(ph)
    check(loads, 'no PT_LOAD segments')
    check(entry < LS_SIZE and entry % 4 == 0, 'invalid entry local-store address')
    for i in range(shnum):
        _, typ, flags, addr, off, size, link, _, align, entsize = struct.unpack_from(
            '>IIIIIIIIII', buf, offset + shoff + i * shes)
        if typ != 8:
            end = max(end, extent(off, size, 'section'))
        check(align in (0, 1) or (align & (align - 1)) == 0, 'invalid section alignment')
        if typ == 2:
            check(entsize == 16 and size % 16 == 0 and link < shnum,
                  'invalid symbol table layout')
        if flags & 4 and size:
            check(typ != 8 and addr % 4 == 0 and size % 4 == 0,
                  'invalid executable section')
            check(any(p['addr'] <= addr and addr + size <= p['addr'] + p['filesz']
                      and off - p['off'] == addr - p['addr'] for p in executable),
                  'executable section is not mapped by executable PT_LOAD')
        sections.append(dict(type=typ, flags=flags, addr=addr, off=off, size=size))
    check(end >= max(52, phoff + phes * phnum),
          'runtime fingerprint extent omits ELF/program headers')
    return dict(size=end, entry=entry, loads=loads, executable=executable, sections=sections)


def scan_spu_candidates(buf):
    """Yield valid and invalid EM_SPU candidates, retaining every occurrence.

    Search every magic occurrence (even inside another image) so dedup never
    hides nested occurrences. Other ELF machines are not SPU candidates.
    """
    pos = 0
    while True:
        offset = buf.find(MAGIC, pos)
        if offset < 0:
            return
        pos = offset + 4
        if offset + 20 > len(buf) or buf[offset + 18:offset + 20] != b'\x00\x17':
            continue
        try:
            info = validate_spu_elf(buf, offset)
        except InvalidSPUELF as error:
            yield offset, None, str(error)
        else:
            yield offset, info, None


def fnv1a64(buf):
    # This is the existing runtime's offset basis (not the standard FNV value).
    value = 1469598103934665603
    for byte in buf:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return value
