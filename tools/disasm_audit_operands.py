#!/usr/bin/env py -3
"""disasm_audit_operands.py - PPU decoder OPERAND-level cross-check: v2 of
tools/disasm_audit.py.

Purpose: v1 (disasm_audit.py) cross-checks tools/ppu_disasm.py's decode()
against Capstone at the MNEMONIC level only and is blind to the `il`-class
of bug: a mnemonic decodes correctly but an OPERAND (register index,
immediate value, shift/mask field, or operand count/order) is wrong. This
is historically the most expensive bug class in this project (double
sign-extension, wrong register field, swapped operand order) because our
disasm and lifter can independently agree with each other and both be
wrong -- Capstone is the independent oracle needed to catch it.

Scope: for every word where ours and Capstone already AGREE on the
mnemonic (mnemonic mismatches are v1's job -- this tool doesn't re-derive
them), parse both operand strings into a canonical comparable form and
diff. A DIVERGENCE = same mnemonic, differing canonical operand (register
index, immediate VALUE, or operand count/order).

Canonicalization:
  - Registers (any of r/f/v/cr prefix, or a bare "crN"/"crN lt" CR-bit
    name, or Capstone's occasional bare-integer register operand) map to
    ("reg", class_letter_or_None, index).
  - Immediates (hex "0x..", "-0x..", plain decimal, negative decimal) map
    to ("imm", signed_python_int).
  - Anything else that isn't a recognized register/immediate token
    (rare -- e.g. a CR-bit condition-name operand like "lt") is kept as a
    normalized string token: ("raw", token.lower()).
  - Operand ORDER is part of the comparison (a swapped-operand-order bug
    is exactly what this tool must catch), but see the WHITELIST for
    known legitimate operand-order/format differences between the two
    printers (e.g. Capstone shows an absolute branch target while ours
    shows... no, both show absolute -- see whitelist file for the actual
    seeded/measured cases).

Whitelist: tools/disasm_audit_operands_whitelist.txt, one entry per
KNOWN-benign format difference, of the form:
    mnemonic | short reason
A mnemonic listed there has ALL its divergences reported as INFO instead
of REAL. Entries must be added only after direct measurement (hand-built
encoding through both decoders) -- see the comments in the seed list
below for the measured basis of each seeded entry.

Usage:
    py -3 tools\\disasm_audit_operands.py game\\EBOOT.elf [--limit N] [--top N]
    py -3 tools\\disasm_audit_operands.py --self-test-immediate-bug
    py -3 tools\\disasm_audit_operands.py --self-test-mask-bug

Exit code: 0 if residual (non-whitelisted) REAL divergence count is 0 for
the run, else 1.
"""

import argparse
import os
import re
import struct
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from elf_parser import ELFFile, PT_LOAD  # noqa: E402
from ppu_disasm import decode  # noqa: E402

WHITELIST_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               "disasm_audit_operands_whitelist.txt")


def _capstone_or_die():
    try:
        import capstone
        return capstone
    except ImportError:
        print("disasm_audit_operands: capstone not installed -- "
              "run: py -3 -m pip install capstone", file=sys.stderr)
        sys.exit(1)


# ---------------------------------------------------------------------------
# Operand canonicalization
# ---------------------------------------------------------------------------

# Register token forms seen from EITHER decoder:
#   ours:      r3, f3, v3, cr3, cr3lt/cr3gt/cr3eq/cr3so (via _cr_bit_name),
#              bare "lt"/"gt"/"eq"/"so" (cr0 bit name with empty prefix)
#   capstone:  r3, f3, v3, cr3, occasionally bare "3" for some XO-form regs,
#              condition register bit field names as an operand for BOnly
#              forms (not relevant here -- mnemonic differs, v1's job)
_REG_RE = re.compile(r"^(?P<cls>[rfv]|cr)(?P<idx>\d+)$", re.IGNORECASE)
_CR_BIT_RE = re.compile(r"^(?:cr(?P<idx>\d+))?(?P<bit>lt|gt|eq|so|un)$",
                         re.IGNORECASE)
