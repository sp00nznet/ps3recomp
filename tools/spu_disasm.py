#!/usr/bin/env python3
"""
SPU (Synergistic Processing Unit) disassembler for PS3 binaries.

Decodes 32-bit fixed-width SPU instructions covering memory, integer,
logical, shift/rotate, branch, compare, channel, and hint-for-branch
operations.

Usage:
    python spu_disasm.py <input_file> [--base ADDR] [--json] [--length N]
"""

import argparse
import json
import os
import struct
import sys

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def sign_extend(value: int, nbits: int) -> int:
    """Sign-extend *value* from *nbits* to a Python int."""
    if value & (1 << (nbits - 1)):
        value -= 1 << nbits
    return value


def bits(insn: int, hi: int, lo: int) -> int:
    """Extract bits [hi:lo] (inclusive, hi is MSB, lo is LSB, 0-indexed from MSB).

    SPU instruction encoding uses bit 0 as MSB.
    bits(insn, 0, 3)  => top 4 bits.
    """
    shift = 31 - lo
    mask = (1 << (lo - hi + 1)) - 1
    return (insn >> shift) & mask

# ---------------------------------------------------------------------------
# Instruction representation
# ---------------------------------------------------------------------------

class SPUInstruction:
    """A decoded SPU instruction."""

    __slots__ = ("addr", "raw", "mnemonic", "operands", "comment")

    def __init__(self, addr: int = 0, raw: int = 0, mnemonic: str = "???",
                 operands: str = "", comment: str = ""):
        self.addr = addr
        self.raw = raw
        self.mnemonic = mnemonic
        self.operands = operands
        self.comment = comment

    def __str__(self) -> str:
        hexb = f"{self.raw:08X}"
        line = f"{self.addr:08X}:  {hexb}  {self.mnemonic:<12s} {self.operands}"
        if self.comment:
            line += f"  ; {self.comment}"
        return line

# ---------------------------------------------------------------------------
# Decode tables
#
# SPU instructions are categorised by opcode field widths:
#   - 4-bit (bits 0-3)   -- not many
#   - 7-bit (bits 0-6)   -- RI18, RI16
#   - 8-bit (bits 0-7)   -- RI10
#   - 9-bit (bits 0-8)   -- RI8
#   - 11-bit (bits 0-10) -- RR, RRR, special
# ---------------------------------------------------------------------------

# RRR format: opcd(4) rt(7) rb(7) ra(7) rc(7)
RRR_TABLE: dict[int, str] = {
    0b1100: "mpya",       # multiply and add
    0b1110: "fma",        # floating multiply-add
    0b1111: "fms",        # floating multiply-subtract
    0b1101: "fnms",       # floating negative multiply-subtract
    0b1011: "selb",       # select bits
    0b1000: "shufb",      # shuffle bytes
}

# RI18 format: opcd(7) i18(18) rt(7).
# Per Cell BE ISA + RPCS3 SPUOpcodes.h, only THREE instructions are RI18.
# IL/ILH/ILHU were *incorrectly* classified here previously — they are RI16
# (16-bit immediate, 9-bit opcode); fixed in RI16_TABLE below. Same for
# BR/BRSL/BRA/BRASL.
RI18_TABLE: dict[int, str] = {
    0b0100001: "ila",     # 0x21 - immediate load address (unsigned 18-bit)
    0b0001000: "hbra",    # 0x08 - hint for branch (absolute)
    0b0001001: "hbrr",    # 0x09 - hint for branch (relative)
}

# RI16 format: opcd(9) i16(16) rt(7).
# Values verified against RPCS3 SPUOpcodes.h (magn=2 entries).
RI16_TABLE: dict[int, str] = {
    # Immediate loaders (most common — bulk of pre-fix .word misses)
    0b010000001: "il",      # 0x81 - load word (sign-ext 16-bit) into all lanes
    0b010000010: "ilhu",    # 0x82 - load halfword upper (i16 << 16)
    0b010000011: "ilh",     # 0x83 - load halfword (broadcast)
    0b011000001: "iohl",    # 0xC1 - OR halfword lower
    # Quadword load/store, absolute and PC-relative
    0b001100001: "lqa",     # 0x61
    0b001000001: "stqa",    # 0x41
    0b001100111: "lqr",     # 0x67
    0b001000111: "stqr",    # 0x47
    # Branches (relative/absolute, +link). Target = PC + sx(i16)*4 (rel)
    # or i16*4 (abs).
    0b001100100: "br",      # 0x64
    0b001100110: "brsl",    # 0x66 - branch + link (rt = link reg)
    0b001100000: "bra",     # 0x60 - absolute
    0b001100010: "brasl",   # 0x62 - absolute + link
    # Conditional branches (RT tested)
    0b001000000: "brz",     # 0x40
    0b001000010: "brnz",    # 0x42
    0b001000100: "brhz",    # 0x44
    0b001000110: "brhnz",   # 0x46
    # Form-select mask from bytes immediate
    0b001100101: "fsmbi",   # 0x65
}

