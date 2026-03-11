#!/usr/bin/env python3
"""
PS3 ELF / SELF / PRX binary parser.

Parses standard ELF64 (PPU) and ELF32 headers, SELF (Signed ELF) wrappers,
program headers, section headers, PRX module info, export/import tables,
NID tables, and relocations.

Usage:
    python elf_parser.py <input_file> [--json] [--sections] [--segments]
                         [--imports] [--exports] [--relocs] [--all]
"""

import argparse
import json
import os
import struct
import sys

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

# ELF magic
ELF_MAGIC = b"\x7fELF"

# SELF magic  "SCE\0"
SELF_MAGIC = 0x53434500

# ELF classes
ELFCLASS32 = 1
ELFCLASS64 = 2

# Endianness
ELFDATA2MSB = 2  # big-endian  (PS3 native)
ELFDATA2LSB = 1  # little-endian

# OS/ABI
ELFOSABI_CELL_LV2 = 0x66

# ELF types
ET_EXEC = 2
ET_SCE_PPURELEXEC = 0xFFA4  # PRX

# Machine
EM_PPC64 = 21

# Program header types
PT_NULL = 0
PT_LOAD = 1
PT_NOTE = 4
PT_SCE_PPURELA = 0x700000A4

# Section header types
SHT_NULL = 0
SHT_PROGBITS = 1
SHT_SYMTAB = 2
SHT_STRTAB = 3
SHT_RELA = 4
SHT_NOBITS = 8
SHT_REL = 9

# Relocation types (R_PPC64_*)
R_PPC64_NONE = 0
R_PPC64_ADDR32 = 1
R_PPC64_ADDR16_LO = 4
R_PPC64_ADDR16_HI = 5
R_PPC64_ADDR16_HA = 6
R_PPC64_REL24 = 10
R_PPC64_REL14 = 11
R_PPC64_TOC16 = 47
R_PPC64_TOC16_LO = 48
R_PPC64_TOC16_HI = 49
R_PPC64_TOC16_HA = 50

RELA_TYPE_NAMES = {
    R_PPC64_NONE: "R_PPC64_NONE",
    R_PPC64_ADDR32: "R_PPC64_ADDR32",
    R_PPC64_ADDR16_LO: "R_PPC64_ADDR16_LO",
    R_PPC64_ADDR16_HI: "R_PPC64_ADDR16_HI",
    R_PPC64_ADDR16_HA: "R_PPC64_ADDR16_HA",
    R_PPC64_REL24: "R_PPC64_REL24",
    R_PPC64_REL14: "R_PPC64_REL14",
    R_PPC64_TOC16: "R_PPC64_TOC16",
    R_PPC64_TOC16_LO: "R_PPC64_TOC16_LO",
    R_PPC64_TOC16_HI: "R_PPC64_TOC16_HI",
    R_PPC64_TOC16_HA: "R_PPC64_TOC16_HA",
}

PT_TYPE_NAMES = {
    PT_NULL: "PT_NULL",
    PT_LOAD: "PT_LOAD",
    PT_NOTE: "PT_NOTE",
    PT_SCE_PPURELA: "PT_SCE_PPURELA",
}

# ---------------------------------------------------------------------------
# Helper: endian-aware unpacker
# ---------------------------------------------------------------------------

class BinaryReader:
    """Thin wrapper around a bytes buffer with endian-aware reads."""

    def __init__(self, data: bytes, big_endian: bool = True):
        self.data = data
        self.endian = ">" if big_endian else "<"

    def u8(self, off: int) -> int:
        return self.data[off]

    def u16(self, off: int) -> int:
        return struct.unpack_from(self.endian + "H", self.data, off)[0]

    def u32(self, off: int) -> int:
        return struct.unpack_from(self.endian + "I", self.data, off)[0]

    def u64(self, off: int) -> int:
        return struct.unpack_from(self.endian + "Q", self.data, off)[0]

    def s32(self, off: int) -> int:
        return struct.unpack_from(self.endian + "i", self.data, off)[0]

    def s64(self, off: int) -> int:
        return struct.unpack_from(self.endian + "q", self.data, off)[0]

    def bytes_at(self, off: int, size: int) -> bytes:
        return self.data[off:off + size]

    def cstring(self, off: int, max_len: int = 256) -> str:
        end = self.data.find(b"\x00", off, off + max_len)
        if end == -1:
            end = off + max_len
        return self.data[off:end].decode("utf-8", errors="replace")