# "un" (unordered) is Capstone's alternate name for CR sub-bit 3 -- the
# SAME physical bit as "so" (summary overflow); Capstone spells it "un"
# specifically on crand/cror/crxor/etc. (raw CR-bit logical ops, which
# don't know whether the bit's producer was an integer or FP compare),
# reserving "so" for contexts where the bit demonstrably came from an
# integer-compare/XER path. MEASURED: 0x01293918 real EBOOT code,
# "crxor 7, 31, 24" (ours, raw indices) vs "crxor cr1un, cr7un, cr6lt"
# (capstone) -- bit 7 -> field 1, sub 3 -> "un" here (not "so"); same
# numeric bit-value either way.
_CR_BIT_VALUE = {"lt": 0, "gt": 1, "eq": 2, "so": 3, "un": 3}
_HEX_RE = re.compile(r"^-?0x[0-9a-fA-F]+$")
_DEC_RE = re.compile(r"^-?\d+$")
# Capstone displacement-form operand: "-0x8000(r5)" / "8(r3)" / "-4(r5)".
# Capstone ALSO uses a bare "0" (no letter prefix) for the base-register
# slot whenever RA==0 in a D-form load/store (PowerPC semantics: RA field
# of 0 means "no base register, EA = 0 + disp", so Capstone prints the
# architectural constant 0 instead of the register name r0). Ours always
# prints "r0" there regardless. MEASURED: 0x00014838 real EBOOT code,
# "lwz r0, 0x0(r0)" (ours) vs "lwz r0, 0(0)" (capstone) -- both compute
# EA=0, pure spelling difference, not a value divergence. The regex below
# accepts either "rN" or a bare integer (always 0 in practice) as the base.
_DISP_RE = re.compile(
    r"^(?P<disp>-?(?:0x[0-9a-fA-F]+|\d+))\((?P<reg>[a-zA-Z]+\d+|\d+)\)$")


def _parse_int_token(tok: str):
    """Parse a bare hex/decimal token to a signed Python int, or None."""
    if _HEX_RE.match(tok):
        return int(tok, 16)
    if _DEC_RE.match(tok):
        return int(tok, 10)
    return None


def canon_operand(tok: str):
    """Canonicalize a single operand token to a comparable tuple.

    Returns one of:
      ("reg", class_letter, index)   -- class_letter in {'r','f','v','cr',None}
      ("crbit", cr_field, bit_value) -- cr_field 0..7, bit_value 0..3 (lt/gt/eq/so)
      ("imm", value)                 -- signed python int
      ("dispmem", disp_value, base_reg_index) -- D-form "disp(rN)" memory operand
      ("raw", lowercased_token)      -- fallback, unrecognized token
    """
    tok = tok.strip()
    if not tok:
        return ("raw", "")

    m = _DISP_RE.match(tok)
    if m:
        disp = _parse_int_token(m.group("disp"))
        reg_tok = m.group("reg")
        base = _REG_RE.match(reg_tok)
        if base:
            base_idx = int(base.group("idx"))
        elif reg_tok.isdigit():
            # Capstone's bare-"0" spelling of "RA field is architecturally
            # zero" (no base register) -- canonicalize to the same base
            # index as an explicit r0 token (see _DISP_RE comment above).
            base_idx = int(reg_tok)
        else:
            base_idx = reg_tok
        return ("dispmem", disp, base_idx)

    m = _REG_RE.match(tok)
    if m:
        return ("reg", m.group("cls").lower(), int(m.group("idx")))

    m = _CR_BIT_RE.match(tok)
    if m and (m.group("idx") is not None or m.group("bit")):
        field = int(m.group("idx")) if m.group("idx") is not None else 0
        return ("crbit", field, _CR_BIT_VALUE[m.group("bit").lower()])

    val = _parse_int_token(tok)
    if val is not None:
        return ("imm", val)

    return ("raw", tok.lower())


# CR-logical XL-form ops (Book I Ch.2): all operands are raw CR-BIT indices
# (0..31), NOT general integers or GPRs. Ours prints the bare numeric bit
# index (e.g. "cror 6, 1, 2"); Capstone always spells the "crNbit" named
# form (e.g. "cror cr1eq, cr0gt, cr0eq"). MEASURED equivalent at
# 0x00011288 (bt=6->cr1eq, ba=1->cr0gt, bb=2->cr0eq) -- pure naming
# convention, not a value divergence, so these mnemonics' bare-integer
# operands are reinterpreted as ("crbit", field, bit) tuples below instead
# of ("imm", value), to compare equal to Capstone's named spelling.
_CR_LOGICAL_MNEMONICS = {
    "crand", "cror", "crxor", "crnand", "crnor", "creqv", "crandc", "crorc",
}