# RI10 format: opcd(8) i10(10) ra(7) rt(7).
# Values verified against RPCS3 SPUOpcodes.h (magn=3 entries).
# NOTE: shli/shlhi/roti/rothi/rotmi/rotmai/rotmhi/rotmahi/shlqbii/shlqbyi/
# rotqbii/rotqbyi are NOT RI10 — they are 11-bit RR-form opcodes (0x78-0x7F
# and 0x1F8-0x1FF) and live in SPU_RR below. The previous table mis-classified
# them, which is why a real SPU image produced hundreds of `.word` lines.
RI10_TABLE: dict[int, str] = {
    0x04: "ori",    0x05: "orhi",   0x06: "orbi",
    0x0c: "sfi",    0x0d: "sfhi",
    0x14: "andi",   0x15: "andhi",  0x16: "andbi",
    0x1c: "ai",     0x1d: "ahi",
    0x24: "stqd",   0x34: "lqd",
    0x44: "xori",   0x45: "xorhi",  0x46: "xorbi",
    0x4c: "cgti",   0x4d: "cgthi",  0x4e: "cgtbi",  0x4f: "hgti",
    0x5c: "clgti",  0x5d: "clgthi", 0x5e: "clgtbi", 0x5f: "hlgti",
    0x74: "mpyi",   0x75: "mpyui",
    0x7c: "ceqi",   0x7d: "ceqhi",  0x7e: "ceqbi",  0x7f: "heqi",
}

# RI8 format: opcd(9+) ... we handle these specially

