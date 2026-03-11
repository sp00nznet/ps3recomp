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

# RI18 format: opcd(7) i18(18) rt(7)
RI18_TABLE: dict[int, str] = {
    0b0100001: "ila",     # immediate load address (unsigned 18-bit)
    0b0100010: "ilhu",    # immediate load halfword upper
    0b0100000: "ilh",     # immediate load halfword
    0b0100011: "il",      # immediate load word (sign-extended 16-bit)
    0b0110000: "br",      # branch relative
    0b0110001: "brsl",    # branch relative and set link
    0b0110010: "bra",     # branch absolute
    0b0110011: "brasl",   # branch absolute and set link
    0b0110100: "brnz",    # branch if not zero
    0b0110101: "brz",     # branch if zero
    0b0110110: "brhnz",   # branch if halfword not zero
    0b0110111: "brhz",    # branch if halfword zero
}

# RI16 format: opcd(9) i16(16) rt(7)  -- actually 9-bit opcode
RI16_TABLE: dict[int, str] = {
    0b010000011: "iohl",  # immediate or halfword lower
    0b011000100: "lqa",   # load quadword (absolute address)
    0b001000100: "stqa",  # store quadword (absolute address)
    0b011000001: "hbra",  # hint for branch (absolute)
    0b011000010: "hbrr",  # hint for branch (relative)
}

# RI10 format: opcd(8) i10(10) ra(7) rt(7)
RI10_TABLE: dict[int, str] = {
    0b00110100: "lqd",    # load quadword d-form
    0b00100100: "stqd",   # store quadword d-form
    0b00011100: "ai",     # add word immediate
    0b00001100: "sfi",    # subtract from word immediate
    0b00011101: "ahi",    # add halfword immediate
    0b00001101: "sfhi",   # subtract from halfword immediate
    0b00010100: "andi",   # and word immediate
    0b00000100: "ori",    # or word immediate
    0b01000100: "xori",   # xor word immediate
    0b01111100: "ceqi",   # compare equal word immediate
    0b01111101: "ceqhi",  # compare equal halfword immediate
    0b01111110: "ceqbi",  # compare equal byte immediate
    0b01001100: "cgti",   # compare greater than word immediate
    0b01001101: "cgthi",  # compare greater than halfword immediate
    0b01001110: "cgtbi",  # compare greater than byte immediate
    0b01011100: "clgti",  # compare logical greater than word immediate
    0b01011101: "clgthi", # compare logical greater than halfword immediate
    0b01011110: "clgtbi", # compare logical greater than byte immediate
    0b01110100: "mpyi",   # multiply immediate
    0b01110101: "mpyui",  # multiply unsigned immediate
}

# RI8 format: opcd(9+) ... we handle these specially

# RR format: opcd(11) rb(7) ra(7) rt(7)
RR_TABLE: dict[int, str] = {
    # Integer arithmetic
    0b00011000000: "a",       # add word
    0b00001000000: "sf",      # subtract from word
    0b00011000001: "ah",      # add halfword
    0b00001000001: "sfh",     # subtract from halfword
    0b01101000000: "addx",    # add extended
    0b01101000001: "sfx",     # subtract from extended
    0b01011000100: "cg",      # carry generate
    0b01000000100: "bg",      # borrow generate
    0b01111000100: "mpy",     # multiply
    0b01111001100: "mpyu",    # multiply unsigned
    0b01111000101: "mpyh",    # multiply high
    0b01111001101: "mpyhhu",  # multiply high high unsigned
    0b01111000110: "mpys",    # multiply and shift right
    0b01111001110: "mpyhh",   # multiply high high

    # Logical
    0b00011000001: "and",     # NOTE: conflicts with ah; SPU uses separate encoding
    0b00001000001: "or",
    # Proper logical encodings:
    0b11000000000: "and_rr",  # placeholder -- see below
    # We use the correct 11-bit opcodes:
}

# Rebuild RR_TABLE with correct SPU 11-bit opcodes
RR_TABLE_CORRECT: dict[int, str] = {
    # Integer arithmetic
    0b00011000000: "a",
    0b00001000000: "sf",
    0b00011000001: "ah",
    0b00001000001: "sfh",
    0b01101000000: "addx",
    0b01000000000: "sfx",
    0b00011000010: "cg",
    0b00001000010: "bg",

    # Multiply
    0b01111000100: "mpy",
    0b01111001100: "mpyu",
    0b01111000101: "mpyh",
    0b01111001101: "mpyhhu",
    0b01111000110: "mpys",
    0b01111001110: "mpyhh",

    # Logical
    0b00011000001 + 0x400: "and_x",  # dummy; real opcodes below
    0b11000000000: "shlh",
    0b11000000001: "roth",
}

