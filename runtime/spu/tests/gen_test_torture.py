#!/usr/bin/env python3
"""SPU lifter torture KATs.

Each KAT was chosen to catch a bug class the LBP bring-up hit in the wild
(2026-07-18); had this suite existed, each would have failed in seconds
instead of costing days:

  K1  brhz  preferred-slot: il r,1     -> must NOT branch (halfword lo16 = 1).
      The historical `_u16[1]` bug read the HIGH half (0) and branched.
  K2  brhz  halfword-vs-word: ilhu r,1 -> MUST branch (lo16 = 0, word != 0).
      A full-word test would not branch.
  K3  brhnz mirror of K1/K2.
  K4  the exact binkspu decode-gate reduction: ceqbi/xsbh on a 0/1 flag,
      then brhz on the preferred halfword. flag=1 must fall through to the
      "decode" path; flag=0 must skip.
  K5  ila-continuation: `ila r29, cont; br sub`, sub returns via `bi r29`.
      Continuation is NOT a brsl target -- if function discovery misses it
      (the pre-collect_ila_continuation_targets state), the indirect branch
      is unresolved and the KAT value never arrives.
  K6  bi-loop stack soak: 100k iterations of a loop whose back-edge is a
      computed `bi`. With the historical plain-call dispatch this leaked
      ~250B/iteration (~25MB) and crashes the harness; flat dispatch passes.

Protocol: each KAT wrch's one marker word to SPU_WrOutMbox; the host main
records the sequence and compares against EXPECTED (also emitted below into
test_torture_expected.h). Ends with a stop.
"""
import struct, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "tools"))
from wrap_spu_elf import wrap

def w(v): return struct.pack(">I", v & 0xFFFFFFFF)
def ri16(op9, i16, rt): return w(((op9 & 0x1FF) << 23) | ((i16 & 0xFFFF) << 7) | (rt & 0x7F))
def ri18(op7, i18, rt): return w(((op7 & 0x7F) << 25) | ((i18 & 0x3FFFF) << 7) | (rt & 0x7F))
def ri10(op8, i10, ra, rt): return w(((op8 & 0xFF) << 24) | ((i10 & 0x3FF) << 14) | ((ra & 0x7F) << 7) | (rt & 0x7F))
def rr(op11, rb, ra, rt): return w(((op11 & 0x7FF) << 21) | ((rb & 0x7F) << 14) | ((ra & 0x7F) << 7) | (rt & 0x7F))
def ch(op11, channel, rt): return w(((op11 & 0x7FF) << 21) | ((channel & 0x1F) << 7) | (rt & 0x7F))

OP_IL    = 0x081   # il   rt, s16
OP_ILH   = 0x083   # ilh  rt, u16
OP_ILHU  = 0x082   # ilhu rt, u16
OP_IOHL  = 0x0C1   # iohl rt, u16
OP_ILA   = 0x21    # ila  rt, u18 (7-bit op)
OP_BR    = 0x64    # br   s16 (word offset, 9-bit op RI16-style: op9=0xC8? no ->)
OP_BRZ   = 0x40    # brz  rt, s16    (op9 0x080? SPU: brz=0b001000000=0x40<<... )
# SPU branch opcodes (RI16 form, 9-bit opcode field):
OP9_BR    = 0x064  # br
OP9_BRSL  = 0x066  # brsl
OP9_BRZ   = 0x040  # brz
OP9_BRNZ  = 0x042  # brnz
OP9_BRHZ  = 0x044  # brhz
OP9_BRHNZ = 0x046  # brhnz
OP11_BI   = 0x1A8  # bi ra
OP11_CEQBI= None
OP_WRCH   = 0x10D  # wrch
CH_OUTMBOX = 28

def il(rt, s16):   return ri16(OP_IL,   s16, rt)
def ilh(rt, u16):  return ri16(OP_ILH,  u16, rt)
def ilhu(rt, u16): return ri16(OP_ILHU, u16, rt)
def iohl(rt, u16): return ri16(OP_IOHL, u16, rt)
def ila(rt, u18):  return ri18(OP_ILA,  u18, rt)
def ai(rt, ra, s10): return ri10(0x1C, s10, ra, rt)
def ceqbi(rt, ra, s10): return ri10(0x7E, s10, ra, rt)
def xsbh(rt, ra):  return rr(0x2B6, 0, ra, rt)
def wrch_out(rt):  return ch(OP_WRCH, CH_OUTMBOX, rt)
def br(word_off):   return ri16(OP9_BR,    word_off, 0)
def brsl(rt, word_off): return ri16(OP9_BRSL, word_off, rt)
def brz(rt, word_off):  return ri16(OP9_BRZ,  word_off, rt)
def brnz(rt, word_off): return ri16(OP9_BRNZ, word_off, rt)
def brhz(rt, word_off): return ri16(OP9_BRHZ, word_off, rt)
def brhnz(rt, word_off):return ri16(OP9_BRHNZ,word_off, rt)
def bi(ra):        return rr(OP11_BI, 0, ra, 0)
def stop():        return w(0)

# ---------------------------------------------------------------------------
# Assemble with a tiny two-pass label system: instructions are (emit_fn) or
# ('label', name); branch emitters take a label and get patched in pass 2.
# ---------------------------------------------------------------------------
prog = []          # list of ('ins', bytes) / ('label', name) / ('branch', kind, rt, label)
def L(name): prog.append(('label', name))
def I(bs):   prog.append(('ins', bs))
def B(kind, rt, label): prog.append(('branch', kind, rt, label))