# ---------------------------------------------------------------------------
# SELF header
# ---------------------------------------------------------------------------

class SELFHeader:
    """SCE SELF header – wraps an encrypted/signed ELF."""

    SIZE = 32  # minimum SCE header size

    def __init__(self):
        self.magic = 0
        self.version = 0
        self.key_type = 0
        self.category = 0
        self.metadata_offset = 0
        self.header_size = 0
        self.data_size = 0
        self.is_self = False
        self.elf_offset = 0  # offset to the embedded ELF

    @classmethod
    def parse(cls, data: bytes) -> "SELFHeader":
        if len(data) < cls.SIZE:
            raise ValueError("Data too small for SELF header")

        hdr = cls()
        # SELF header is always big-endian
        rd = BinaryReader(data, big_endian=True)
        hdr.magic = rd.u32(0)
        if hdr.magic != SELF_MAGIC:
            return hdr  # not a SELF

        hdr.is_self = True
        hdr.version = rd.u32(4)
        hdr.key_type = rd.u16(8)
        hdr.category = rd.u16(10)
        hdr.metadata_offset = rd.u32(12)
        hdr.header_size = rd.u64(16)
        hdr.data_size = rd.u64(24)

        # The embedded ELF starts at header_size in most SELF variants.
        # For retail SELFs the ELF header is actually at a fixed offset
        # after the extended header + section info entries.  We search
        # for the ELF magic within the first 0x1000 bytes as a fallback.
        candidate = int(hdr.header_size)
        if candidate < len(data) and data[candidate:candidate + 4] == ELF_MAGIC:
            hdr.elf_offset = candidate
        else:
            # brute search
            idx = data.find(ELF_MAGIC, 0, min(len(data), 0x10000))
            if idx != -1:
                hdr.elf_offset = idx
            else:
                hdr.elf_offset = 0  # give up – caller should handle
        return hdr

    def to_dict(self) -> dict:
        return {
            "magic": f"0x{self.magic:08X}",
            "version": self.version,
            "key_type": self.key_type,
            "category": self.category,
            "metadata_offset": self.metadata_offset,
            "header_size": self.header_size,
            "data_size": self.data_size,
            "elf_offset": self.elf_offset,
        }

# ---------------------------------------------------------------------------
# ELF header
# ---------------------------------------------------------------------------