def _reinterpret_as_crbit(canon):
    """If canon is ("imm", v) with v in [0,31], reinterpret as a CR-bit
    index tuple ("crbit", field, bit); otherwise return canon unchanged.
    """
    if canon[0] == "imm" and 0 <= canon[1] <= 31:
        v = canon[1]
        return ("crbit", v >> 2, v & 3)
    return canon


def canon_operand_list(op_str: str, mnemonic: str = ""):
    """Split an operand string on top-level commas and canonicalize each.

    D-form memory operands like "4(r5)" contain no comma so a naive split
    on ',' is safe here (no operand ever nests a comma inside parens in
    either decoder's output for PPC).
    """
    op_str = op_str.strip()
    if not op_str:
        return []
    parts = [p.strip() for p in op_str.split(",") if p.strip() != ""]
    canon = [canon_operand(p) for p in parts]
    if mnemonic in _CR_LOGICAL_MNEMONICS:
        canon = [_reinterpret_as_crbit(c) for c in canon]
    return canon


# ---------------------------------------------------------------------------
# Value-equivalence normalization between canon operand lists
#
# Some (mnemonic-agreeing) pairs are STRUCTURALLY worded differently but
# numerically identical -- e.g. ours may print a raw CR-bit name where
# Capstone prints "crN, bit" split differently. We normalize a handful of
# known-shape differences here (measured against real decoder output,
# see disasm_audit_operands_whitelist.txt for the mnemonic-level list of
# what's tolerated); this function only smooths PURELY SYNTACTIC splits,
# never changes an actual value.
# ---------------------------------------------------------------------------

def _standalone_reg0_equiv(a, b):
    """True if *a* and *b* are the SAME position's canon operand and one is
    ("reg", "r", 0) while the other is ("imm", 0) -- i.e. one decoder wrote
    a bare register token "r0" and the other wrote the literal integer 0
    for an X-form base-register field where the architecture defines
    RA==0 as "no base register / value 0" (Book I: any X-form/D-form base
    register field). This ONLY fires for index 0 in either direction --
    r1..r31 vs any "imm" never match here, so it cannot mask a genuine
    register-index divergence. MEASURED: 0x00012830 real EBOOT code,
    "dcbt r0, r7" (ours) vs "dcbt 0, r7" (capstone) -- same architectural
    value (EA contribution 0), pure base-register spelling difference
    (same class as the dispmem "(0)" vs "(r0)" case handled in
    canon_operand's _DISP_RE, but here the register stands alone, not
    inside parens, e.g. dcbt/dcbst/icbi/lvx/stvx/lfsx-family RA operand).
    """
    reg0 = ("reg", "r", 0)
    imm0 = ("imm", 0)
    return (a == reg0 and b == imm0) or (a == imm0 and b == reg0)


