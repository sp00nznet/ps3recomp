#!/usr/bin/env python3
"""Generate the SPU interpreter's decode table (C) from the validated Python
decoder (spu_disasm.py). The interpreter is the correctness floor for the SPU
static recompiler: computed/indirect branches (`bi $reg`) and DMA'd or
self-modified code (LBP overlays, spu_0004's `sync; bi $3` trampoline) can't be
statically lifted, so on a lifted-function-table miss we interpret from live
local store. Emitting the opcode->mnemonic tables from spu_disasm keeps the
interpreter's decode bit-identical to the decoder we already proved 100% vs
spu-lv2-objdump; the execute half (spu_interp.c) calls the SAME spu_<mnemonic>
helpers the lifter emits, so lifted and interpreted code share exact semantics.

    python gen_spu_interp.py > ../runtime/spu/spu_interp_tables.inc
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import spu_disasm as D  # RRR_TABLE, RI18_TABLE, RI16_TABLE, RI10_TABLE, SPU_RR, ...

# The RI7 / RI8 / LSX tables live inside spu_decode() as locals; mirror them here
# (kept in sync by this generator; a drift shows immediately in the selftest).
RI7 = {
    0x078: "roti", 0x079: "rotmi", 0x07a: "rotmai", 0x07b: "shli",
    0x07c: "rothi", 0x07d: "rothmi", 0x07e: "rotmahi", 0x07f: "shlhi",
    0x1f4: "cbd", 0x1f5: "chd", 0x1f6: "cwd", 0x1f7: "cdd",
    0x1f8: "rotqbii", 0x1f9: "rotqmbii", 0x1fb: "shlqbii",
    0x1fc: "rotqbyi", 0x1fd: "rotqmbyi", 0x1ff: "shlqbyi",
}
RI7_UNSIGNED = {"cbd", "chd", "cwd", "cdd"}
RI8 = {0x1d8: "cflts", 0x1d9: "cfltu", 0x1da: "csflt", 0x1db: "cuflt"}  # op10
LSX = {0x1c4: "lqx", 0x144: "stqx"}
CHANNEL = {0x00D: "rdch", 0x10D: "wrch", 0x00F: "rchcnt"}  # op11

# Every mnemonic the decoder can emit -> a stable enum. `.word` is the trap.
all_mn = set()
for t in (D.RRR_TABLE, D.RI18_TABLE, D.RI16_TABLE, D.RI10_TABLE, D.SPU_RR,
          RI7, RI8, LSX, CHANNEL):
    all_mn.update(t.values())
all_mn.add(".word")
mnems = sorted(all_mn)


def enum_name(m):
    return "SPU_" + ("word" if m == ".word" else m)


# Every opcode field of the SPU ISA (RRR 4 bits, RI18 7, RI16 9, RI10 8, RI8 10,
# RI7/RR/LSX/channel 11) is a prefix of instruction bits 31..21, so the matched
# mnemonic is a pure function of p = insn >> 21 and the whole decode collapses
# into one 2048-entry table. The immediate form is a function of the matched
# mnemonic and of the table it came from; the position-fixed operand fields
# (rt/ra/rb/rc/ch) stay in spu_decode1.
#
# Prefixes may match more than one table, and the first stage to match wins, so
# this order IS the decoder's precedence and mirrors spu_disasm.spu_decode:
# RI16 before RI10 (an RI10 opcode equal to the top 8 bits of an RI16 opcode
# must not shadow it), channel ops before RI10, RR before RI7, LSX last, else
# `.word`. With the current tables the only prefixes matched twice are
# rdch/rchcnt/wrch (channel and RR) and lqx/stqx (RR and LSX), where both
# stages agree; the order is kept so a table entry added later resolves
# exactly as the old cascade did instead of silently changing which
# instruction executes.
STAGES = (
    ("rrr", 4, D.RRR_TABLE),
    ("ri18", 7, D.RI18_TABLE),
    ("ri16", 9, D.RI16_TABLE),
    ("chan", 11, CHANNEL),
    ("ri10", 8, D.RI10_TABLE),
    ("ri8", 10, RI8),
    ("rr", 11, D.SPU_RR),
    ("ri7", 11, RI7),
    ("lsx", 11, LSX),
)

# How spu_decode1 derives `imm` / `tgt` for a decoded instruction.
FORMS = (
    ("NONE", "no immediate: RRR, RR (except stop), LSX, channel ops, .word"),
    ("I18", "ila: full 18-bit immediate"),
    ("I18LO", "other RI18 (hbra/hbrr): low 16 bits of the 18-bit field"),
    ("LQA", "lqa/stqa: absolute 16-byte aligned LS address in tgt"),
    ("LQR", "lqr/stqr: pc-relative 16-byte aligned LS address in tgt"),
    ("BREL", "br/brsl/brz/brnz/brhz/brhnz: pc-relative target in tgt"),
    ("BABS", "bra/brasl: absolute target in tgt"),
    ("I16S", "il: sign-extended 16-bit immediate"),
    ("I16U", "ilh/ilhu/iohl/fsmbi: zero-extended 16-bit immediate"),
    ("I10", "RI10: sign-extended 10-bit immediate"),
    ("I10Q", "lqd/stqd: sign-extended 10-bit immediate scaled by 16"),
    ("I8", "RI8 float<->int conversions: unsigned 8-bit scale"),
    ("STOP", "stop: 14-bit signal type"),
    ("I7S", "RI7: sign-extended 7-bit immediate"),
    ("I7U", "cbd/chd/cwd/cdd: unsigned 7-bit immediate"),
)

RI16_FORMS = {
    "lqa": "LQA", "stqa": "LQA", "lqr": "LQR", "stqr": "LQR",
    "br": "BREL", "brsl": "BREL", "brz": "BREL", "brnz": "BREL",
    "brhz": "BREL", "brhnz": "BREL", "bra": "BABS", "brasl": "BABS",
    "il": "I16S",
}


def form_of(stage, mn):
    if stage == "ri18":
        return "I18" if mn == "ila" else "I18LO"
    if stage == "ri16":
        return RI16_FORMS.get(mn, "I16U")
    if stage == "ri10":
        return "I10Q" if mn in ("lqd", "stqd") else "I10"
    if stage == "ri8":
        return "I8"
    if stage == "rr":
        return "STOP" if mn == "stop" else "NONE"
    if stage == "ri7":
        return "I7U" if mn in RI7_UNSIGNED else "I7S"
    return "NONE"


def decode_prefix(p):
    """(mnemonic, form) for the 11-bit prefix p = insn >> 21."""
    for stage, bits, table in STAGES:
        mn = table.get(p >> (11 - bits))
        if mn is not None:
            return mn, form_of(stage, mn)
    return ".word", "NONE"


out = []
w = out.append
w("/* GENERATED by tools/gen_spu_interp.py -- do not edit. */")
w("/* Opcode->mnemonic enum and the insn>>21 decode table for the SPU")
w(" * interpreter, from the validated spu_disasm.py decoder. Regenerate after")
w(" * any decoder table change. */")
w("")
w("typedef enum {")
for m in mnems:
    w(f"    {enum_name(m)},")
w("    SPU_OP_COUNT")
w("} spu_op;")
w("")
w("static const char* const spu_op_name[SPU_OP_COUNT] = {")
for m in mnems:
    w(f'    [{enum_name(m)}] = "{m}",')
w("};")
w("")
w("/* How spu_decode1 derives imm/tgt. */")
w("typedef enum {")
for f, why in FORMS:
    w(f"    SPU_F_{f},  /* {why} */")
w("} spu_decode_form;")
w("")
w("typedef struct { uint8_t op; uint8_t form; } spu_decode_ent;")
w('_Static_assert(SPU_OP_COUNT <= 256, "spu_op must fit spu_decode_ent.op");')
w("")
w("/* Indexed by insn >> 21; precedence is documented in tools/gen_spu_interp.py. */")
w("static const spu_decode_ent spu_decode_tab[2048] = {")
for base in range(0, 2048, 4):
    cells = []
    for p in range(base, base + 4):
        mn, form = decode_prefix(p)
        cells.append("{%s, SPU_F_%s}," % (enum_name(mn), form))
    w(f"    /*{base:03X}*/ " + " ".join(cells))
w("};")

sys.stdout.write("\n".join(out) + "\n")