class ELFHeader:
    """ELF32 / ELF64 file header."""

    def __init__(self):
        self.ei_class = 0      # 1=32, 2=64
        self.ei_data = 0       # 1=LE, 2=BE
        self.ei_osabi = 0
        self.e_type = 0
        self.e_machine = 0
        self.e_version = 0
        self.e_entry = 0
        self.e_phoff = 0
        self.e_shoff = 0
        self.e_flags = 0
        self.e_ehsize = 0
        self.e_phentsize = 0
        self.e_phnum = 0
        self.e_shentsize = 0
        self.e_shnum = 0
        self.e_shstrndx = 0
        self.big_endian = True
        self.is64 = True

    @classmethod
    def parse(cls, data: bytes, offset: int = 0) -> "ELFHeader":
        if data[offset:offset + 4] != ELF_MAGIC:
            raise ValueError(f"Bad ELF magic at offset {offset}")

        hdr = cls()
        hdr.ei_class = data[offset + 4]
        hdr.ei_data = data[offset + 5]
        hdr.ei_osabi = data[offset + 7]
        hdr.is64 = hdr.ei_class == ELFCLASS64
        hdr.big_endian = hdr.ei_data == ELFDATA2MSB

        rd = BinaryReader(data, hdr.big_endian)
        off = offset

        hdr.e_type = rd.u16(off + 16)
        hdr.e_machine = rd.u16(off + 18)
        hdr.e_version = rd.u32(off + 20)

        if hdr.is64:
            hdr.e_entry = rd.u64(off + 24)
            hdr.e_phoff = rd.u64(off + 32)
            hdr.e_shoff = rd.u64(off + 40)
            hdr.e_flags = rd.u32(off + 48)
            hdr.e_ehsize = rd.u16(off + 52)
            hdr.e_phentsize = rd.u16(off + 54)
            hdr.e_phnum = rd.u16(off + 56)
            hdr.e_shentsize = rd.u16(off + 58)
            hdr.e_shnum = rd.u16(off + 60)
            hdr.e_shstrndx = rd.u16(off + 62)
        else:
            hdr.e_entry = rd.u32(off + 24)
            hdr.e_phoff = rd.u32(off + 28)
            hdr.e_shoff = rd.u32(off + 32)
            hdr.e_flags = rd.u32(off + 36)
            hdr.e_ehsize = rd.u16(off + 40)
            hdr.e_phentsize = rd.u16(off + 42)
            hdr.e_phnum = rd.u16(off + 44)
            hdr.e_shentsize = rd.u16(off + 46)
            hdr.e_shnum = rd.u16(off + 48)
            hdr.e_shstrndx = rd.u16(off + 50)

        return hdr

    def to_dict(self) -> dict:
        return {
            "class": "ELF64" if self.is64 else "ELF32",
            "endian": "big" if self.big_endian else "little",
            "osabi": f"0x{self.ei_osabi:02X}",
            "type": f"0x{self.e_type:04X}",
            "machine": f"0x{self.e_machine:04X}",
            "entry": f"0x{self.e_entry:X}",
            "phoff": self.e_phoff,
            "shoff": self.e_shoff,
            "flags": f"0x{self.e_flags:08X}",
            "phnum": self.e_phnum,
            "shnum": self.e_shnum,
            "shstrndx": self.e_shstrndx,
        }

# ---------------------------------------------------------------------------
# Program header
# ---------------------------------------------------------------------------

class ProgramHeader:
    """Single ELF program header (PHDR) entry."""

    def __init__(self):
        self.p_type = 0
        self.p_flags = 0
        self.p_offset = 0
        self.p_vaddr = 0
        self.p_paddr = 0
        self.p_filesz = 0
        self.p_memsz = 0
        self.p_align = 0

    @classmethod
    def parse(cls, data: bytes, offset: int, is64: bool, big_endian: bool) -> "ProgramHeader":
        rd = BinaryReader(data, big_endian)
        ph = cls()
        off = offset
        if is64:
            ph.p_type = rd.u32(off + 0)
            ph.p_flags = rd.u32(off + 4)
            ph.p_offset = rd.u64(off + 8)
            ph.p_vaddr = rd.u64(off + 16)
            ph.p_paddr = rd.u64(off + 24)
            ph.p_filesz = rd.u64(off + 32)
            ph.p_memsz = rd.u64(off + 40)
            ph.p_align = rd.u64(off + 48)
        else:
            ph.p_type = rd.u32(off + 0)
            ph.p_offset = rd.u32(off + 4)
            ph.p_vaddr = rd.u32(off + 8)
            ph.p_paddr = rd.u32(off + 12)
            ph.p_filesz = rd.u32(off + 16)
            ph.p_memsz = rd.u32(off + 20)
            ph.p_flags = rd.u32(off + 24)
            ph.p_align = rd.u32(off + 28)
        return ph

    def type_name(self) -> str:
        return PT_TYPE_NAMES.get(self.p_type, f"0x{self.p_type:08X}")

    def to_dict(self) -> dict:
        return {
            "type": self.type_name(),
            "flags": f"0x{self.p_flags:X}",
            "offset": f"0x{self.p_offset:X}",
            "vaddr": f"0x{self.p_vaddr:X}",
            "paddr": f"0x{self.p_paddr:X}",
            "filesz": f"0x{self.p_filesz:X}",
            "memsz": f"0x{self.p_memsz:X}",
            "align": f"0x{self.p_align:X}",
        }

# ---------------------------------------------------------------------------
# Section header
# ---------------------------------------------------------------------------