def _abs_branch_target_equiv(a, b):
    """True if *a* and *b* are ("imm", v1)/("imm", v2) and v1, v2 agree on
    the low 32 bits with the difference explained PURELY by 32-bit-vs-64-
    bit sign-extension of an ABSOLUTE branch target (AA=1 forms: b/bl/ba/
    bla and every conditional b<cond><l><a> variant, e.g. bdnzla, bnsla).
    tools/ppu_disasm.py deliberately masks every absolute branch target to
    32 bits (`target & 0xFFFFFFFF`, ppu_disasm.py:159/166/180/194/197 and
    all the bclr/bcctr sites) -- a documented, consistent convention across
    this whole codebase, since Cell/PS3 PPU effective addresses for this
    game are 32-bit. Capstone, running in CS_MODE_64, instead sign-extends
    the same target to a full 64-bit hex display (e.g. ours "0xFFFF9998"
    vs capstone "0xffffffffffff9998"). MEASURED across many real EBOOT
    branch sites (e.g. 0x009B0780 bdza, 0x0110C530 ba): masking either
    side's value to 32 bits always makes them equal. Scoped tightly to
    avoid masking a real bug: both values must already agree on the low
    32 bits, AND the higher-magnitude one must be a canonical sign
    extension of the low 32 bits (all 1s above bit 31, i.e. the low-32
    value's bit 31 was set) -- so this can never quietly accept two
    genuinely different small immediates or two different register
    indices; it only fires for the specific "same low 32 bits, one side
    64-bit-sign-extended" shape.
    """
    if a[0] != "imm" or b[0] != "imm":
        return False
    v1, v2 = a[1], b[1]
    # Canon_operand's _parse_int_token parses hex/decimal tokens (including
    # Capstone's "0xffffffffffff9998" spelling) as plain non-negative
    # Python ints -- there is no leading '-' in that token, so v1/v2 here
    # are the RAW unsigned integer value of whatever hex/decimal digits
    # were printed, never a Python-negative int. Compare on that basis:
    # both must already agree on the low 32 bits, and the numerically
    # LARGER of the two must equal the low-32 value widened with all 1s
    # in bits 32..63 (i.e. it must equal low32 | 0xFFFFFFFF00000000) --
    # the canonical unsigned-64-bit spelling of a sign-extended-negative
    # 32-bit value. This can only fire when bit 31 of the low 32 bits is
    # set (a "negative-looking" 32-bit value), so it never accepts two
    # unrelated positive immediates.
    if (v1 & 0xFFFFFFFF) != (v2 & 0xFFFFFFFF):
        return False
    low32 = v1 & 0xFFFFFFFF
    if v1 == v2:
        return True  # identical already (no sign-extension involved)
    if not (low32 & 0x80000000):
        return False  # bit 31 clear -> no valid sign-extension story
    sign_ext_64_unsigned = low32 | 0xFFFFFFFF00000000
    return {v1, v2} == {low32, sign_ext_64_unsigned}


def _capstone_dropped_zero_branch_target(ours_list, cs_list, addr):
    """True if ours_list has exactly one MORE trailing operand than
    cs_list, that extra trailing operand is an immediate whose value is
    consistent with an architectural BD (branch-displacement) field of
    exactly 0, and the remaining (leading) operands already match. This
    is Capstone's PPC-backend quirk of omitting the branch-target operand
    entirely whenever the raw BD field (bits 16-29) is exactly 0.

    Two shapes, both MEASURED on real EBOOT sites:
      - PC-relative conditional branches (AA=0): architectural target =
        PC+0 = the instruction's own address, so ours prints the resolved
        absolute address equal to *addr* itself (e.g. 0x00023220 "bge
        0x23220" -- 0x23220 == addr).
      - Absolute conditional+LK branches (AA=1): architectural target =
        literal 0, so ours prints ("imm", 0) (e.g. 0x01241820 "bgela
        0x0").
    In both shapes Capstone prints nothing. Scoped tightly: the extra
    trailing operand must be EITHER exactly 0 OR exactly equal to the
    instruction's own address -- no other value is accepted, so this can
    never mask a genuine wrong-target bug (a real miscomputed target
    essentially never equals literal 0 or the instruction's own PC).
    """
    if len(ours_list) != len(cs_list) + 1:
        return False
    extra = ours_list[-1]
    if extra[0] != "imm":
        return False
    if extra[1] not in (0, addr):
        return False
    return ours_list[:-1] == cs_list


def operands_equal(ours_list, cs_list, addr=None):
    if ours_list == cs_list:
        return True
    if addr is not None and \
            _capstone_dropped_zero_branch_target(ours_list, cs_list, addr):
        return True
    if len(ours_list) != len(cs_list):
        return False
    for a, b in zip(ours_list, cs_list):
        if a == b:
            continue
        if _standalone_reg0_equiv(a, b):
            continue
        if _abs_branch_target_equiv(a, b):
            continue
        return False
    return True


# ---------------------------------------------------------------------------
# Whitelist (mnemonic -> reason). A mnemonic in this table has ALL its
# divergences reported as INFO, never counted in the REAL residual.
# ---------------------------------------------------------------------------