EXPECTED = []
def expect(v): EXPECTED.append(v & 0xFFFFFFFF)

def emit_marker(rt, val):
    """il/ilhu+iohl the 32-bit marker into rt and wrch it."""
    I(ilhu(rt, (val >> 16) & 0xFFFF)); I(iohl(rt, val & 0xFFFF)); I(wrch_out(rt))

# --- K1: brhz on il r3,1 -> preferred halfword (lo16)=1 -> NOT taken --------
I(il(3, 1))
B('brhz', 3, 'k1_bad')
emit_marker(4, 0x0AA10001); expect(0x0AA10001)   # fall-through = correct
B('br', 0, 'k2')
L('k1_bad'); emit_marker(4, 0x0DEAD001)          # branch taken = _u16[1] bug
L('k2')

# --- K2: brhz on ilhu r3,1 (word=0x00010000, lo16=0) -> MUST be taken -------
I(ilhu(3, 1))
B('brhz', 3, 'k2_good')
emit_marker(4, 0x0DEAD002)                       # fall-through = word-test bug
B('br', 0, 'k3')
L('k2_good'); emit_marker(4, 0x0AA10002); expect(0x0AA10002)
L('k3')

# --- K3: brhnz mirrors ------------------------------------------------------
I(il(3, 1))
B('brhnz', 3, 'k3_good')
emit_marker(4, 0x0DEAD003)
B('br', 0, 'k3b')
L('k3_good'); emit_marker(4, 0x0AA10003); expect(0x0AA10003)
L('k3b')
I(ilhu(3, 1))                                    # lo16=0 -> brhnz NOT taken
B('brhnz', 3, 'k3_bad')
emit_marker(4, 0x0AA10004); expect(0x0AA10004)
B('br', 0, 'k4')
L('k3_bad'); emit_marker(4, 0x0DEAD004)
L('k4')

# --- K4: the binkspu decode-gate reduction ---------------------------------
# flag=1: r87=1 -> ceqbi r85,r87,0 -> xsbh r83,r85 -> brhz r83 must NOT skip
I(il(87, 1))
I(ceqbi(85, 87, 0))
I(xsbh(83, 85))
B('brhz', 83, 'k4_decode')                       # lo16 after xsbh = 0x0000 -> TAKEN
emit_marker(4, 0x0DEAD005)                       # not taken = gate stuck closed
B('br', 0, 'k4b')
L('k4_decode'); emit_marker(4, 0x0AA10005); expect(0x0AA10005)
L('k4b')
# flag=0 must skip (lo16 = 0xFFFF -> NOT taken)
I(il(87, 0))
I(ceqbi(85, 87, 0))
I(xsbh(83, 85))
B('brhz', 83, 'k4_bad2')
emit_marker(4, 0x0AA10006); expect(0x0AA10006)
B('br', 0, 'k5')
L('k4_bad2'); emit_marker(4, 0x0DEAD006)
L('k5')

# --- K5: ila-continuation (caller-side ila, callee-side bi) ----------------
B('ila', 29, 'k5_cont')                          # ila r29, cont  (label-patched)
B('br', 0, 'k5_sub')
L('k5_cont')                                     # <- NOT a brsl target!
emit_marker(4, 0x0AA10007); expect(0x0AA10007)
B('br', 0, 'k6')
L('k5_sub')
I(ai(30, 30, 1))                                 # token work
I(bi(29))                                        # "return" to continuation
L('k6')

# --- K6: bi-loop stack soak: 100000 iterations through a computed branch ----
I(ilhu(20, 100000 >> 16)); I(iohl(20, 100000 & 0xFFFF))   # r20 = counter
B('ila', 21, 'k6_loop')                          # r21 = loop head address
L('k6_loop')
I(ai(20, 20, -1))                                # counter--
B('brz', 20, 'k6_done')
I(bi(21))                                        # back-edge via computed branch
L('k6_done')
emit_marker(4, 0x0AA10008); expect(0x0AA10008)

I(stop())

# ---- two-pass assembly ----
addr = 0; addrs = {}
for p in prog:
    if p[0] == 'label': addrs[p[1]] = addr
    else: addr += 4
out = b""; addr = 0
for p in prog:
    if p[0] == 'label': continue
    if p[0] == 'ins':
        out += p[1]; addr += 4; continue
    _, kind, rt, label = p
    t = addrs[label]
    if kind == 'ila':
        out += ila(rt, t)
    else:
        if kind == 'br':
            out += br((t - addr) >> 2)
        else:
            emit = {'brsl': brsl, 'brz': brz, 'brnz': brnz,
                    'brhz': brhz, 'brhnz': brhnz}[kind]
            out += emit(rt, (t - addr) >> 2)
    addr += 4

elf = wrap(out, base=0, entry=0, symbols=[{"name": "main", "addr": 0, "size": 0}])
open(os.path.join(HERE, "test_torture.elf"), "wb").write(elf)
with open(os.path.join(HERE, "test_torture_expected.h"), "w") as f:
    f.write("/* generated by gen_test_torture.py */\n")
    f.write(f"static const unsigned TORTURE_EXPECTED[] = {{ "
            + ", ".join(f"0x{v:08X}u" for v in EXPECTED) + " };\n")
    f.write(f"enum {{ TORTURE_EXPECTED_N = {len(EXPECTED)} }};\n")
print(f"Wrote test_torture.elf ({len(out)} bytes code), {len(EXPECTED)} expected markers")