class SectionHeader:
    """Single ELF section header entry."""

    def __init__(self):
        self.sh_name = 0
        self.sh_type = 0
        self.sh_flags = 0
        self.sh_addr = 0
        self.sh_offset = 0
        self.sh_size = 0
        self.sh_link = 0
        self.sh_info = 0
        self.sh_addralign = 0
        self.sh_entsize = 0
        self.name_str = ""

    @classmethod
    def parse(cls, data: bytes, offset: int, is64: bool, big_endian: bool) -> "SectionHeader":
        rd = BinaryReader(data, big_endian)
        sh = cls()
        off = offset
        if is64:
            sh.sh_name = rd.u32(off + 0)
            sh.sh_type = rd.u32(off + 4)
            sh.sh_flags = rd.u64(off + 8)
            sh.sh_addr = rd.u64(off + 16)
            sh.sh_offset = rd.u64(off + 24)
            sh.sh_size = rd.u64(off + 32)
            sh.sh_link = rd.u32(off + 40)
            sh.sh_info = rd.u32(off + 44)
            sh.sh_addralign = rd.u64(off + 48)
            sh.sh_entsize = rd.u64(off + 56)
        else:
            sh.sh_name = rd.u32(off + 0)
            sh.sh_type = rd.u32(off + 4)
            sh.sh_flags = rd.u32(off + 8)
            sh.sh_addr = rd.u32(off + 12)
            sh.sh_offset = rd.u32(off + 16)
            sh.sh_size = rd.u32(off + 20)
            sh.sh_link = rd.u32(off + 24)
            sh.sh_info = rd.u32(off + 28)
            sh.sh_addralign = rd.u32(off + 32)
            sh.sh_entsize = rd.u32(off + 36)
        return sh

    def to_dict(self) -> dict:
        return {
            "name": self.name_str or f"(stridx {self.sh_name})",
            "type": f"0x{self.sh_type:X}",
            "flags": f"0x{self.sh_flags:X}",
            "addr": f"0x{self.sh_addr:X}",
            "offset": f"0x{self.sh_offset:X}",
            "size": f"0x{self.sh_size:X}",
            "link": self.sh_link,
            "info": self.sh_info,
            "entsize": f"0x{self.sh_entsize:X}",
        }

# ---------------------------------------------------------------------------
# Relocation entry
# ---------------------------------------------------------------------------

class RelaEntry:
    """ELF RELA relocation entry."""

    def __init__(self):
        self.r_offset = 0
        self.r_info = 0
        self.r_addend = 0
        self.r_sym = 0
        self.r_type = 0

    @classmethod
    def parse(cls, data: bytes, offset: int, is64: bool, big_endian: bool) -> "RelaEntry":
        rd = BinaryReader(data, big_endian)
        r = cls()
        if is64:
            r.r_offset = rd.u64(offset)
            r.r_info = rd.u64(offset + 8)
            r.r_addend = rd.s64(offset + 16)
            r.r_sym = (r.r_info >> 32) & 0xFFFFFFFF
            r.r_type = r.r_info & 0xFFFFFFFF
        else:
            r.r_offset = rd.u32(offset)
            r.r_info = rd.u32(offset + 4)
            r.r_addend = rd.s32(offset + 8)
            r.r_sym = (r.r_info >> 8) & 0xFFFFFF
            r.r_type = r.r_info & 0xFF
        return r

    def type_name(self) -> str:
        return RELA_TYPE_NAMES.get(self.r_type, f"0x{self.r_type:X}")

    def to_dict(self) -> dict:
        return {
            "offset": f"0x{self.r_offset:X}",
            "sym": self.r_sym,
            "type": self.type_name(),
            "addend": f"0x{self.r_addend:X}" if self.r_addend >= 0 else f"-0x{-self.r_addend:X}",
        }

# ---------------------------------------------------------------------------
# PRX module info
# ---------------------------------------------------------------------------