def load_whitelist(path: str):
    wl = {}
    if not os.path.exists(path):
        return wl
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if "|" not in line:
                continue
            mn, reason = line.split("|", 1)
            wl[mn.strip()] = reason.strip()
    return wl


# ---------------------------------------------------------------------------
# ELF word iteration (same as v1)
# ---------------------------------------------------------------------------

def executable_words(elf_path: str):
    elf = ELFFile(elf_path)
    elf.load()
    if not elf.elf_header.big_endian:
        print("disasm_audit_operands: WARNING -- ELF parsed as "
              "little-endian; PS3 PPU binaries should be big-endian",
              file=sys.stderr)

    segments = []
    for idx, ph in enumerate(elf.program_headers):
        if ph.p_type == PT_LOAD and (ph.p_flags & 1) and ph.p_filesz > 0:
            data = elf.get_segment_data(idx)
            segments.append((ph.p_vaddr, data))

    if not segments:
        print("disasm_audit_operands: no PF_X PT_LOAD segments found",
              file=sys.stderr)
        return

    fmt = ">I" if elf.elf_header.big_endian else "<I"
    for base_vaddr, data in segments:
        n_words = len(data) // 4
        for i in range(n_words):
            off = i * 4
            word = struct.unpack_from(fmt, data, off)[0]
            yield base_vaddr + off, word


def decode_ours(word: int, addr: int):
    insn = decode(word, addr)
    return insn.mnemonic, insn.operands


def make_capstone_decoder():
    capstone = _capstone_or_die()
    md = capstone.Cs(capstone.CS_ARCH_PPC,
                      capstone.CS_MODE_64 + capstone.CS_MODE_BIG_ENDIAN)
    md.detail = False

    def _decode(word: int, addr: int):
        data = struct.pack(">I", word)
        insns = list(md.disasm(data, addr))
        if not insns:
            return None
        return insns[0].mnemonic, insns[0].op_str

    return _decode


# ---------------------------------------------------------------------------
# Main audit
# ---------------------------------------------------------------------------

def run_audit(elf_path: str, limit: int, whitelist: dict, ours_decoder=None,
              label="real"):
    """Decode every word with both decoders. For words where the mnemonic
    already matches, canonicalize + compare operands.

    Returns a dict of counters/samples; see print_report for consumption.
    """
    cs_decode = make_capstone_decoder()
    if ours_decoder is None:
        ours_decoder = decode_ours

    total = 0
    mnemonic_match = 0
    divergence_counter = Counter()          # (mnemonic, kind) -> count, REAL only
    divergence_samples = defaultdict(list)  # (mnemonic, kind) -> [(addr, ours_op, cs_op)]
    info_counter = Counter()                # whitelisted mnemonic divergences
    info_samples = defaultdict(list)

    for addr, word in executable_words(elf_path):
        if limit and total >= limit:
            break
        total += 1

        ours_mn, ours_ops = ours_decoder(word, addr)
        cs_result = cs_decode(word, addr)
        if cs_result is None:
            continue
        cs_mn, cs_ops = cs_result

        if ours_mn != cs_mn:
            continue  # v1's territory
        mnemonic_match += 1

        ours_canon = canon_operand_list(ours_ops, ours_mn)
        cs_canon = canon_operand_list(cs_ops, ours_mn)

        if operands_equal(ours_canon, cs_canon, addr):
            continue

        # Classify divergence kind
        if len(ours_canon) != len(cs_canon):
            kind = "operand-count"
        else:
            kind = "operand-value"
            # refine: is it purely an order permutation of the same
            # multiset of canonical operands? (helps spot swapped-operand
            # bugs distinctly from value/register-index bugs)
            if sorted(map(repr, ours_canon)) == sorted(map(repr, cs_canon)):
                kind = "operand-order"

        key = (ours_mn, kind)
        if ours_mn in whitelist:
            info_counter[key] += 1
            if len(info_samples[key]) < 5:
                info_samples[key].append((addr, ours_ops, cs_ops))
        else:
            divergence_counter[key] += 1
            if len(divergence_samples[key]) < 5:
                divergence_samples[key].append((addr, ours_ops, cs_ops))

    return {
        "total": total,
        "mnemonic_match": mnemonic_match,
        "divergence_counter": divergence_counter,
        "divergence_samples": divergence_samples,
        "info_counter": info_counter,
        "info_samples": info_samples,
        "label": label,
    }


