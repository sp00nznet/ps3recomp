#!/usr/bin/env python3
"""
PowerPC 64-bit (PPU) disassembler for PS3 binaries.

Decodes 32-bit fixed-width, big-endian PowerPC instructions covering
integer arithmetic, logical, shift/rotate, load/store, branch, compare,
condition register, system, and common VMX/AltiVec operations.

Usage:
    python ppu_disasm.py <input_file> [--base ADDR] [--raw] [--functions]
"""

import argparse
import json
import os
import struct
import sys

# ---------------------------------------------------------------------------
# SPR name table
# ---------------------------------------------------------------------------
SPR_NAMES = {
    1: "XER",
    8: "LR",
    9: "CTR",
    18: "DSISR",
    19: "DAR",
    22: "DEC",
    25: "SDR1",
    26: "SRR0",
    27: "SRR1",
    268: "TBL",
    269: "TBU",
    272: "SPRG0",
    273: "SPRG1",
    274: "SPRG2",
    275: "SPRG3",
    287: "PVR",
    1008: "HID0",
    1009: "HID1",
    1013: "DABR",
}

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def sign_extend(value: int, bits: int) -> int:
    """Sign-extend *value* from *bits* width to Python int."""
    if value & (1 << (bits - 1)):
        value -= 1 << bits
    return value


def bits(insn: int, start: int, end: int) -> int:
    """Extract bits [start..end] inclusive (PowerPC bit numbering: 0=MSB)."""
    shift = 31 - end
    mask = (1 << (end - start + 1)) - 1
    return (insn >> shift) & mask


def bit(insn: int, pos: int) -> int:
    return (insn >> (31 - pos)) & 1

# ---------------------------------------------------------------------------
# Instruction decoder
# ---------------------------------------------------------------------------

class Instruction:
    """A single decoded PPU instruction."""

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
        line = f"{self.addr:08X}:  {hexb}  {self.mnemonic:<10s} {self.operands}"
        if self.comment:
            line += f"  ; {self.comment}"
        return line


def _cr_bit_name(bi: int) -> str:
    """Human-readable CR bit."""
    cr = bi >> 2
    sub = bi & 3
    subnames = ["lt", "gt", "eq", "so"]
    prefix = f"cr{cr}" if cr != 0 else ""
    return prefix + subnames[sub] if prefix else subnames[sub]


def _bo_hint(bo: int) -> str:
    """Simplified BO mnemonic component."""
    if bo & 0x14 == 0x14:
        return ""  # unconditional
    return ""


def _branch_cond_mnemonic(bo: int, bi: int) -> tuple[str, str]:
    """Return (suffix, cr_operand) for a conditional branch."""
    cr = bi >> 2
    cond = bi & 3
    cond_names_true = ["lt", "gt", "eq", "so"]
    cond_names_false = ["ge", "le", "ne", "ns"]

    # decrement CTR?
    decr = not (bo & 0x04)
    cond_flag = not (bo & 0x10)
    true_cond = bool(bo & 0x08)

    if not cond_flag and not decr:
        # unconditional (bo=0b10100)
        return "", ""

    if cond_flag and not decr:
        # simple conditional
        name = cond_names_true[cond] if true_cond else cond_names_false[cond]
        cr_str = f"cr{cr}, " if cr != 0 else ""
        return name, cr_str

    if decr and not cond_flag:
        name = "dnz" if bo & 0x02 == 0 else "dz"
        return name, ""

    # combined
    name = ("dnz" if bo & 0x02 == 0 else "dz")
    if true_cond:
        name += cond_names_true[cond]
    else:
        name += cond_names_false[cond]
    cr_str = f"cr{cr}, " if cr != 0 else ""
    return name, cr_str