class PRXModuleInfo:
    """PS3 PRX sys_process_prx_info_t structure.

    Typically located at the start of the first PT_LOAD segment for PRX files.
    Layout (big-endian, 64-bit pointers):
        u16  attributes
        u8   version[2]
        char name[28]
        u32  toc
        u64  exports_start
        u64  exports_end
        u64  imports_start
        u64  imports_end
    """

    SIZE = 0x48

    def __init__(self):
        self.attributes = 0
        self.version = (0, 0)
        self.name = ""
        self.toc = 0
        self.exports_start = 0
        self.exports_end = 0
        self.imports_start = 0
        self.imports_end = 0

    @classmethod
    def parse(cls, data: bytes, offset: int, big_endian: bool = True) -> "PRXModuleInfo":
        if offset + cls.SIZE > len(data):
            raise ValueError("Not enough data for PRX module info")
        rd = BinaryReader(data, big_endian)
        m = cls()
        m.attributes = rd.u16(offset)
        m.version = (rd.u8(offset + 2), rd.u8(offset + 3))
        m.name = rd.cstring(offset + 4, 28)
        m.toc = rd.u32(offset + 32)
        m.exports_start = rd.u64(offset + 36)
        m.exports_end = rd.u64(offset + 44)
        m.imports_start = rd.u64(offset + 52)
        m.imports_end = rd.u64(offset + 60)
        return m

    def to_dict(self) -> dict:
        return {
            "attributes": f"0x{self.attributes:04X}",
            "version": f"{self.version[0]}.{self.version[1]}",
            "name": self.name,
            "toc": f"0x{self.toc:08X}",
            "exports_start": f"0x{self.exports_start:X}",
            "exports_end": f"0x{self.exports_end:X}",
            "imports_start": f"0x{self.imports_start:X}",
            "imports_end": f"0x{self.imports_end:X}",
        }

# ---------------------------------------------------------------------------
# PRX export / import table entry helpers
# ---------------------------------------------------------------------------

class PRXExportEntry:
    """PRX export table entry (variable size, 0x1C minimum)."""

    SIZE_MIN = 0x1C

    def __init__(self):
        self.size = 0
        self.attributes = 0
        self.version = 0
        self.num_funcs = 0
        self.num_vars = 0
        self.num_tlsvars = 0
        self.name_ptr = 0
        self.nid_table_ptr = 0
        self.stub_table_ptr = 0
        self.name_str = ""
        self.nids: list[int] = []

    @classmethod
    def parse(cls, data: bytes, offset: int, big_endian: bool = True) -> "PRXExportEntry":
        rd = BinaryReader(data, big_endian)
        e = cls()
        e.size = rd.u8(offset + 0)
        e.attributes = rd.u8(offset + 1) << 8 | rd.u8(offset + 2) if e.size > 2 else 0
        # Typical layout:
        e.version = rd.u16(offset + 2) if e.size >= 4 else 0
        e.attributes = rd.u16(offset + 4) if e.size >= 6 else 0
        e.num_funcs = rd.u16(offset + 6) if e.size >= 8 else 0
        e.num_vars = rd.u16(offset + 8) if e.size >= 10 else 0
        e.num_tlsvars = rd.u16(offset + 10) if e.size >= 12 else 0
        # skip reserved
        e.name_ptr = rd.u32(offset + 16) if e.size >= 20 else 0
        e.nid_table_ptr = rd.u32(offset + 20) if e.size >= 24 else 0
        e.stub_table_ptr = rd.u32(offset + 24) if e.size >= 28 else 0
        return e

    def to_dict(self) -> dict:
        return {
            "name": self.name_str or f"(ptr 0x{self.name_ptr:X})",
            "num_funcs": self.num_funcs,
            "num_vars": self.num_vars,
            "nids": [f"0x{n:08X}" for n in self.nids],
        }