# RR format: opcd(11) [optional: rb(7) | i7(7) | channel(5+pad)] ra(7) rt(7).
# 11-bit opcodes, lookup key = (insn >> 21) & 0x7FF.
# Values verified against RPCS3 SPUOpcodes.h (magn=0 entries — the
# authoritative SPU decoder used by a working emulator). The shift/rotate
# immediates (shli, shlhi, roti, rothi, rotmi, rotmai, rotmhi, rotmahi,
# shlqbii, rotqbii, shlqbyi, rotqbyi) live HERE at opcodes 0x078-0x07F and
# 0x1F8-0x1FF; they are NOT RI10. The previous table had them in RI10 at
# 0x20/0x21/etc., which made every real SPU image fall through to `.word`.
SPU_RR: dict[int, str] = {
    # Control / channel
    0x000: "stop",   0x001: "lnop",  0x002: "sync",  0x003: "dsync",
    0x00c: "mfspr",  0x00d: "rdch",  0x00f: "rchcnt",
    # Arithmetic / logical (3-reg)
    0x040: "sf",     0x041: "or",    0x042: "bg",
    0x048: "sfh",    0x049: "nor",
    0x053: "absdb",
    # Shift / rotate (register-variable)
    0x058: "rot",    0x059: "rotm",  0x05a: "rotma", 0x05b: "shl",
    0x05c: "roth",   0x05d: "rothm", 0x05e: "rotmah",0x05f: "shlh",
    # Shift / rotate (immediate, RI7 form within RR-space)
    0x078: "roti",   0x079: "rotmi", 0x07a: "rotmai",0x07b: "shli",
    0x07c: "rothi",  0x07d: "rothmi",0x07e: "rotmahi",0x07f: "shlhi",
    # More arithmetic / logical
    0x0c0: "a",      0x0c1: "and",   0x0c2: "cg",
    0x0c8: "ah",     0x0c9: "nand",
    0x0d3: "avgb",
    0x10c: "mtspr",  0x10d: "wrch",
    # Indirect-branch conditionals
    0x128: "biz",    0x129: "binz",  0x12a: "bihz",  0x12b: "bihnz",
    0x140: "stopd",  0x144: "stqx",
    # Indirect branches / hints
    0x1a8: "bi",     0x1a9: "bisl",  0x1aa: "iret",  0x1ab: "bisled",
    0x1ac: "hbr",
    # Gather bit / form-select mask / FP reciprocal estimates
    0x1b0: "gb",     0x1b1: "gbh",   0x1b2: "gbb",
    0x1b4: "fsm",    0x1b5: "fsmh",  0x1b6: "fsmb",
    0x1b8: "frest",  0x1b9: "frsqest",
    0x1c4: "lqx",
    # Whole-quadword byte rotate/shift (register-variable, by-bit-count)
    0x1cc: "rotqbybi", 0x1cd: "rotqmbybi", 0x1cf: "shlqbybi",
    # Constant generation
    0x1d4: "cbx",    0x1d5: "chx",   0x1d6: "cwx",   0x1d7: "cdx",
    # Whole-quadword bit/byte rotate/shift (register-variable)
    0x1d8: "rotqbi", 0x1d9: "rotqmbi", 0x1db: "shlqbi",
    0x1dc: "rotqby", 0x1dd: "rotqmby", 0x1df: "shlqby",
    0x1f0: "orx",
    # Constant generation (d-form, 7-bit immediate)
    0x1f4: "cbd",    0x1f5: "chd",   0x1f6: "cwd",   0x1f7: "cdd",
    # Whole-quadword bit/byte rotate/shift (immediate, RI7)
    0x1f8: "rotqbii",0x1f9: "rotqmbii", 0x1fb: "shlqbii",
    0x1fc: "rotqbyi",0x1fd: "rotqmbyi", 0x1ff: "shlqbyi",
    0x201: "nop",
    # Compares + XOR / sumb / halt-greater
    0x240: "cgt",    0x241: "xor",   0x248: "cgth",  0x249: "eqv",
    0x250: "cgtb",   0x253: "sumb",  0x258: "hgt",
    0x2a5: "clz",    0x2a6: "xswd",  0x2ae: "xshw",  0x2b4: "cntb",
    0x2b6: "xsbh",
    0x2c0: "clgt",   0x2c1: "andc",  0x2c2: "fcgt",  0x2c3: "dfcgt",
    0x2c4: "fa",     0x2c5: "fs",    0x2c6: "fm",
    0x2c8: "clgth",  0x2c9: "orc",   0x2ca: "fcmgt", 0x2cb: "dfcmgt",
    0x2cc: "dfa",    0x2cd: "dfs",   0x2ce: "dfm",
    0x2d0: "clgtb",  0x2d8: "hlgt",
    # Extended add/sub w/ carry, multiply-add-high
    0x340: "addx",   0x341: "sfx",   0x342: "cgx",   0x343: "bgx",
    0x346: "mpyhha", 0x34e: "mpyhhau",
    # Double-precision FMA
    0x35c: "dfma",   0x35d: "dfms",  0x35e: "dfnms", 0x35f: "dfnma",
    # FPSCR + double-extend / round + DP test
    0x398: "fscrrd", 0x3b8: "fesd",  0x3b9: "frds",  0x3ba: "fscrwr",
    0x3bf: "dftsv",
    0x3c0: "ceq",    0x3c2: "fceq",  0x3c3: "dfceq",
    0x3c4: "mpy",    0x3c5: "mpyh",  0x3c6: "mpyhh", 0x3c7: "mpys",
    0x3c8: "ceqh",   0x3ca: "fcmeq", 0x3cb: "dfcmeq",
    0x3cc: "mpyu",   0x3ce: "mpyhhu",
    0x3d0: "ceqb",   0x3d4: "fi",    0x3d8: "heq",
}

# Channel names — corrected per IBM Cell BE Architecture Manual v1.02
# Channel number is in bits 11-7 of rdch/wrch/rchcnt instructions.
CHANNEL_NAMES = {
    0:  "SPU_RdEventStat",
    1:  "SPU_WrEventMask",
    2:  "SPU_WrEventAck",
    3:  "SPU_RdSigNotify1",
    4:  "SPU_RdSigNotify2",
    7:  "SPU_WrDec",
    8:  "SPU_RdDec",
    9:  "MFC_WrMSSyncReq",
    11: "SPU_RdEventMask",
    13: "SPU_RdMachStat",
    14: "SPU_WrSRR0",
    15: "SPU_RdSRR0",
    16: "MFC_LSA",
    17: "MFC_EAH",
    18: "MFC_EAL",
    19: "MFC_Size",
    20: "MFC_TagID",
    21: "MFC_Cmd",
    22: "MFC_WrTagMask",
    23: "MFC_WrTagUpdate",
    24: "MFC_RdTagStat",
    25: "MFC_RdListStallStat",
    26: "MFC_WrListStallAck",
    27: "MFC_RdAtomicStat",
    28: "SPU_WrOutMbox",
    29: "SPU_RdInMbox",
    30: "SPU_WrOutIntrMbox",
}

# ---------------------------------------------------------------------------
# Decode
# ---------------------------------------------------------------------------