def decode(insn: int, addr: int = 0) -> Instruction:
    """Decode a single 32-bit PowerPC instruction."""
    opcd = bits(insn, 0, 5)
    result = Instruction(addr=addr, raw=insn)

    # --- Major opcode switch ---

    # Branch I-form  (opcd 18)
    if opcd == 18:
        li = bits(insn, 6, 29)
        aa = bit(insn, 30)
        lk = bit(insn, 31)
        target = sign_extend(li << 2, 26)
        if not aa:
            target += addr
        else:
            target = target & 0xFFFFFFFF
        mne = "b"
        if lk:
            mne += "l"
        if aa:
            mne += "a"
        result.mnemonic = mne
        result.operands = f"0x{target & 0xFFFFFFFF:X}"
        return result

    # Branch conditional B-form  (opcd 16)
    if opcd == 16:
        bo = bits(insn, 6, 10)
        bi = bits(insn, 11, 15)
        bd = bits(insn, 16, 29)
        aa = bit(insn, 30)
        lk = bit(insn, 31)
        target = sign_extend(bd << 2, 16)
        if not aa:
            target += addr
        else:
            target = target & 0xFFFFFFFF

        cond_suf, cr_str = _branch_cond_mnemonic(bo, bi)
        mne = "b"
        if cond_suf:
            mne += cond_suf
        else:
            mne += "c"
        if lk:
            mne += "l"
        if aa:
            mne += "a"
        if cond_suf:
            result.mnemonic = mne
            result.operands = f"{cr_str}0x{target & 0xFFFFFFFF:X}"
        else:
            result.mnemonic = mne
            result.operands = f"{bo}, {bi}, 0x{target & 0xFFFFFFFF:X}"
        return result

    # Syscall (opcd 17)
    if opcd == 17:
        if bit(insn, 30):
            result.mnemonic = "sc"
            return result

    # Condition register XL-form (opcd 19)
    if opcd == 19:
        xo = bits(insn, 21, 30)
        bo = bits(insn, 6, 10)
        bi = bits(insn, 11, 15)
        lk = bit(insn, 31)

        if xo == 16:  # bclr
            cond_suf, cr_str = _branch_cond_mnemonic(bo, bi)
            if bo == 20 and bi == 0:
                mne = "blr"
                if lk:
                    mne += "l"  # blrl is rare but valid
                result.mnemonic = mne
            else:
                mne = "b" + cond_suf + "lr" if cond_suf else "bclr"
                if lk:
                    mne += "l"
                result.mnemonic = mne
                if not cond_suf:
                    result.operands = f"{bo}, {bi}"
                elif cr_str:
                    result.operands = cr_str.rstrip(", ")
            return result

        if xo == 528:  # bcctr
            cond_suf, cr_str = _branch_cond_mnemonic(bo, bi)
            if bo == 20 and bi == 0:
                mne = "bctr"
                if lk:
                    mne += "l"
                result.mnemonic = mne
            else:
                mne = "b" + cond_suf + "ctr" if cond_suf else "bcctr"
                if lk:
                    mne += "l"
                result.mnemonic = mne
                if not cond_suf:
                    result.operands = f"{bo}, {bi}"
                elif cr_str:
                    result.operands = cr_str.rstrip(", ")
            return result

        # CR logical operations
        bt = bits(insn, 6, 10)
        ba = bits(insn, 11, 15)
        bb = bits(insn, 16, 20)
        cr_ops = {
            257: "crand", 449: "cror", 193: "crxor",
            33: "crnand", 225: "crnor", 289: "creqv",
            129: "crandc", 417: "crorc",
            0: "mcrf",
        }
        if xo in cr_ops:
            result.mnemonic = cr_ops[xo]
            if xo == 0:
                result.operands = f"cr{bt >> 2}, cr{ba >> 2}"
            else:
                result.operands = f"{bt}, {ba}, {bb}"
            return result

        if xo == 50:  # rfi
            result.mnemonic = "rfi"
            return result

        if xo == 150:  # isync
            result.mnemonic = "isync"
            return result

    # Integer arithmetic / logical / load-store by major opcode
    rd = bits(insn, 6, 10)
    ra = bits(insn, 11, 15)
    si = sign_extend(bits(insn, 16, 31), 16)
    ui = bits(insn, 16, 31)

    # addi (14), addis (15)
    if opcd == 14:
        if ra == 0:
            result.mnemonic = "li"
            result.operands = f"r{rd}, {si}"
        else:
            result.mnemonic = "addi"
            result.operands = f"r{rd}, r{ra}, {si}"
        return result

    if opcd == 15:
        if ra == 0:
            result.mnemonic = "lis"
            result.operands = f"r{rd}, 0x{ui:X}"
        else:
            result.mnemonic = "addis"
            result.operands = f"r{rd}, r{ra}, 0x{ui:X}"
        return result

    # ori (24), oris (25), xori (26), xoris (27), andi. (28), andis. (29)
    if opcd == 24:
        if rd == 0 and ra == 0 and ui == 0:
            result.mnemonic = "nop"
        else:
            result.mnemonic = "ori"
            result.operands = f"r{ra}, r{rd}, 0x{ui:X}"
        return result
    if opcd == 25:
        result.mnemonic = "oris"
        result.operands = f"r{ra}, r{rd}, 0x{ui:X}"
        return result
    if opcd == 26:
        result.mnemonic = "xori"
        result.operands = f"r{ra}, r{rd}, 0x{ui:X}"
        return result
    if opcd == 27:
        result.mnemonic = "xoris"
        result.operands = f"r{ra}, r{rd}, 0x{ui:X}"
        return result
    if opcd == 28:
        result.mnemonic = "andi."
        result.operands = f"r{ra}, r{rd}, 0x{ui:X}"
        return result
    if opcd == 29:
        result.mnemonic = "andis."
        result.operands = f"r{ra}, r{rd}, 0x{ui:X}"
        return result

    # mulli (7)
    if opcd == 7:
        result.mnemonic = "mulli"
        result.operands = f"r{rd}, r{ra}, {si}"
        return result

    # subfic (8)
    if opcd == 8:
        result.mnemonic = "subfic"
        result.operands = f"r{rd}, r{ra}, {si}"
        return result

    # cmpli (10), cmpi (11)
    if opcd == 10:
        bf = bits(insn, 6, 8)
        l_bit = bit(insn, 10)
        result.mnemonic = "cmpldi" if l_bit else "cmplwi"
        cr_str = f"cr{bf}, " if bf else ""
        result.operands = f"{cr_str}r{ra}, 0x{ui:X}"
        return result
    if opcd == 11:
        bf = bits(insn, 6, 8)
        l_bit = bit(insn, 10)
        result.mnemonic = "cmpdi" if l_bit else "cmpwi"
        cr_str = f"cr{bf}, " if bf else ""
        result.operands = f"{cr_str}r{ra}, {si}"
        return result

    # addic (12), addic. (13)
    if opcd == 12:
        result.mnemonic = "addic"
        result.operands = f"r{rd}, r{ra}, {si}"
        return result
    if opcd == 13:
        result.mnemonic = "addic."
        result.operands = f"r{rd}, r{ra}, {si}"
        return result

    # Load/store instructions
    load_store = {
        32: ("lwz", True), 33: ("lwzu", True),
        34: ("lbz", True), 35: ("lbzu", True),
        36: ("stw", False), 37: ("stwu", False),
        38: ("stb", False), 39: ("stbu", False),
        40: ("lhz", True), 41: ("lhzu", True),
        42: ("lha", True), 43: ("lhau", True),
        44: ("sth", False), 45: ("sthu", False),
        46: ("lmw", True), 47: ("stmw", False),
        48: ("lfs", True), 49: ("lfsu", True),
        50: ("lfd", True), 51: ("lfdu", True),
        52: ("stfs", False), 53: ("stfsu", False),
        54: ("stfd", False), 55: ("stfdu", False),
    }
    if opcd in load_store:
        mne, is_load = load_store[opcd]
        d = sign_extend(bits(insn, 16, 31), 16)
        if d < 0:
            disp = f"-0x{-d:X}"
        else:
            disp = f"0x{d:X}"
        result.mnemonic = mne
        result.operands = f"r{rd}, {disp}(r{ra})"
        return result

    # DS-form loads/stores (opcd 58 = ld/ldu/lwa, opcd 62 = std/stdu)
    if opcd == 58:
        ds = sign_extend(bits(insn, 16, 29) << 2, 16)
        sub = bits(insn, 30, 31)
        mne = {0: "ld", 1: "ldu", 2: "lwa"}.get(sub, "ld?")
        disp = f"-0x{-ds:X}" if ds < 0 else f"0x{ds:X}"
        result.mnemonic = mne
        result.operands = f"r{rd}, {disp}(r{ra})"
        return result
    if opcd == 62:
        ds = sign_extend(bits(insn, 16, 29) << 2, 16)
        sub = bits(insn, 30, 31)
        mne = {0: "std", 1: "stdu"}.get(sub, "std?")
        disp = f"-0x{-ds:X}" if ds < 0 else f"0x{ds:X}"
        result.mnemonic = mne
        result.operands = f"r{rd}, {disp}(r{ra})"
        return result

    # Rotate / shift (opcd 21 = rlwinm, 23 = rlwnm, 20 = rlwimi)
    if opcd == 21:
        sh = bits(insn, 16, 20)
        mb = bits(insn, 21, 25)
        me = bits(insn, 26, 30)
        rc = bit(insn, 31)
        mne = "rlwinm" + ("." if rc else "")
        result.mnemonic = mne
        result.operands = f"r{ra}, r{rd}, {sh}, {mb}, {me}"
        return result
    if opcd == 23:
        rb = bits(insn, 16, 20)
        mb = bits(insn, 21, 25)
        me = bits(insn, 26, 30)
        rc = bit(insn, 31)
        mne = "rlwnm" + ("." if rc else "")
        result.mnemonic = mne
        result.operands = f"r{ra}, r{rd}, r{rb}, {mb}, {me}"
        return result
    if opcd == 20:
        sh = bits(insn, 16, 20)
        mb = bits(insn, 21, 25)
        me = bits(insn, 26, 30)
        rc = bit(insn, 31)
        mne = "rlwimi" + ("." if rc else "")
        result.mnemonic = mne
        result.operands = f"r{ra}, r{rd}, {sh}, {mb}, {me}"
        return result

    # 64-bit rotate (opcd 30 = rldic*/rldc*)
    if opcd == 30:
        rc = bit(insn, 31)

        # The sub-opcode encoding is different for immediate vs register forms:
        # Bits 27-30 for immediate forms (rldicl, rldicr, rldic, rldimi)
        # Bits 27-29 for register forms (rldcl, rldcr) where bit 30 is part of sh
        sub4 = bits(insn, 27, 30)  # 4-bit sub-opcode
        sub3 = bits(insn, 27, 29)  # 3-bit sub-opcode (for register forms)

        rld_imm_ops = {0: "rldicl", 1: "rldicr", 2: "rldic", 3: "rldimi"}
        if sub4 in rld_imm_ops:
            sh = (bits(insn, 16, 20) | (bit(insn, 30) << 5))
            mb = (bits(insn, 21, 25) | (bit(insn, 26) << 5))
            mne = rld_imm_ops[sub4] + ("." if rc else "")
            result.mnemonic = mne
            result.operands = f"r{ra}, r{rd}, {sh}, {mb}"
        elif sub3 == 4:  # rldcl (sub bits 27-29 = 100)
            rb = bits(insn, 16, 20)
            mb = (bits(insn, 21, 25) | (bit(insn, 26) << 5))
            mne = "rldcl" + ("." if rc else "")
            result.mnemonic = mne
            result.operands = f"r{ra}, r{rd}, r{rb}, {mb}"
        elif sub3 == 5:  # rldcr (sub bits 27-29 = 101)
            rb = bits(insn, 16, 20)
            me = (bits(insn, 21, 25) | (bit(insn, 26) << 5))
            mne = "rldcr" + ("." if rc else "")
            result.mnemonic = mne
            result.operands = f"r{ra}, r{rd}, r{rb}, {me}"
        else:
            sh = (bits(insn, 16, 20) | (bit(insn, 30) << 5))
            mb = (bits(insn, 21, 25) | (bit(insn, 26) << 5))
            result.mnemonic = "rld??"
            result.operands = f"r{ra}, r{rd}, {sh}, {mb}"
        return result

    # Extended opcode 31 (X-form, XO-form, XFX-form)
    if opcd == 31:
        xo_full = bits(insn, 21, 30)
        xo_9 = bits(insn, 22, 30)
        rb = bits(insn, 16, 20)
        rc = bit(insn, 31)
        oe = bit(insn, 21)

        # --- XO-form integer arithmetic (xo_9) ---
        # Instructions with only 2 register operands (rD, rA — no rB)
        xo_arith_2op = {
            202: "addze", 234: "addme",
            200: "subfze", 232: "subfme",
            104: "neg",
        }
        # Instructions with 3 register operands (rD, rA, rB)
        xo_arith_3op = {
            266: "add", 10: "addc", 138: "adde",
            40: "subf", 8: "subfc", 136: "subfe",
            235: "mullw", 75: "mulhw", 11: "mulhwu",
            491: "divw", 459: "divwu",
            233: "mulld", 73: "mulhd",
            489: "divd", 457: "divdu",
        }
        if xo_9 in xo_arith_2op:
            mne = xo_arith_2op[xo_9]
            if oe: mne += "o"
            if rc: mne += "."
            result.mnemonic = mne
            result.operands = f"r{rd}, r{ra}"
            return result
        if xo_9 in xo_arith_3op:
            mne = xo_arith_3op[xo_9]
            if oe: mne += "o"
            if rc: mne += "."
            result.mnemonic = mne
            result.operands = f"r{rd}, r{ra}, r{rb}"
            return result

        # --- X-form logical ---
        x_logical = {
            28: "and", 444: "or", 316: "xor",
            476: "nand", 124: "nor",
            284: "eqv", 60: "andc", 412: "orc",
            954: "extsb", 922: "extsh", 986: "extsw",
            26: "cntlzw", 58: "cntlzd",
        }
        if xo_full in x_logical:
            mne = x_logical[xo_full]
            if rc:
                mne += "."
            result.mnemonic = mne
            if xo_full in (954, 922, 986, 26, 58):
                result.operands = f"r{ra}, r{rd}"
            else:
                result.operands = f"r{ra}, r{rd}, r{rb}"
            return result

        # --- Shifts ---
        x_shift = {
            24: "slw", 536: "srw", 792: "sraw", 824: "srawi",
            27: "sld", 539: "srd", 794: "srad", 826: "sradi",
        }
        if xo_full in x_shift:
            mne = x_shift[xo_full]
            if rc:
                mne += "."
            result.mnemonic = mne
            if xo_full in (824, 826):
                result.operands = f"r{ra}, r{rd}, {rb}"
            else:
                result.operands = f"r{ra}, r{rd}, r{rb}"
            return result

        # --- Compare ---
        if xo_full == 0:  # cmp
            bf = bits(insn, 6, 8)
            l_bit = bit(insn, 10)
            mne = "cmpd" if l_bit else "cmpw"
            cr_str = f"cr{bf}, " if bf else ""
            result.mnemonic = mne
            result.operands = f"{cr_str}r{ra}, r{rb}"
            return result
        if xo_full == 32:  # cmpl
            bf = bits(insn, 6, 8)
            l_bit = bit(insn, 10)
            mne = "cmpld" if l_bit else "cmplw"
            cr_str = f"cr{bf}, " if bf else ""
            result.mnemonic = mne
            result.operands = f"{cr_str}r{ra}, r{rb}"
            return result

        # --- Load/store indexed ---
        x_ldst = {
            23: ("lwzx", False), 55: ("lwzux", False),
            87: ("lbzx", False), 119: ("lbzux", False),
            151: ("stwx", False), 183: ("stwux", False),
            215: ("stbx", False), 247: ("stbux", False),
            279: ("lhzx", False), 311: ("lhzux", False),
            343: ("lhax", False), 375: ("lhaux", False),
            407: ("sthx", False), 439: ("sthux", False),
            21: ("ldx", False), 53: ("ldux", False),
            149: ("stdx", False), 181: ("stdux", False),
            20: ("lwarx", False), 150: ("stwcx.", True),
            84: ("ldarx", False), 214: ("stdcx.", True),
            535: ("lfsx", False), 567: ("lfsux", False),
            599: ("lfdx", False), 631: ("lfdux", False),
            663: ("stfsx", False), 695: ("stfsux", False),
            727: ("stfdx", False), 759: ("stfdux", False),
        }
        if xo_full in x_ldst:
            mne, has_dot = x_ldst[xo_full]
            result.mnemonic = mne
            result.operands = f"r{rd}, r{ra}, r{rb}"
            return result

        # --- SPR operations ---
        if xo_full == 339:  # mfspr
            spr_raw = bits(insn, 11, 20)
            spr = ((spr_raw & 0x1F) << 5) | ((spr_raw >> 5) & 0x1F)
            spr_name = SPR_NAMES.get(spr, f"spr{spr}")
            if spr == 8:
                result.mnemonic = "mflr"
                result.operands = f"r{rd}"
            elif spr == 9:
                result.mnemonic = "mfctr"
                result.operands = f"r{rd}"
            else:
                result.mnemonic = "mfspr"
                result.operands = f"r{rd}, {spr_name}"
            return result

        if xo_full == 467:  # mtspr
            spr_raw = bits(insn, 11, 20)
            spr = ((spr_raw & 0x1F) << 5) | ((spr_raw >> 5) & 0x1F)
            spr_name = SPR_NAMES.get(spr, f"spr{spr}")
            if spr == 8:
                result.mnemonic = "mtlr"
                result.operands = f"r{rd}"
            elif spr == 9:
                result.mnemonic = "mtctr"
                result.operands = f"r{rd}"
            else:
                result.mnemonic = "mtspr"
                result.operands = f"{spr_name}, r{rd}"
            return result

        # mfcr, mtcrf
        if xo_full == 19:  # mfcr
            result.mnemonic = "mfcr"
            result.operands = f"r{rd}"
            return result
        if xo_full == 144:  # mtcrf
            crm = bits(insn, 12, 19)
            if crm == 0xFF:
                result.mnemonic = "mtcr"
                result.operands = f"r{rd}"
            else:
                result.mnemonic = "mtcrf"
                result.operands = f"0x{crm:02X}, r{rd}"
            return result

        # --- Sync / cache / misc ---
        misc_x = {
            598: "sync", 854: "eieio", 278: "dcbt", 246: "dcbtst",
            86: "dcbf", 54: "dcbst", 982: "icbi", 758: "dcba",
            1014: "dcbz",
        }
        if xo_full in misc_x:
            result.mnemonic = misc_x[xo_full]
            if xo_full in (278, 246, 86, 54, 982, 758, 1014):
                result.operands = f"r{ra}, r{rb}"
            return result

        # twi trap
        if xo_full == 4:
            result.mnemonic = "tw"
            result.operands = f"{rd}, r{ra}, r{rb}"
            return result

        # --- Byte-reverse loads/stores ---
        if xo_full == 534:  # lwbrx (Load Word Byte-Reverse Indexed)
            result.mnemonic = "lwbrx"
            result.operands = f"r{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 662:  # stwbrx (Store Word Byte-Reverse Indexed)
            result.mnemonic = "stwbrx"
            result.operands = f"r{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 790:  # lhbrx (Load Halfword Byte-Reverse Indexed)
            result.mnemonic = "lhbrx"
            result.operands = f"r{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 918:  # sthbrx (Store Halfword Byte-Reverse Indexed)
            result.mnemonic = "sthbrx"
            result.operands = f"r{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 827:  # lhaux (Load Halfword Algebraic with Update Indexed)
            result.mnemonic = "lhaux"
            result.operands = f"r{rd}, r{ra}, r{rb}"
            return result

        # --- Load/store algebraic ---
        if xo_full == 341:  # lwax (Load Word Algebraic Indexed)
            result.mnemonic = "lwax"
            result.operands = f"r{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 373:  # lwaux (Load Word Algebraic with Update Indexed)
            result.mnemonic = "lwaux"
            result.operands = f"r{rd}, r{ra}, r{rb}"
            return result

        # --- Move from/to timebase ---
        if xo_full == 371:  # mftb (Move From Time Base)
            tbr_raw = bits(insn, 11, 20)
            tbr = ((tbr_raw & 0x1F) << 5) | ((tbr_raw >> 5) & 0x1F)
            if tbr == 268:
                result.mnemonic = "mftb"
            elif tbr == 269:
                result.mnemonic = "mftbu"
            else:
                result.mnemonic = "mftb"
            result.operands = f"r{rd}"
            return result

        # --- String word load/store ---
        if xo_full == 597:  # lswi (Load String Word Immediate)
            nb = rb  # nb field reuses rb position
            result.mnemonic = "lswi"
            result.operands = f"r{rd}, r{ra}, {nb}"
            return result
        if xo_full == 725:  # stswi (Store String Word Immediate)
            nb = rb
            result.mnemonic = "stswi"
            result.operands = f"r{rd}, r{ra}, {nb}"
            return result

        # --- VMX scalar helpers ---
        if xo_full == 6:  # lvsl (Load Vector for Shift Left)
            result.mnemonic = "lvsl"
            result.operands = f"v{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 38:  # lvsr (Load Vector for Shift Right)
            result.mnemonic = "lvsr"
            result.operands = f"v{rd}, r{ra}, r{rb}"
            return result

        # Fall through – unknown ext opcode 31
        result.mnemonic = f"op31_x{xo_full}"
        result.operands = f"r{rd}, r{ra}, r{rb}"
        return result

    # Extended opcode 63 (floating-point)
    if opcd == 63:
        xo_full = bits(insn, 21, 30)
        xo_5 = bits(insn, 26, 30)
        frd = rd
        fra = ra
        frb = bits(insn, 16, 20)
        frc = bits(insn, 21, 25)
        rc = bit(insn, 31)

        fp_a = {
            21: "fadd", 20: "fsub", 25: "fmul", 18: "fdiv",
            29: "fmadd", 28: "fmsub", 31: "fnmadd", 30: "fnmsub",
            23: "fsel", 22: "fsqrt",
        }
        if xo_5 in fp_a:
            mne = fp_a[xo_5]
            if rc:
                mne += "."
            result.mnemonic = mne
            if xo_5 == 25:  # fmul: frd, fra, frc
                result.operands = f"f{frd}, f{fra}, f{frc}"
            elif xo_5 in (29, 28, 31, 30):  # fmadd etc: frd, fra, frc, frb
                result.operands = f"f{frd}, f{fra}, f{frc}, f{frb}"
            elif xo_5 == 23:  # fsel
                result.operands = f"f{frd}, f{fra}, f{frc}, f{frb}"
            else:
                result.operands = f"f{frd}, f{fra}, f{frb}"
            return result

        fp_x = {
            0: "fcmpu", 32: "fcmpo",
            12: "frsp", 14: "fctiw", 15: "fctiwz",
            846: "fctid", 847: "fctidz", 814: "fcfid",
            40: "fneg", 72: "fmr", 264: "fabs", 136: "fnabs",
            64: "mcrfs",
            583: "mffs", 711: "mtfsf",
        }
        if xo_full in fp_x:
            mne = fp_x[xo_full]
            if rc:
                mne += "."
            result.mnemonic = mne
            if xo_full in (0, 32):
                bf = bits(insn, 6, 8)
                result.operands = f"cr{bf}, f{fra}, f{frb}"
            elif xo_full in (40, 72, 264, 136):
                result.operands = f"f{frd}, f{frb}"
            elif xo_full == 583:
                result.operands = f"f{frd}"
            elif xo_full == 711:
                fm = bits(insn, 7, 14)
                result.operands = f"0x{fm:02X}, f{frb}"
            else:
                result.operands = f"f{frd}, f{frb}"
            return result

    # Extended opcode 59 (single-precision FP)
    if opcd == 59:
        xo_5 = bits(insn, 26, 30)
        frd = rd
        fra = ra
        frb = bits(insn, 16, 20)
        frc = bits(insn, 21, 25)
        rc = bit(insn, 31)
        fps = {21: "fadds", 20: "fsubs", 25: "fmuls", 18: "fdivs",
               29: "fmadds", 28: "fmsubs", 31: "fnmadds", 30: "fnmsubs",
               22: "fsqrts", 24: "fres"}
        if xo_5 in fps:
            mne = fps[xo_5]
            if rc:
                mne += "."
            result.mnemonic = mne
            if xo_5 == 25:
                result.operands = f"f{frd}, f{fra}, f{frc}"
            elif xo_5 in (29, 28, 31, 30):
                result.operands = f"f{frd}, f{fra}, f{frc}, f{frb}"
            else:
                result.operands = f"f{frd}, f{fra}, f{frb}"
            return result

    # VMX / AltiVec (opcd 4)
    if opcd == 4:
        vd = rd
        va = ra
        vb = bits(insn, 16, 20)
        vc = bits(insn, 21, 25)
        xo_full = bits(insn, 21, 30)
        xo_6 = bits(insn, 26, 31)

        # VA-form (6-bit xo at bits 26-31)
        vmx_va = {
            46: "vmaddfp", 47: "vnmsubfp",
            32: "vmhaddshs", 33: "vmhraddshs",
            34: "vmladduhm",
            37: "vmsumubm", 38: "vmsummbm",
            40: "vmsumuhm", 41: "vmsumuhs",
            44: "vmsumshm", 45: "vmsumshs",
            36: "vperm", 43: "vsel",
            42: "vsldoi",
        }
        if xo_6 in vmx_va:
            result.mnemonic = vmx_va[xo_6]
            if xo_6 == 42:  # vsldoi — 4th operand is shift count, not vector reg
                result.operands = f"v{vd}, v{va}, v{vb}, {vc}"
            else:
                result.operands = f"v{vd}, v{va}, v{vb}, v{vc}"
            return result

        # VX-form (11-bit xo in bits 21-30, but bit 21 = Rc for compares)
        # For non-compare instructions, xo is bits 21-30 (10 bits effectively)
        # For compare instructions, bit 21 is Rc flag

        # Compare instructions (bit 21 = Rc): strip Rc to get base xo
        xo_10 = bits(insn, 22, 30)  # 9-bit sub-opcode ignoring Rc
        vmx_cmp_rc = bit(insn, 21)  # Rc bit for compare instructions

        vmx_cmp = {
            6: "vcmpequb", 70: "vcmpequh", 134: "vcmpequw",
            198: "vcmpeqfp", 326: "vcmpbfp", 454: "vcmpgefp",
            710: "vcmpgtfp",
            518: "vcmpgtsb", 582: "vcmpgtsh", 646: "vcmpgtsw",
            774: "vcmpgtub", 838: "vcmpgtuh", 902: "vcmpgtuw",
        }
        # Check if this is a compare (xo_10 matches a compare, with or without Rc)
        cmp_xo = xo_10 | (vmx_cmp_rc << 9)  # reconstruct with Rc=0 to check
        # Actually, the compare xo values above are the FULL 10-bit values (without Rc)
        # So check xo_full with Rc masked off: xo_full & ~(1 << 9) = bits 22-30
        base_cmp_xo = xo_full & ~(1 << 9)  # strip Rc bit
        if base_cmp_xo in vmx_cmp:
            mne = vmx_cmp[base_cmp_xo]
            if vmx_cmp_rc:
                mne += "."
            result.mnemonic = mne
            result.operands = f"v{vd}, v{va}, v{vb}"
            return result

        # Non-compare VX instructions (full 11-bit xo, no Rc ambiguity)
        vmx_vx = {
            # Float arithmetic
            10: "vaddfp", 74: "vsubfp",
            842: "vrefp", 778: "vrsqrtefp",
            266: "vmaxfp", 1034: "vminfp",

            # Integer add/sub
            0: "vaddubm", 64: "vadduhm", 128: "vadduwm",
            1024: "vsububm", 1088: "vsubuhm", 1152: "vsubuwm",
            512: "vaddubs", 576: "vadduhs", 640: "vadduws",
            768: "vaddsbs", 832: "vaddshs", 896: "vaddsws",
            1536: "vsububs", 1600: "vsubuhs", 1664: "vsubuws",
            1792: "vsubsbs", 1856: "vsubshs", 1920: "vsubsws",

            # Integer min/max
            256: "vmaxub", 320: "vmaxuh", 384: "vmaxuw",
            258: "vmaxsb", 322: "vmaxsh", 386: "vmaxsw",
            514: "vminub", 578: "vminuh", 642: "vminuw",
            770: "vminsb", 834: "vminsh", 898: "vminsw",

            # Logical
            1028: "vand", 1092: "vandc", 1220: "vor", 1284: "vxor",
            1348: "vnor",

            # Shift
            260: "vslb", 324: "vslh", 388: "vslw",
            516: "vsrb", 580: "vsrh", 644: "vsrw",
            772: "vsrab", 836: "vsrah", 900: "vsraw",

            # Splat
            1098: "vspltb", 1162: "vsplth", 1226: "vspltw",
            780: "vspltisb", 844: "vspltish", 908: "vspltisw",

            # Merge
            12: "vmrghb", 76: "vmrghh", 140: "vmrghw",
            268: "vmrglb", 332: "vmrglh", 396: "vmrglw",

            # Convert
            394: "vcfsx", 330: "vcfux",
            970: "vctsxs", 906: "vctuxs",

            # Pack/unpack
            782: "vpkuhum", 846: "vpkuwum",
            14: "vpkuhus", 78: "vpkuwus",
            398: "vpkshus", 462: "vpkswus",
            270: "vpkshss", 334: "vpkswss",
            526: "vupkhsb", 590: "vupkhsh",
            654: "vupklsb", 718: "vupklsh",

            # Rotate
            4: "vrlb", 68: "vrlh", 132: "vrlw",

            # Average
            1026: "vavgub", 1090: "vavguh", 1154: "vavguw",
            1282: "vavgsb", 1346: "vavgsh", 1410: "vavgsw",

            # Multiply
            8: "vmuloub", 72: "vmulouh",
            264: "vmulosb", 328: "vmulosh",
            520: "vmuleub", 584: "vmuleuh",
            776: "vmulesb", 840: "vmulesh",

            # Sum across
            1032: "vsumsws", 1672: "vsum2sws",
            1544: "vsum4ubs", 1608: "vsum4sbs", 1800: "vsum4shs",
        }
        if xo_full in vmx_vx:
            result.mnemonic = vmx_vx[xo_full]
            result.operands = f"v{vd}, v{va}, v{vb}"
            return result

        # lvx / stvx (X-form under opcode 31 actually, but some are opcd 4)
        # Fallback for opcode 4
        result.mnemonic = f"vmx_x{xo_full}"
        result.operands = f"v{vd}, v{va}, v{vb}"
        return result

    # lvx/stvx are actually under opcode 31
    if opcd == 31:
        xo_full = bits(insn, 21, 30)
        if xo_full == 103:  # lvx
            result.mnemonic = "lvx"
            result.operands = f"v{rd}, r{ra}, r{bits(insn, 16, 20)}"
            return result
        if xo_full == 231:  # stvx
            result.mnemonic = "stvx"
            result.operands = f"v{rd}, r{ra}, r{bits(insn, 16, 20)}"
            return result
        if xo_full == 359:  # lvxl
            result.mnemonic = "lvxl"
            result.operands = f"v{rd}, r{ra}, r{bits(insn, 16, 20)}"
            return result
        if xo_full == 487:  # stvxl
            result.mnemonic = "stvxl"
            result.operands = f"v{rd}, r{ra}, r{bits(insn, 16, 20)}"
            return result

    # twi (3)
    if opcd == 3:
        to = rd
        result.mnemonic = "twi"
        result.operands = f"{to}, r{ra}, {si}"
        return result

    # tdi (2)
    if opcd == 2:
        to = rd
        result.mnemonic = "tdi"
        result.operands = f"{to}, r{ra}, {si}"
        return result

    # Unknown
    result.mnemonic = f".word"
    result.operands = f"0x{insn:08X}"
    return result