class PRXImportEntry:
    """PRX import table entry."""

    SIZE_MIN = 0x2C

    def __init__(self):
        self.size = 0
        self.attributes = 0
        self.version = 0
        self.num_funcs = 0
        self.num_vars = 0
        self.num_tlsvars = 0
        self.name_ptr = 0
        self.nid_table_ptr = 0
        self.stub_table_ptr = 0
        self.name_str = ""
        self.nids: list[int] = []

    @classmethod
    def parse(cls, data: bytes, offset: int, big_endian: bool = True) -> "PRXImportEntry":
        rd = BinaryReader(data, big_endian)
        i = cls()
        i.size = rd.u8(offset + 0)
        i.version = rd.u16(offset + 2) if i.size >= 4 else 0
        i.attributes = rd.u16(offset + 4) if i.size >= 6 else 0
        i.num_funcs = rd.u16(offset + 6) if i.size >= 8 else 0
        i.num_vars = rd.u16(offset + 8) if i.size >= 10 else 0
        i.num_tlsvars = rd.u16(offset + 10) if i.size >= 12 else 0
        # reserved[4]
        i.name_ptr = rd.u32(offset + 16) if i.size >= 20 else 0
        i.nid_table_ptr = rd.u32(offset + 20) if i.size >= 24 else 0
        i.stub_table_ptr = rd.u32(offset + 24) if i.size >= 28 else 0
        return i

    def to_dict(self) -> dict:
        return {
            "name": self.name_str or f"(ptr 0x{self.name_ptr:X})",
            "num_funcs": self.num_funcs,
            "num_vars": self.num_vars,
            "nids": [f"0x{n:08X}" for n in self.nids],
        }

# ---------------------------------------------------------------------------
# Top-level ELF file
# ---------------------------------------------------------------------------

def vaddr_to_offset(phdrs: list[ProgramHeader], vaddr: int) -> int | None:
    """Convert a virtual address to a file offset using PT_LOAD segments."""
    for ph in phdrs:
        if ph.p_type == PT_LOAD and ph.p_filesz > 0:
            if ph.p_vaddr <= vaddr < ph.p_vaddr + ph.p_filesz:
                return int(ph.p_offset + (vaddr - ph.p_vaddr))
    return None