# Let's define the real SPU 11-bit RR opcodes properly
SPU_RR: dict[int, str] = {
    # Arithmetic
    0b00011000000: "a",
    0b00001000000: "sf",
    0b11001000000: "ah",
    0b00001001000: "sfh",
    0b01101000000: "addx",
    0b01000000000: "sfx",
    0b00011000010: "cg",
    0b00001000010: "bg",
    0b01010100101: "clz",
    0b01010110100: "cntb",
    0b00110110010: "gbh",
    0b00110110000: "gb",
    0b00110110001: "gbh",

    # Multiply
    0b01111000100: "mpy",
    0b01111001100: "mpyu",
    0b01111000101: "mpyh",
    0b01111001101: "mpyhhu",
    0b01111000110: "mpys",
    0b01111001110: "mpyhh",

    # Logical
    0b00011000001: "and",
    0b00001000001: "or",
    0b01001000001: "xor",
    0b00011001001: "andc",
    0b01011000001: "orc",
    0b00001001001: "nand",
    0b00001000001: "or",  # also mr (move register) when ra==rb
    0b01001000001: "xor",
    0b10011000001: "nor",
    0b01001000000: "orx",

    # Shift/Rotate
    0b00001011111: "shlqbii",
    0b00001011011: "shlqbi",
    0b00111011111: "rotqbii",
    0b00111011011: "rotqbi",
    0b00001011100: "shlqbybi",
    0b00111011100: "rotqbybi",
    0b00001111111: "shlqbyi",
    0b00111111111: "rotqbyi",
    0b00001111100: "shlqby",
    0b00111111100: "rotqby",
    0b00001011000: "shl",
    0b00001011001: "shlh",
    0b00001111000: "rot",
    0b00001111001: "roth",
    0b00001011100: "shlqbybi",
    0b00001111100: "rotqby",
    0b00001100000: "shr",     # shift right (placeholder)

    # Compare
    0b01111000000: "ceq",
    0b01111001000: "ceqh",
    0b01111010000: "ceqb",
    0b01001000000: "cgt",
    0b01001001000: "cgth",
    0b01001010000: "cgtb",
    0b01011000000: "clgt",
    0b01011001000: "clgth",
    0b01011010000: "clgtb",

    # Branch
    0b00110101000: "bi",      # branch indirect
    0b00110101001: "bisl",    # branch indirect and set link
    0b00100101000: "biz",     # branch indirect if zero
    0b00100101001: "binz",    # branch indirect if not zero
    0b00100101010: "bihz",    # branch indirect if halfword zero
    0b00100101011: "bihnz",   # branch indirect if halfword not zero

    # Hint for branch
    0b00110101100: "hbr",

    # Channel
    0b00000001101: "rdch",
    0b00000001101 + 0x400: "rchcnt",
    0b00000001100: "wrch",

    # Stop and signal
    0b00000000000: "stop",
    0b00000000001: "lnop",
    0b01000000001: "nop",
    0b00000000010: "sync",
    0b00000000011: "dsync",

    # Floating-point
    0b01011000100: "fa",      # floating add
    0b01011000101: "fs",      # floating subtract
    0b01011000110: "fm",      # floating multiply
    0b01111000010: "fceq",    # floating compare equal
    0b01011000010: "fcgt",    # floating compare greater than
    0b01110000010: "fcmeq",   # floating compare magnitude equal
    0b01010000010: "fcmgt",   # floating compare magnitude greater than
    0b00110111000: "fi",      # floating interpolate
    0b00110111010: "frest",   # floating reciprocal estimate
    0b00110111001: "frsqest", # floating reciprocal square root estimate
    0b01010110110: "fscrrd",  # floating-point status read
    0b01110110110: "fscrwr",  # floating-point status write
}