def spu_decode(insn: int, addr: int = 0) -> SPUInstruction:
    """Decode a single 32-bit SPU instruction."""
    result = SPUInstruction(addr=addr, raw=insn)

    # Extract various opcode widths
    op4 = (insn >> 28) & 0xF
    op7 = (insn >> 25) & 0x7F
    op8 = (insn >> 24) & 0xFF
    op9 = (insn >> 23) & 0x1FF
    op11 = (insn >> 21) & 0x7FF

    # Fields
    rt = insn & 0x7F
    ra = (insn >> 7) & 0x7F
    rb = (insn >> 14) & 0x7F
    rc = (insn >> 21) & 0x7F  # for RRR format

    i10 = sign_extend((insn >> 14) & 0x3FF, 10)
    i16 = sign_extend((insn >> 7) & 0xFFFF, 16)
    i18 = (insn >> 7) & 0x3FFFF

    # ---- RRR format (4-bit opcode) ----
    if op4 in RRR_TABLE:
        mne = RRR_TABLE[op4]
        result.mnemonic = mne
        result.operands = f"$r{rt}, $r{ra}, $r{rb}, $r{rc}"
        return result

    # ---- RI18 format (7-bit opcode) ----
    # Only ila / hbra / hbrr after the table cleanup.
    if op7 in RI18_TABLE:
        mne = RI18_TABLE[op7]
        result.mnemonic = mne
        if mne == "ila":
            result.operands = f"$r{rt}, 0x{i18:X}"
        elif mne in ("hbra", "hbrr"):
            # Branch hints. The 18-bit field encodes the hint anchor; the
            # lifter currently ignores hbra/hbrr so the exact shape of this
            # operand string doesn't affect codegen.
            result.operands = f"0x{i18:X}, $r{rt}"
        else:
            result.operands = f"$r{rt}, 0x{i18:X}"
        return result

    # ---- RI16 format (9-bit opcode) ----
    # Must be checked BEFORE RI10 — some RI16 op9 values share top 8 bits
    # with RI10 entries (brz/shlhi both have op8=0x20; brnz/shli have op8=0x21).
    if op9 in RI16_TABLE:
        mne = RI16_TABLE[op9]
        result.mnemonic = mne
        # Branches (relative): target = PC + sx(i16)*4
        if mne in ("br", "brsl"):
            target = (i16 * 4 + addr) & 0x3FFFC
            # Lifter recovers brsl's link reg from raw bits; only print target.
            result.operands = f"0x{target:X}"
        elif mne in ("bra", "brasl"):
            target = (i16 * 4) & 0x3FFFC
            result.operands = f"0x{target:X}"
        elif mne in ("brz", "brnz", "brhz", "brhnz"):
            target = (i16 * 4 + addr) & 0x3FFFC
            result.operands = f"$r{rt}, 0x{target:X}"
        elif mne in ("lqr", "stqr"):
            target = (i16 * 4 + addr) & 0x3FFFC
            result.operands = f"$r{rt}, 0x{target:X}"
        elif mne in ("lqa", "stqa"):
            lsa = (i16 & 0x3FFF) << 4
            result.operands = f"$r{rt}, 0x{lsa:X}"
        elif mne == "il":
            # Sign-extended 16-bit immediate, splatted into word lanes.
            result.operands = f"$r{rt}, {i16}"
        elif mne in ("ilh", "ilhu", "iohl", "fsmbi"):
            result.operands = f"$r{rt}, 0x{i16 & 0xFFFF:X}"
        else:
            result.operands = f"$r{rt}, 0x{i16 & 0xFFFF:X}"
        return result

    # ---- Channel instructions (checked BEFORE RI10 to avoid wrch/shli clash) ----
    # wrch (op11=0x10D) shares op8=0x21 with shli; must be identified by op11.
    # Format: op11(11) | zeros(2) | channel(5) | zeros(2) | RT(7)
    # Channel address: bits 11-7 of instruction = (insn >> 7) & 0x1F
    _ch_field = (insn >> 7) & 0x1F
    if op11 == 0b00000001101:   # rdch RT, CA    op11=0x00D=13
        _ch = CHANNEL_NAMES.get(_ch_field, f"ch{_ch_field}")
        result.mnemonic = "rdch"
        result.operands = f"$r{rt}, {_ch}"
        return result
    if op11 == 0b00100001101:   # wrch CA, RT    op11=0x10D=269
        _ch = CHANNEL_NAMES.get(_ch_field, f"ch{_ch_field}")
        result.mnemonic = "wrch"
        result.operands = f"{_ch}, $r{rt}"
        return result
    if op11 == 0b00000001111:   # rchcnt RT, CA  op11=0x00F=15
        _ch = CHANNEL_NAMES.get(_ch_field, f"ch{_ch_field}")
        result.mnemonic = "rchcnt"
        result.operands = f"$r{rt}, {_ch}"
        return result

    # ---- RI10 format (8-bit opcode) ----
    if op8 in RI10_TABLE:
        mne = RI10_TABLE[op8]
        result.mnemonic = mne
        if mne in ("lqd", "stqd"):
            offset = i10 << 4  # quadword offset
            if offset < 0:
                disp = f"-0x{-offset:X}"
            else:
                disp = f"0x{offset:X}"
            result.operands = f"$r{rt}, {disp}($r{ra})"
        else:
            result.operands = f"$r{rt}, $r{ra}, {i10}"
        return result

    # ---- RR format (11-bit opcode) ----
    # Mnemonic sets used to pick the operand layout:
    #   RI7    — uses i7 (bits 14-20) instead of $rb: shift/rotate immediates
    #            living in the 11-bit opcode space.
    #   RR2    — $rt, $ra (no $rb)
    #   RR1A   — $ra only (bi, iret)
    #   NONE   — no operands (stop/nop/sync/lnop/dsync)
    RR_RI7 = {
        "roti", "rotmi", "rotmai", "shli", "rothi", "rothmi", "rotmahi", "shlhi",
        "rotqbii", "rotqmbii", "shlqbii", "rotqbyi", "rotqmbyi", "shlqbyi",
        "cbd", "chd", "cwd", "cdd",          # d-form constant generators
    }
    RR_TWO_OPERAND = {
        "clz", "cntb", "gb", "gbh", "gbb", "orx",
        "frest", "frsqest", "fsm", "fsmh", "fsmb",
        "xswd", "xshw", "xsbh", "fesd", "frds",
        "fscrrd", "fscrwr", "mfspr", "mtspr", "dftsv",
        "biz", "binz", "bihz", "bihnz",
        "bisl", "bisled", "hbr",
    }
    RR_BRANCH_RA = {"bi", "iret"}
    RR_NO_OPERANDS = {"stop", "lnop", "nop", "sync", "dsync"}
    if op11 in SPU_RR:
        mne = SPU_RR[op11]
        result.mnemonic = mne
        if mne in RR_NO_OPERANDS:
            return result
        if mne in RR_BRANCH_RA:
            result.operands = f"$r{ra}"
            return result
        if mne in RR_RI7:
            i7 = (insn >> 14) & 0x7F
            result.operands = f"$r{rt}, $r{ra}, {i7}"
            return result
        if mne in RR_TWO_OPERAND:
            result.operands = f"$r{rt}, $r{ra}"
            return result
        # Default: 3-register
        result.operands = f"$r{rt}, $r{ra}, $r{rb}"
        return result

    # ---- Fallback ----
    result.mnemonic = ".word"
    result.operands = f"0x{insn:08X}"
    return result