def print_report(result, top_n: int, whitelist: dict):
    total = result["total"]
    mnemonic_match = result["mnemonic_match"]
    divergence_counter = result["divergence_counter"]
    divergence_samples = result["divergence_samples"]
    info_counter = result["info_counter"]
    info_samples = result["info_samples"]

    print(f"disasm_audit_operands [{result['label']}]: {total} words "
          f"decoded, {mnemonic_match} had matching mnemonic (the operand-"
          f"comparison population)")

    residual = sum(divergence_counter.values())
    residual_groups = len(divergence_counter)
    info_total = sum(info_counter.values())

    print()
    print(f"=== REAL operand divergences (non-whitelisted): {residual} "
          f"words across {residual_groups} (mnemonic, kind) groups ===")
    print(f"top {top_n} by count (descending):")
    for (mn, kind), count in divergence_counter.most_common(top_n):
        samples = divergence_samples[(mn, kind)]
        print(f"  {count:8d}  mnemonic={mn!r:14s} kind={kind:16s}")
        for addr, ours_op, cs_op in samples:
            print(f"        0x{addr:08X}: ours=\"{mn} {ours_op}\" "
                  f"capstone=\"{mn} {cs_op}\"")

    print()
    print(f"=== Whitelisted operand-format noise (INFO): {info_total} "
          f"words across {len(info_counter)} (mnemonic, kind) groups ===")
    for (mn, kind), count in info_counter.most_common():
        reason = whitelist.get(mn, "(no reason recorded)")
        print(f"  INFO  {count:8d}  mnemonic={mn!r:14s} kind={kind:16s}"
              f"  -- {reason}")
        for addr, ours_op, cs_op in info_samples[(mn, kind)][:2]:
            print(f"        0x{addr:08X}: ours=\"{mn} {ours_op}\" "
                  f"capstone=\"{mn} {cs_op}\"")

    print()
    print("=== Sanity check: common ops (addi, lwz, stw, ori, rlwinm) ===")
    sanity_ok = True
    for op in ("addi", "lwz", "stw", "ori", "rlwinm"):
        bad = [((mn, kind), c) for (mn, kind), c in divergence_counter.items()
               if mn == op]
        if bad:
            sanity_ok = False
            for (mn, kind), c in bad:
                print(f"  FAIL  {op}: {c} non-whitelisted operand "
                      f"divergences (kind={kind})")
        else:
            print(f"  PASS  {op}: 0 non-whitelisted operand divergences")
    if not sanity_ok:
        print("  ** SANITY FAILED ** -- canonicalization is incomplete, "
              "see FAIL lines above")

    print()
    print(f"SUMMARY[{result['label']}]: total_words={total} "
          f"mnemonic_match_population={mnemonic_match} "
          f"residual_real_divergence_words={residual} "
          f"residual_groups={residual_groups} "
          f"whitelisted_info_words={info_total}")

    return residual, sanity_ok


# ---------------------------------------------------------------------------
# Seeded self-tests (build a corrupted COPY of decode() in-process; never
# touches tools/ppu_disasm.py on disk).
# ---------------------------------------------------------------------------

def _make_corrupted_decoder_double_sign_extend():
    """Simulate the classic `il`-class bug: an immediate that gets
    sign-extended TWICE (or equivalently, off by a power-of-two wraparound)
    on D-form loads (lwz/addi/etc). We corrupt just 'addi' operands: take
    ours' real decode, then if mnemonic is 'addi', re-render the operand
    string with the immediate corrupted by subtracting 0x10000 (simulating
    a double 16-bit sign-extension) whenever the immediate is negative.
    """
    def _decode(word, addr):
        mn, ops = decode_ours(word, addr)
        if mn == "addi":
            parts = [p.strip() for p in ops.split(",")]
            if len(parts) == 3:
                imm = _parse_int_token(parts[2])
                if imm is not None and imm < 0:
                    corrupted = imm - 0x10000  # double sign-extension bug
                    parts[2] = f"{corrupted}" if corrupted >= 0 \
                        else f"-0x{-corrupted:X}"
                    ops = ", ".join(parts)
        return mn, ops
    return _decode


