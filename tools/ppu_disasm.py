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
    256: "VRSAVE",
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
            33: "crnor", 225: "crnand", 289: "creqv",
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

        # MD form: XO is bits 27-29 (3-bit), bit 30 is sh[5]
        # MDS form: XO is bits 27-30 (4-bit, values 8=rldcl, 9=rldcr)
        md_ops = {0: "rldicl", 1: "rldicr", 2: "rldic", 3: "rldimi"}
        if sub3 in md_ops:
            sh = (bits(insn, 16, 20) | (bit(insn, 30) << 5))
            mb = (bits(insn, 21, 25) | (bit(insn, 26) << 5))
            mne = md_ops[sub3] + ("." if rc else "")
            result.mnemonic = mne
            result.operands = f"r{ra}, r{rd}, {sh}, {mb}"
        elif sub4 == 8:  # rldcl (MDS form)
            rb = bits(insn, 16, 20)
            mb = (bits(insn, 21, 25) | (bit(insn, 26) << 5))
            mne = "rldcl" + ("." if rc else "")
            result.mnemonic = mne
            result.operands = f"r{ra}, r{rd}, r{rb}, {mb}"
        elif sub4 == 9:  # rldcr (MDS form)
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
            233: "mulld", 73: "mulhd", 9: "mulhdu",
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
            827: "sradi",
        }
        if xo_full in x_shift:
            mne = x_shift[xo_full]
            if rc:
                mne += "."
            result.mnemonic = mne
            if xo_full in (824, 826, 827):
                # sradi is XS-form: sh[5] lives in bit 30, so the 10-bit
                # field reads 827 when the shift is >= 32.
                sh = rb + (32 if xo_full == 827 else 0)
                result.operands = f"r{ra}, r{rd}, {sh}"
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
            983: ("stfiwx", False),
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
        # XO 598 is three different barriers, told apart by the 2-bit L field
        # (bits 9..10): 0 is the heavyweight sync, 1 lwsync, 2 ptesync. Naming
        # all three "sync" cost nothing while the lifter treated them as
        # no-ops, but it does now that they lower to host fences: every
        # lightweight barrier a game emits would take a full one, and the
        # lifter's lwsync arm would be unreachable. objdump spells them apart
        # too, so this also keeps the disassembly audit honest.
        if xo_full == 598:
            result.mnemonic = {1: "lwsync", 2: "ptesync"}.get(bits(insn, 9, 10),
                                                              "sync")
            return result

        misc_x = {
            854: "eieio", 278: "dcbt", 246: "dcbtst",
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
        # NOTE: XO 827 is sradi with shift >= 32 (handled in the x_shift block
        # above). The real lhaux is XO 375, decoded in the algebraic table below.

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

        # VMX vector load/store (opcode 31, X-form)
        if xo_full == 103:  # lvx
            result.mnemonic = "lvx"
            result.operands = f"v{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 231:  # stvx
            result.mnemonic = "stvx"
            result.operands = f"v{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 7:    # lvebx
            result.mnemonic = "lvebx"
            result.operands = f"v{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 39:   # lvehx
            result.mnemonic = "lvehx"
            result.operands = f"v{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 71:   # lvewx
            result.mnemonic = "lvewx"
            result.operands = f"v{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 135:  # stvebx
            result.mnemonic = "stvebx"
            result.operands = f"v{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 167:  # stvehx
            result.mnemonic = "stvehx"
            result.operands = f"v{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 199:  # stvewx
            result.mnemonic = "stvewx"
            result.operands = f"v{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 359:  # lvxl
            result.mnemonic = "lvxl"
            result.operands = f"v{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 487:  # stvxl
            result.mnemonic = "stvxl"
            result.operands = f"v{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 6:    # lvsl
            result.mnemonic = "lvsl"
            result.operands = f"v{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 38:   # lvsr
            result.mnemonic = "lvsr"
            result.operands = f"v{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 519:  # lvlx (Cell ext: Load Vector Left Indexed)
            result.mnemonic = "lvlx"
            result.operands = f"v{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 551:  # lvrx (Cell ext: Load Vector Right Indexed)
            result.mnemonic = "lvrx"
            result.operands = f"v{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 775:  # lvlxl (Cell ext: Load Vector Left Indexed Last)
            result.mnemonic = "lvlxl"
            result.operands = f"v{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 532:  # ldbrx (Load Doubleword Byte-Reverse Indexed)
            result.mnemonic = "ldbrx"
            result.operands = f"r{rd}, r{ra}, r{rb}"
            return result

        # Cell unaligned vector STORES (were previously undecoded, so any
        # vector store using them fell through to the op31_x fallback and got
        # lifted as a TODO no-op -- a silent memory non-write, not just a
        # wrong value).
        if xo_full == 647:  # stvlx (Store Vector Left Indexed)
            result.mnemonic = "stvlx"
            result.operands = f"v{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 679:  # stvrx (Store Vector Right Indexed)
            result.mnemonic = "stvrx"
            result.operands = f"v{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 903:  # stvlxl
            result.mnemonic = "stvlxl"
            result.operands = f"v{rd}, r{ra}, r{rb}"
            return result
        if xo_full == 935:  # stvrxl
            result.mnemonic = "stvrxl"
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
            23: "fsel", 22: "fsqrt", 26: "frsqrte", 24: "fre",
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
            elif xo_5 in (26, 24, 22):  # frsqrte / fre / fsqrt(s): frd, frb (frA reserved=0)
                result.operands = f"f{frd}, f{frb}"
            else:
                result.operands = f"f{frd}, f{fra}, f{frb}"
            return result

        fp_x = {
            0: "fcmpu", 32: "fcmpo",
            12: "frsp", 14: "fctiw", 15: "fctiwz",
            814: "fctid", 815: "fctidz", 846: "fcfid",
            40: "fneg", 72: "fmr", 264: "fabs", 136: "fnabs",
            64: "mcrfs",
            583: "mffs", 711: "mtfsf",
            # FPSCR bit ops. These previously fell through to the unknown-xo
            # catch-all and emitted a raw .word, silently dropping the
            # instruction from the lift.
            70: "mtfsb0", 38: "mtfsb1", 134: "mtfsfi",
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
               22: "fsqrts", 24: "fres", 26: "frsqrtes"}
        if xo_5 in fps:
            mne = fps[xo_5]
            if rc:
                mne += "."
            result.mnemonic = mne
            if xo_5 == 25:
                result.operands = f"f{frd}, f{fra}, f{frc}"
            elif xo_5 in (29, 28, 31, 30):
                result.operands = f"f{frd}, f{fra}, f{frc}, f{frb}"
            elif xo_5 in (24, 26, 22):  # fres / frsqrtes / fsqrts: frd, frb (frA reserved=0)
                result.operands = f"f{frd}, f{frb}"
            else:
                result.operands = f"f{frd}, f{fra}, f{frb}"
            return result

    # VMX / AltiVec (opcd 4)
    if opcd == 4:
        vd = rd
        va = ra
        vb = bits(insn, 16, 20)
        vc = bits(insn, 21, 25)
        # AltiVec VX-form has an 11-bit XO at bits 21-31; extract the full 11 bits.
        # Bit 21 doubles as the Rc flag for compare (VC-form) instructions.
        xo_full = bits(insn, 21, 31)   # was bits(21,30) — missed bit 31 (XO LSB)
        xo_6 = bits(insn, 26, 31)

        # VA-form (6-bit xo at bits 26-31). XOs corrected per kakaroto/ps3ida
        # PPCAltivec (VXA macro) — the 36-45 range was previously scrambled,
        # mislabeling vperm/vsel/vsldoi and the whole vmsum* family.
        vmx_va = {
            32: "vmhaddshs", 33: "vmhraddshs", 34: "vmladduhm",
            36: "vmsumubm", 37: "vmsummbm", 38: "vmsumuhm", 39: "vmsumuhs",
            40: "vmsumshm", 41: "vmsumshs",
            42: "vsel", 43: "vperm", 44: "vsldoi",
            46: "vmaddfp", 47: "vnmsubfp",
        }
        if xo_6 in vmx_va:
            result.mnemonic = vmx_va[xo_6]
            if xo_6 == 44:  # vsldoi — 4th operand is a shift count, not a vector reg
                result.operands = f"v{vd}, v{va}, v{vb}, {vc}"
            else:
                result.operands = f"v{vd}, v{va}, v{vb}, v{vc}"
            return result

        # VX-form: xo_full is now 11 bits (bits 21-31).
        # For compare (VC-form) instructions, bit 21 is Rc; XO_10 is bits 22-31.
        vmx_cmp_rc = bit(insn, 21)  # Rc bit for compare instructions
        xo_10 = bits(insn, 22, 31)  # 10-bit XO (bits 22-31), no Rc

        vmx_cmp = {
            6: "vcmpequb", 70: "vcmpequh", 134: "vcmpequw",
            198: "vcmpeqfp", 966: "vcmpbfp", 454: "vcmpgefp",
            710: "vcmpgtfp",
            774: "vcmpgtsb", 838: "vcmpgtsh", 902: "vcmpgtsw",
            518: "vcmpgtub", 582: "vcmpgtuh", 646: "vcmpgtuw",
        }
        # Strip Rc (bit 10 of the 11-bit xo_full) to get the base 10-bit XO
        base_cmp_xo = xo_full & ~(1 << 10)  # strip Rc bit (now bit 10 of 11-bit field)
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
            1034: "vmaxfp", 1098: "vminfp",
            # Float estimate family (vD, vB; step-64 XOs). vexptefp/vlogefp were
            # decoding as vmx_x394/x458 -- found by diffing against spu/ppu-lv2-objdump
            # over 17M instructions of retail code (objdump_audit.py).
            266: "vrefp", 330: "vrsqrtefp", 394: "vexptefp", 458: "vlogefp",
            # Round to FP integer (vD, vB; step-64 XOs): nearest / zero / +inf / -inf.
            522: "vrfin", 586: "vrfiz", 650: "vrfip", 714: "vrfim",

            # Integer add/sub
            0: "vaddubm", 64: "vadduhm", 128: "vadduwm",
            1024: "vsububm", 1088: "vsubuhm", 1152: "vsubuwm",
            512: "vaddubs", 576: "vadduhs", 640: "vadduws",
            768: "vaddsbs", 832: "vaddshs", 896: "vaddsws",
            1536: "vsububs", 1600: "vsubuhs", 1664: "vsubuws",
            1792: "vsubsbs", 1856: "vsubshs", 1920: "vsubsws",

            # Integer min/max
            2: "vmaxub", 66: "vmaxuh", 130: "vmaxuw",
            258: "vmaxsb", 322: "vmaxsh", 386: "vmaxsw",
            514: "vminub", 578: "vminuh", 642: "vminuw",
            770: "vminsb", 834: "vminsh", 898: "vminsw",

            # Logical (XOs corrected per kakaroto/ps3ida PPCAltivec + NXP ALTIVECPEM:
            # vor=1156, vxor=1220, vnor=1284 — were previously shifted +64, which
            # mislabeled 327 vxor (mostly vxor vD,vX,vX register-clears) as vor)
            1028: "vand", 1092: "vandc", 1156: "vor", 1220: "vxor",
            1284: "vnor",

            # Shift
            260: "vslb", 324: "vslh", 388: "vslw",
            516: "vsrb", 580: "vsrh", 644: "vsrw",
            772: "vsrab", 836: "vsrah", 900: "vsraw",

            # (vspltisb/h/w moved to the SIMM handler below the UIMM block — their
            # vA field (bits 11-15) is a 5-bit SIGNED immediate, not a register.)

            # Merge
            12: "vmrghb", 76: "vmrghh", 140: "vmrghw",
            268: "vmrglb", 332: "vmrglh", 396: "vmrglw",

            # Convert (vcfux/vcfsx/vctuxs/vctsxs) — these carry a UIMM scale in
            # the vA field, so they are decoded below with the dedicated
            # "vD, vB, UIMM" operand form, not here.

            # Pack/unpack
            14: "vpkuhum", 78: "vpkuwum",
            142: "vpkuhus", 206: "vpkuwus",
            270: "vpkshus", 334: "vpkswus",
            398: "vpkshss", 462: "vpkswss",
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
            1928: "vsumsws", 1672: "vsum2sws",
            1544: "vsum4ubs", 1800: "vsum4sbs", 1608: "vsum4shs",
        }
        if xo_full in vmx_vx:
            result.mnemonic = vmx_vx[xo_full]
            result.operands = f"v{vd}, v{va}, v{vb}"
            return result

        # VX instructions whose vA field (bits 11-15) is an immediate, not a
        # register: splats (element index) and FP<->int converts (scale).
        # All emit "vD, vB, UIMM" so the lifter reads a bare integer at ops[2].
        vmx_uimm = {
            524: "vspltb", 588: "vsplth", 652: "vspltw",
            778: "vcfux", 842: "vcfsx", 906: "vctuxs", 970: "vctsxs",
        }
        if xo_full in vmx_uimm:
            result.mnemonic = vmx_uimm[xo_full]
            result.operands = f"v{vd}, v{vb}, {va}"
            return result

        # vspltis* : vD, SIMM — the vA field (bits 11-15) is a 5-bit SIGNED
        # immediate (-16..15), NOT a register (unlike vspltb/h/w's UIMM index).
        vmx_simm = {780: "vspltisb", 844: "vspltish", 908: "vspltisw"}
        if xo_full in vmx_simm:
            result.mnemonic = vmx_simm[xo_full]
            simm = va - 32 if va >= 16 else va
            result.operands = f"v{vd}, {simm}"
            return result

        # lvx / stvx (X-form under opcode 31 actually, but some are opcd 4)
        # Fallback for opcode 4
        result.mnemonic = f"vmx_x{xo_full}"
        result.operands = f"v{vd}, v{va}, v{vb}"
        return result

    # lvx/stvx now handled inside the main opcode 31 block above

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
    parser.add_argument("--va", type=lambda x: int(x, 0), default=None,
                        help="Virtual address to disassemble (ELF: maps va->file via PT_LOAD segments; "
                             "use with --length). Correct alternative to --raw --offset, which treats "
                             "--offset as a raw FILE offset (off by the segment base).")
    parser.add_argument("--little-endian", action="store_true",
                        help="Decode as little-endian")
    args = parser.parse_args()

    with open(args.input, "rb") as f:
        file_data = f.read()

    big_endian = not args.little_endian
    base_addr = args.base

    # --va: disassemble a window at a VIRTUAL address, mapping va->file offset through the ELF's
    # PT_LOAD segments (fixes the long-standing footgun where --raw --offset <vaddr> read the wrong
    # bytes because --offset is a file offset, off by the .text segment base e.g. 0x10000).
    if args.va is not None:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from elf_parser import ELFFile, PT_LOAD
        elf = ELFFile(args.input); elf.load()
        big_endian = elf.elf_header.big_endian
        va = args.va; seg = None
        for ph in elf.program_headers:
            if ph.p_type == PT_LOAD and ph.p_vaddr <= va < ph.p_vaddr + ph.p_filesz:
                seg = ph; break
        if seg is None:
            print(f"Error: vaddr 0x{va:08X} not in any PT_LOAD file-backed range", file=sys.stderr)
            sys.exit(1)
        file_off = seg.p_offset + (va - seg.p_vaddr)
        n = args.length or 0x40
        data = file_data[file_off:file_off + n]
        for i in disassemble_bytes(data, va, big_endian):
            print(i)
        return

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