# ---------------------------------------------------------------------------
# Bulk disassembly
# ---------------------------------------------------------------------------

def disassemble_spu(data: bytes, base_addr: int = 0) -> list[SPUInstruction]:
    """Disassemble a buffer of SPU instructions (big-endian)."""
    instructions: list[SPUInstruction] = []
    for off in range(0, len(data) - 3, 4):
        raw = struct.unpack_from(">I", data, off)[0]
        addr = base_addr + off
        insn = spu_decode(raw, addr)
        instructions.append(insn)
    return instructions

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(description="SPU disassembler for PS3 binaries")
    parser.add_argument("input", help="Input binary or ELF file")
    parser.add_argument("--base", type=lambda x: int(x, 0), default=0,
                        help="Base address (hex ok)")
    parser.add_argument("--offset", type=lambda x: int(x, 0), default=0,
                        help="Start offset in file")
    parser.add_argument("--length", type=lambda x: int(x, 0), default=0,
                        help="Bytes to disassemble (0=all)")
    parser.add_argument("--json", action="store_true", help="Output as JSON")
    args = parser.parse_args()

    with open(args.input, "rb") as f:
        file_data = f.read()

    data = file_data[args.offset:]
    if args.length:
        data = data[:args.length]

    instructions = disassemble_spu(data, args.base)

    if args.json:
        out = [{"addr": f"0x{i.addr:08X}", "hex": f"{i.raw:08X}",
                "mnemonic": i.mnemonic, "operands": i.operands}
               for i in instructions]
        print(json.dumps(out, indent=2))
    else:
        for i in instructions:
            print(i)


if __name__ == "__main__":
    main()