# ---------------------------------------------------------------------------
# Bulk disassembly
# ---------------------------------------------------------------------------

def disassemble_bytes(data: bytes, base_addr: int = 0,
                      big_endian: bool = True) -> list[Instruction]:
    """Disassemble a block of bytes into instructions."""
    fmt = ">I" if big_endian else "<I"
    instructions: list[Instruction] = []
    for off in range(0, len(data) - 3, 4):
        raw = struct.unpack_from(fmt, data, off)[0]
        addr = base_addr + off
        insn = decode(raw, addr)
        instructions.append(insn)
    return instructions


# ---------------------------------------------------------------------------
# Function boundary detection helpers
# ---------------------------------------------------------------------------

def detect_functions(instructions: list[Instruction]) -> list[dict]:
    """Detect function boundaries using prologue/epilogue patterns.

    Looks for standard PPU function prologues:
        mflr  r0
        stw/std r0, X(r1)
        stwu/stdu r1, -Y(r1)

    And epilogues:
        lwz/ld r0, X(r1)
        mtlr  r0
        blr
    """
    functions: list[dict] = []
    n = len(instructions)
    current_func_start = None

    for i, insn in enumerate(instructions):
        # Detect prologue: mflr r0
        if insn.mnemonic == "mflr" and insn.operands == "r0":
            # check next 1-3 instructions for stw/std r0, X(r1) and stwu/stdu r1
            for j in range(1, min(4, n - i)):
                nxt = instructions[i + j]
                if nxt.mnemonic in ("stw", "std") and "r0," in nxt.operands and "(r1)" in nxt.operands:
                    current_func_start = insn.addr
                    break

        # Detect blr (end of function)
        if insn.mnemonic == "blr" and current_func_start is not None:
            functions.append({
                "start": current_func_start,
                "end": insn.addr + 4,
                "size": insn.addr + 4 - current_func_start,
            })
            current_func_start = None

        # Also detect bctr as possible function end (tail call / switch)
        if insn.mnemonic == "bctr" and current_func_start is not None:
            # only end if next instruction looks like a new prologue
            if i + 1 < n and instructions[i + 1].mnemonic == "mflr":
                functions.append({
                    "start": current_func_start,
                    "end": insn.addr + 4,
                    "size": insn.addr + 4 - current_func_start,
                })
                current_func_start = None

    return functions


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(description="PPU (PowerPC 64) disassembler for PS3 binaries")
    parser.add_argument("input", help="Input ELF file or raw binary")
    parser.add_argument("--base", type=lambda x: int(x, 0), default=0,
                        help="Base address for raw binary (hex ok)")
    parser.add_argument("--raw", action="store_true",
                        help="Treat input as raw binary (not ELF)")
    parser.add_argument("--functions", action="store_true",
                        help="Detect and list function boundaries")
    parser.add_argument("--json", action="store_true",
                        help="Output as JSON instead of text")
    parser.add_argument("--offset", type=lambda x: int(x, 0), default=0,
                        help="Start offset within file (for raw mode)")
    parser.add_argument("--length", type=lambda x: int(x, 0), default=0,
                        help="Number of bytes to disassemble (0=all)")
    parser.add_argument("--little-endian", action="store_true",
                        help="Decode as little-endian")
    args = parser.parse_args()

    with open(args.input, "rb") as f:
        file_data = f.read()

    big_endian = not args.little_endian
    base_addr = args.base

    if args.raw:
        data = file_data[args.offset:]
        if args.length:
            data = data[:args.length]
    else:
        # Try to parse as ELF and disassemble executable segments
        try:
            sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
            from elf_parser import ELFFile, PT_LOAD
            elf = ELFFile(args.input)
            elf.load()
            big_endian = elf.elf_header.big_endian

            # Collect all executable segments
            segments = []
            for ph in elf.program_headers:
                if ph.p_type == PT_LOAD and (ph.p_flags & 1):  # PF_X
                    seg_data = elf.get_segment_data(elf.program_headers.index(ph))
                    segments.append((ph.p_vaddr, seg_data))

            if not segments:
                # fallback: disassemble first PT_LOAD
                for ph in elf.program_headers:
                    if ph.p_type == PT_LOAD and ph.p_filesz > 0:
                        seg_data = elf.get_segment_data(elf.program_headers.index(ph))
                        segments.append((ph.p_vaddr, seg_data))
                        break

            all_insns: list[Instruction] = []
            for seg_base, seg_data in segments:
                all_insns.extend(disassemble_bytes(seg_data, seg_base, big_endian))

            if args.functions:
                funcs = detect_functions(all_insns)
                if args.json:
                    out = [{"start": f"0x{f['start']:X}", "end": f"0x{f['end']:X}",
                            "size": f['size']} for f in funcs]
                    print(json.dumps(out, indent=2))
                else:
                    print(f"Detected {len(funcs)} functions:")
                    for f in funcs:
                        print(f"  0x{f['start']:08X} - 0x{f['end']:08X}  ({f['size']} bytes)")
                return

            if args.json:
                out = [{"addr": f"0x{i.addr:08X}", "hex": f"{i.raw:08X}",
                        "mnemonic": i.mnemonic, "operands": i.operands} for i in all_insns]
                print(json.dumps(out, indent=2))
            else:
                for i in all_insns:
                    print(i)
            return

        except Exception as exc:
            print(f"Warning: Could not parse as ELF ({exc}), treating as raw binary",
                  file=sys.stderr)
            data = file_data[args.offset:]
            if args.length:
                data = data[:args.length]

    # Raw binary path
    insns = disassemble_bytes(data, base_addr, big_endian)

    if args.functions:
        funcs = detect_functions(insns)
        if args.json:
            out = [{"start": f"0x{f['start']:X}", "end": f"0x{f['end']:X}",
                    "size": f['size']} for f in funcs]
            print(json.dumps(out, indent=2))
        else:
            print(f"Detected {len(funcs)} functions:")
            for f in funcs:
                print(f"  0x{f['start']:08X} - 0x{f['end']:08X}  ({f['size']} bytes)")
        return

    if args.json:
        out = [{"addr": f"0x{i.addr:08X}", "hex": f"{i.raw:08X}",
                "mnemonic": i.mnemonic, "operands": i.operands} for i in insns]
        print(json.dumps(out, indent=2))
    else:
        for i in insns:
            print(i)


if __name__ == "__main__":
    main()