def _make_corrupted_decoder_mask_bug():
    """Simulate a wrong-mask-field bug on rlwinm: corrupt the `me` operand
    (last field) by adding 1 (off-by-one mask bug), whenever me < 31, to
    ensure it produces a genuinely different runtime rotate/mask -- an
    operand-VALUE bug, distinct from the immediate-class self-test above.
    """
    def _decode(word, addr):
        mn, ops = decode_ours(word, addr)
        if mn == "rlwinm" or mn == "rlwinm.":
            parts = [p.strip() for p in ops.split(",")]
            if len(parts) == 5:
                me = _parse_int_token(parts[4])
                if me is not None and me < 31:
                    parts[4] = str(me + 1)
                    ops = ", ".join(parts)
        return mn, ops
    return _decode


def run_self_test(elf_path: str, limit: int, kind: str, whitelist: dict):
    if kind == "immediate":
        print("disasm_audit_operands --self-test-immediate-bug: seeding a "
              "double-sign-extension bug on 'addi' negative immediates in "
              "an IN-PROCESS COPY of decode() (tools/ppu_disasm.py on disk "
              "is untouched)")
        corrupted = _make_corrupted_decoder_double_sign_extend()
        target_mn = "addi"
    elif kind == "mask":
        print("disasm_audit_operands --self-test-mask-bug: seeding an "
              "off-by-one ME-field bug on 'rlwinm' in an IN-PROCESS COPY "
              "of decode() (tools/ppu_disasm.py on disk is untouched)")
        corrupted = _make_corrupted_decoder_mask_bug()
        target_mn = "rlwinm"
    else:
        raise ValueError(kind)

    result = run_audit(elf_path, limit, whitelist, ours_decoder=corrupted,
                        label=f"self-test-{kind}")
    residual, _sanity = print_report(result, 15, whitelist)

    hit = any(mn == target_mn
              for (mn, _kind) in result["divergence_counter"])
    if not hit or residual == 0:
        print(f"SELF-TEST FAILED: seeded {kind} bug on {target_mn!r} did "
              f"NOT surface as a REAL operand divergence")
        return False
    print(f"SELF-TEST PASSED: seeded {kind} bug on {target_mn!r} surfaced "
          f"as a REAL operand divergence ({residual} words)")
    return True


def main():
    parser = argparse.ArgumentParser(
        description="Operand-level cross-check of tools/ppu_disasm.py vs "
                    "Capstone (PPC64 big-endian) over an ELF's executable "
                    "segments, restricted to mnemonic-matching words.")
    parser.add_argument("elf", help="Input PPU ELF to audit (e.g. game/EBOOT.elf)")
    parser.add_argument("--limit", type=int, default=0,
                        help="Stop after N words (0 = all, default)")
    parser.add_argument("--top", type=int, default=30,
                        help="How many divergence groups to print "
                            "(default 30)")
    parser.add_argument("--self-test-immediate-bug", action="store_true",
                        help="Seeded double-sign-extension bug on addi "
                            "(never touches ppu_disasm.py on disk)")
    parser.add_argument("--self-test-mask-bug", action="store_true",
                        help="Seeded off-by-one ME-field bug on rlwinm "
                            "(never touches ppu_disasm.py on disk)")
    args = parser.parse_args()

    whitelist = load_whitelist(WHITELIST_PATH)

    if args.self_test_immediate_bug:
        ok = run_self_test(args.elf, args.limit, "immediate", whitelist)
        sys.exit(0 if ok else 1)
    if args.self_test_mask_bug:
        ok = run_self_test(args.elf, args.limit, "mask", whitelist)
        sys.exit(0 if ok else 1)

    if not os.path.exists(args.elf):
        print(f"disasm_audit_operands: input file not found: {args.elf}",
              file=sys.stderr)
        sys.exit(1)

    result = run_audit(args.elf, args.limit, whitelist)
    residual, sanity_ok = print_report(result, args.top, whitelist)
    sys.exit(0 if (residual == 0 and sanity_ok) else 1)


if __name__ == "__main__":
    main()