# Channel names
CHANNEL_NAMES = {
    0: "SPU_RdEventStat",
    1: "SPU_WrEventMask",
    2: "SPU_WrEventAck",
    3: "SPU_RdSigNotify1",
    4: "SPU_RdSigNotify2",
    7: "SPU_WrDec",
    8: "SPU_RdDec",
    11: "SPU_RdEventMask",
    13: "SPU_RdMachStat",
    14: "SPU_WrSRR0",
    15: "SPU_RdSRR0",
    21: "SPU_WrOuttrMbox",
    22: "SPU_RdInMbox",
    23: "SPU_WrOutMbox",
    24: "MFC_WrMSSyncReq",
    25: "MFC_RdTagMask",
    26: "MFC_LSA",
    27: "MFC_EAH",
    28: "MFC_EAL",
    29: "MFC_Size",
    30: "MFC_TagID",
    31: "MFC_Cmd",
    32: "MFC_WrTagMask",
    33: "MFC_WrTagUpdate",
    34: "MFC_RdTagStat",
    35: "MFC_RdListStallStat",
    36: "MFC_WrListStallAck",
    37: "MFC_RdAtomicStat",
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
    if op7 in RI18_TABLE:
        mne = RI18_TABLE[op7]
        result.mnemonic = mne

        if mne in ("br", "brsl"):
            target = sign_extend(i18, 18) * 4 + addr
            result.operands = f"0x{target & 0xFFFFFFFF:X}"
        elif mne in ("bra", "brasl"):
            target = i18 * 4
            result.operands = f"0x{target & 0xFFFFFFFF:X}"
        elif mne in ("brnz", "brz", "brhnz", "brhz"):
            target = sign_extend(i16, 16) * 4 + addr
            result.operands = f"$r{rt}, 0x{target & 0xFFFFFFFF:X}"
        elif mne == "ila":
            result.operands = f"$r{rt}, 0x{i18:X}"
        elif mne in ("il",):
            result.operands = f"$r{rt}, {sign_extend(i18 & 0xFFFF, 16)}"
        elif mne in ("ilh", "ilhu"):
            result.operands = f"$r{rt}, 0x{i18 & 0xFFFF:X}"
        else:
            result.operands = f"$r{rt}, 0x{i18:X}"
        return result

    # ---- RI16 format (9-bit opcode) ----
    if op9 in RI16_TABLE:
        mne = RI16_TABLE[op9]
        result.mnemonic = mne
        if mne in ("lqa", "stqa"):
            lsa = (i16 & 0x3FFF) << 4
            result.operands = f"$r{rt}, 0x{lsa:X}"
        elif mne in ("hbra", "hbrr"):
            result.operands = f"0x{i16 & 0xFFFF:X}, $r{rt}"
        elif mne == "iohl":
            result.operands = f"$r{rt}, 0x{i16 & 0xFFFF:X}"
        else:
            result.operands = f"$r{rt}, 0x{i16 & 0xFFFF:X}"
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

    # ---- Channel instructions (special RI7 variants) ----
    # rdch: opcode 00000001101, rt, ra encodes channel
    if op11 == 0b00000001101:
        ch = ra
        ch_name = CHANNEL_NAMES.get(ch, f"ch{ch}")
        result.mnemonic = "rdch"
        result.operands = f"$r{rt}, {ch_name}"
        return result

    if op11 == 0b00000001100:
        ch = ra
        ch_name = CHANNEL_NAMES.get(ch, f"ch{ch}")
        result.mnemonic = "wrch"
        result.operands = f"{ch_name}, $r{rt}"
        return result

    if op11 == 0b00000001111:
        ch = ra
        ch_name = CHANNEL_NAMES.get(ch, f"ch{ch}")
        result.mnemonic = "rchcnt"
        result.operands = f"$r{rt}, {ch_name}"
        return result

    # ---- RR format (11-bit opcode) ----
    if op11 in SPU_RR:
        mne = SPU_RR[op11]
        result.mnemonic = mne

        # Branches
        if mne in ("bi", "bisl"):
            result.operands = f"$r{ra}"
            return result
        if mne in ("biz", "binz", "bihz", "bihnz"):
            result.operands = f"$r{rt}, $r{ra}"
            return result
        if mne == "hbr":
            result.operands = f"$r{ra}, $r{rb}"
            return result

        # Stop/nop/sync
        if mne in ("stop", "lnop", "nop", "sync", "dsync"):
            return result

        # Single-operand
        if mne in ("clz", "cntb", "gb", "gbh", "fscrrd", "orx"):
            result.operands = f"$r{rt}, $r{ra}"
            return result

        # Default RR: 3-register
        result.operands = f"$r{rt}, $r{ra}, $r{rb}"
        return result

    # ---- Shift/rotate immediate forms (RI7 = 11-bit opcode, 7-bit immediate) ----
    # These have the format: opcd(11) i7(7) ra(7) rt(7)
    ri7_table: dict[int, str] = {
        0b00001111011: "shlqbii",  # shift left quadword by bits immediate
        0b00111111011: "rotqbii",  # rotate quadword by bits immediate
        0b00001111111: "shlqbyi",  # shift left quadword by bytes immediate
        0b00111111111: "rotqbyi",  # rotate quadword by bytes immediate
        0b00001111100: "shlqby",
        0b00111111100: "rotqby",
    }
    if op11 in ri7_table:
        i7 = rb  # bits 14-20
        result.mnemonic = ri7_table[op11]
        result.operands = f"$r{rt}, $r{ra}, {i7}"
        return result

    # Load/store indexed (RR format)
    lsx_table: dict[int, str] = {
        0b00111000100: "lqx",
        0b00101000100: "stqx",
    }
    if op11 in lsx_table:
        result.mnemonic = lsx_table[op11]
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