class ELFFile:
    """Represents a parsed PS3 ELF / SELF / PRX file."""

    def __init__(self, path: str):
        self.path = path
        self.raw_data: bytes = b""
        self.self_header: SELFHeader | None = None
        self.elf_offset: int = 0
        self.elf_header: ELFHeader | None = None
        self.program_headers: list[ProgramHeader] = []
        self.section_headers: list[SectionHeader] = []
        self.module_info: PRXModuleInfo | None = None
        self.exports: list[PRXExportEntry] = []
        self.imports: list[PRXImportEntry] = []
        self.relocations: list[RelaEntry] = []

    # ---- loading ----

    def load(self) -> None:
        """Read the file and parse all structures."""
        with open(self.path, "rb") as f:
            self.raw_data = f.read()

        # Detect SELF wrapper
        if len(self.raw_data) >= 4:
            maybe_self = SELFHeader.parse(self.raw_data)
            if maybe_self.is_self:
                self.self_header = maybe_self
                self.elf_offset = maybe_self.elf_offset

        # Parse ELF header
        if self.raw_data[self.elf_offset:self.elf_offset + 4] != ELF_MAGIC:
            raise ValueError("Could not locate ELF header in file")

        self.elf_header = ELFHeader.parse(self.raw_data, self.elf_offset)
        self._parse_program_headers()
        self._parse_section_headers()
        self._resolve_section_names()

        # If this is a PRX, parse module info
        if self.elf_header.e_type == ET_SCE_PPURELEXEC:
            self._parse_prx_module_info()

        self._parse_relocations()

    # ---- internal parsing ----

    def _rd(self) -> BinaryReader:
        return BinaryReader(self.raw_data, self.elf_header.big_endian)

    def _parse_program_headers(self) -> None:
        eh = self.elf_header
        base = self.elf_offset + eh.e_phoff
        for i in range(eh.e_phnum):
            off = base + i * eh.e_phentsize
            if off + eh.e_phentsize > len(self.raw_data):
                break
            ph = ProgramHeader.parse(self.raw_data, off, eh.is64, eh.big_endian)
            self.program_headers.append(ph)

    def _parse_section_headers(self) -> None:
        eh = self.elf_header
        if eh.e_shoff == 0 or eh.e_shnum == 0:
            return
        base = self.elf_offset + eh.e_shoff
        for i in range(eh.e_shnum):
            off = base + i * eh.e_shentsize
            if off + eh.e_shentsize > len(self.raw_data):
                break
            sh = SectionHeader.parse(self.raw_data, off, eh.is64, eh.big_endian)
            self.section_headers.append(sh)

    def _resolve_section_names(self) -> None:
        eh = self.elf_header
        if eh.e_shstrndx == 0 or eh.e_shstrndx >= len(self.section_headers):
            return
        strtab_sh = self.section_headers[eh.e_shstrndx]
        strtab_off = self.elf_offset + strtab_sh.sh_offset
        rd = BinaryReader(self.raw_data, eh.big_endian)
        for sh in self.section_headers:
            if sh.sh_name < strtab_sh.sh_size:
                sh.name_str = rd.cstring(strtab_off + sh.sh_name)

    def _parse_prx_module_info(self) -> None:
        """Extract module info from the first PT_LOAD segment of a PRX."""
        for ph in self.program_headers:
            if ph.p_type == PT_LOAD and ph.p_filesz > 0:
                # module info is at paddr offset within the segment for PRX
                info_off = int(ph.p_offset + (ph.p_paddr & 0xFFFFFFFF))
                if info_off + PRXModuleInfo.SIZE <= len(self.raw_data):
                    try:
                        self.module_info = PRXModuleInfo.parse(
                            self.raw_data, info_off, self.elf_header.big_endian
                        )
                        self._parse_exports_imports()
                    except Exception:
                        pass
                break

    def _parse_exports_imports(self) -> None:
        mi = self.module_info
        rd = self._rd()

        # exports
        if mi.exports_start and mi.exports_end and mi.exports_end > mi.exports_start:
            off = vaddr_to_offset(self.program_headers, mi.exports_start)
            end_off = vaddr_to_offset(self.program_headers, mi.exports_end)
            if off is not None and end_off is not None:
                while off < end_off and off + PRXExportEntry.SIZE_MIN <= len(self.raw_data):
                    entry = PRXExportEntry.parse(self.raw_data, off, self.elf_header.big_endian)
                    if entry.size == 0:
                        break
                    # resolve name
                    if entry.name_ptr:
                        name_off = vaddr_to_offset(self.program_headers, entry.name_ptr)
                        if name_off is not None:
                            entry.name_str = rd.cstring(name_off)
                    # read NID table
                    total = entry.num_funcs + entry.num_vars + entry.num_tlsvars
                    if entry.nid_table_ptr and total > 0:
                        nid_off = vaddr_to_offset(self.program_headers, entry.nid_table_ptr)
                        if nid_off is not None:
                            for j in range(total):
                                if nid_off + j * 4 + 4 <= len(self.raw_data):
                                    entry.nids.append(rd.u32(nid_off + j * 4))
                    self.exports.append(entry)
                    off += entry.size

        # imports
        if mi.imports_start and mi.imports_end and mi.imports_end > mi.imports_start:
            off = vaddr_to_offset(self.program_headers, mi.imports_start)
            end_off = vaddr_to_offset(self.program_headers, mi.imports_end)
            if off is not None and end_off is not None:
                while off < end_off and off + PRXImportEntry.SIZE_MIN <= len(self.raw_data):
                    entry = PRXImportEntry.parse(self.raw_data, off, self.elf_header.big_endian)
                    if entry.size == 0:
                        break
                    if entry.name_ptr:
                        name_off = vaddr_to_offset(self.program_headers, entry.name_ptr)
                        if name_off is not None:
                            entry.name_str = rd.cstring(name_off)
                    total = entry.num_funcs + entry.num_vars + entry.num_tlsvars
                    if entry.nid_table_ptr and total > 0:
                        nid_off = vaddr_to_offset(self.program_headers, entry.nid_table_ptr)
                        if nid_off is not None:
                            for j in range(total):
                                if nid_off + j * 4 + 4 <= len(self.raw_data):
                                    entry.nids.append(rd.u32(nid_off + j * 4))
                    self.imports.append(entry)
                    off += entry.size

    def _parse_relocations(self) -> None:
        """Parse RELA entries from PT_SCE_PPURELA segments and SHT_RELA sections."""
        eh = self.elf_header

        # From program headers (PT_SCE_PPURELA)
        for ph in self.program_headers:
            if ph.p_type == PT_SCE_PPURELA and ph.p_filesz > 0:
                entry_size = 24 if eh.is64 else 12
                off = int(ph.p_offset)
                end = off + int(ph.p_filesz)
                while off + entry_size <= end and off + entry_size <= len(self.raw_data):
                    r = RelaEntry.parse(self.raw_data, off, eh.is64, eh.big_endian)
                    self.relocations.append(r)
                    off += entry_size

        # From section headers (SHT_RELA)
        for sh in self.section_headers:
            if sh.sh_type == SHT_RELA and sh.sh_size > 0:
                entry_size = int(sh.sh_entsize) if sh.sh_entsize else (24 if eh.is64 else 12)
                if entry_size == 0:
                    entry_size = 24 if eh.is64 else 12
                off = self.elf_offset + int(sh.sh_offset)
                end = off + int(sh.sh_size)
                while off + entry_size <= end and off + entry_size <= len(self.raw_data):
                    r = RelaEntry.parse(self.raw_data, off, eh.is64, eh.big_endian)
                    self.relocations.append(r)
                    off += entry_size

    # ---- public query helpers ----

    def get_segment_data(self, index: int) -> bytes:
        """Return the raw bytes of program header *index*."""
        ph = self.program_headers[index]
        return self.raw_data[ph.p_offset:ph.p_offset + ph.p_filesz]

    def get_section_data(self, index: int) -> bytes:
        """Return the raw bytes of section header *index*."""
        sh = self.section_headers[index]
        off = self.elf_offset + sh.sh_offset
        return self.raw_data[off:off + sh.sh_size]

    # ---- JSON report ----

    def to_dict(self, sections=True, segments=True, imports=True,
                exports=True, relocs=True) -> dict:
        result: dict = {"file": os.path.basename(self.path)}

        if self.self_header:
            result["self_header"] = self.self_header.to_dict()

        if self.elf_header:
            result["elf_header"] = self.elf_header.to_dict()

        if segments and self.program_headers:
            result["program_headers"] = [ph.to_dict() for ph in self.program_headers]

        if sections and self.section_headers:
            result["section_headers"] = [sh.to_dict() for sh in self.section_headers]

        if self.module_info:
            result["module_info"] = self.module_info.to_dict()

        if exports and self.exports:
            result["exports"] = [e.to_dict() for e in self.exports]

        if imports and self.imports:
            result["imports"] = [i.to_dict() for i in self.imports]

        if relocs and self.relocations:
            result["relocations_count"] = len(self.relocations)
            result["relocations_sample"] = [r.to_dict() for r in self.relocations[:50]]

        return result

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description="PS3 ELF / SELF / PRX binary parser"
    )
    parser.add_argument("input", help="Path to ELF, SELF, or PRX file")
    parser.add_argument("--json", action="store_true", default=True,
                        help="Output as JSON (default)")
    parser.add_argument("--sections", action="store_true", help="Show section headers")
    parser.add_argument("--segments", action="store_true", help="Show program headers")
    parser.add_argument("--imports", action="store_true", help="Show PRX imports")
    parser.add_argument("--exports", action="store_true", help="Show PRX exports")
    parser.add_argument("--relocs", action="store_true", help="Show relocations")
    parser.add_argument("--all", action="store_true", help="Show everything")
    args = parser.parse_args()

    show_all = args.all or not any([args.sections, args.segments, args.imports,
                                     args.exports, args.relocs])

    elf = ELFFile(args.input)
    try:
        elf.load()
    except Exception as exc:
        print(f"Error: {exc}", file=sys.stderr)
        sys.exit(1)

    report = elf.to_dict(
        sections=show_all or args.sections,
        segments=show_all or args.segments,
        imports=show_all or args.imports,
        exports=show_all or args.exports,
        relocs=show_all or args.relocs,
    )

    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
