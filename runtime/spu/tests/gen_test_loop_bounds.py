#!/usr/bin/env python3
"""T-0001 regression: the inFamous WWS scatter loop's bound contract.

Transcribes (from the spu_0003_at_006A3300 lift) the exact instruction
sequence of the game's 0x12D80 loop head and 0x12E40 steady-state body —
the loop whose runaway flattens the whole 256 KB local store when fed a
bad element count (r5) or a shift parameter > 7 (the shl/shlqbi masking
asymmetry: the counter's shift honors 6 bits, the step's only 3). The
data path (fma/lqx input streams) is replaced by constants; the CONTROL
PATH — counter init (shli+shl+sfi), step (shlqbi), exit test (ceqi+selb),
offsets (a/ai), stores (stqx) and the selb+bi indirect branch — is the
game's own idiom, opcode for opcode and register for register.

test_loop_bounds_main.c runs it for (count, shiftA, shiftB) sets and
asserts, by sentinel-scanning the whole 256 KB local store:

  * consistent parameters -> the loop exits and writes EXACTLY count
    records inside the predicted span, NOTHING anywhere else;
  * any store outside the span (the T-0001 flood signature) fails loudly.

run_tests.sh exercises the lifted code; spu_interp_selftest.c runs the
same encoding through the interpreter, so both execution paths are
pinned.
"""
import struct, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "tools"))
from wrap_spu_elf import wrap

def w(v): return struct.pack(">I", v & 0xFFFFFFFF)
def ri16(op9, i16, rt): return w(((op9 & 0x1FF) << 23) | ((i16 & 0xFFFF) << 7) | (rt & 0x7F))
def ri10(op8, i10, ra, rt): return w(((op8 & 0xFF) << 24) | ((i10 & 0x3FF) << 14) | ((ra & 0x7F) << 7) | (rt & 0x7F))
# shift-left-word IMMEDIATE: 11-bit opcode 0x7B + 7-bit shift (0-63) + ra + rt
def shli(i7, ra, rt): return w((0x7B << 21) | ((i7 & 0x7F) << 14) | ((ra & 0x7F) << 7) | (rt & 0x7F))
# RI7 quadword shift/rotate immediates (shlqbii 0x1FB etc.): same field
# layout as shli -- 11-bit opcode, 7-bit immediate, ra, rt. The immediate is
# masked to 3 bits by the hardware for the by-bits forms (RPCS3's static
# interpreter: op.i7 & 7 -- verified 2026-10-06), so only the low bits count.
def ri7(op11, i7, ra, rt): return w(((op11 & 0x7FF) << 21) | ((i7 & 0x7F) << 14) | ((ra & 0x7F) << 7) | (rt & 0x7F))
def rr(op11, rb, ra, rt): return w(((op11 & 0x7FF) << 21) | ((rb & 0x7F) << 14) | ((ra & 0x7F) << 7) | (rt & 0x7F))
# RRR (MSB-first): OP(4) RT(7) RB(7) RA(7) RC(7) -- rc selects bits (spu_selb(a,b,c) = c&b | ~c&a)
def selb(rt, ra, rb, rc): return w((0x8 << 28) | ((rt & 0x7F) << 21) | ((rb & 0x7F) << 14) | ((ra & 0x7F) << 7) | (rc & 0x7F))
def ila(imm18, rt): return w((0x21 << 25) | ((imm18 & 0x3FFFF) << 7) | (rt & 0x7F))

LNOP = w(0x00200000)

HEAD = 0x00
BODY = 0x80
EXIT = 0xC0

# ---- head: r4 = base, r5 = count, r8/r9 = shift params (harness-set);
#      r0 = exit link (harness-set). Initializes the loop exactly as the
#      game's 0x12D80 does. ----
h = b""
h += ri16(0x81, 16, 11)              # il     $11, 16        shift unit
h += shli(4, 5, 2)                   # shli   $2, $5, 4       r2 = count*16
h += rr(0x5B, 8, 2, 2)              # shl    $2, $2, $8      counter init (6-bit shift)  <-- T-0001 asymmetry
h += rr(0x1DB, 8, 11, 8)            # shlqbi $8, $11, $8     step (3-bit mask)          <-- pair
h += ri10(0x0C, 0, 2, 2)            # sfi    $2, $2, 0       r2 = -(...)
h += rr(0x1DB, 9, 11, 9)            # shlqbi $9, $11, $9     stream stride
h += rr(0x0C0, 9, 4, 19)            # a      $19, $9, $4     stream bases: base + k*stride
h += rr(0x0C0, 9, 19, 23)           # a      $23, $9, $19
h += rr(0x0C0, 9, 23, 27)           # a      $27, $9, $23
# --- the x4 step (missing from the first transcription; found 2026-10-06 by
# re-reading the lift). The real 0x12D80 head doubles BOTH strides by 2 BITS
# (x4) AFTER computing the stream bases, so the steady-state body steps the
# counter by 64<<(sa&7) and the store offset by 64<<(sb&7), while the four
# stream bases stay one S = 16<<(sb&7) apart. The loop therefore runs cnt/4
# iterations (cnt MUST be divisible by 4 to exit at all) and writes exactly
# cnt records -- the "32-128 iterations" live telemetry is cnt 128..512. ---
h += ri7(0x1FB, 2, 8, 8)            # shlqbii $8, $8, 2      step   := 4 * (16<<(sa&7))
h += ri7(0x1FB, 2, 9, 9)            # shlqbii $9, $9, 2      stride := 4 * (16<<(sb&7))
h += ri16(0x81, 0, 12)               # il     $12, 0          store offset
h += ri16(0x81, 0, 14)               # il     $14, 0          store step: 0 in the head
                                      # (the real 0x12D9C does shlqbyi $14,$3,16 = 0).
                                      # Round 1's r12 += r14 adds 0, so SET 0 IS
                                      # STORED TWICE (round 1 with the pipeline's
                                      # seed records, round 2 with round-1 fma
                                      # results); the body re-sets r14 := 4S each
                                      # round. Exit round stores too: rounds =
                                      # cnt/4+1, records = cnt+4, contiguous span
                                      # (sb=0) = (cnt-1)*S + 16 bytes.
h += ri16(0x81, 0x1337, 13)         # il     $13, 0x1337      record word, stream 0
h += ri16(0x81, 0x3434, 34)          # il     $34, 0x3434      stream 1
h += ri16(0x81, 0x3535, 35)          # il     $35, 0x3535      stream 2
h += ri16(0x81, 0x3636, 36)          # il     $36, 0x3636      stream 3
h += ila(BODY, 15)                   # ila    $15, BODY        loop address for selb
br_at = HEAD + len(h)
h += ri16(0x64, (BODY - br_at) // 4, 0)   # br  -> BODY
while len(h) < BODY:
    h += LNOP

# ---- body: the game's steady-state scatter iteration (0x12E40) ----
b = b""
b += ri10(0x7C, 0, 2, 37)            # ceqi   $37, $2, 0      exit test
b += rr(0x144, 12, 4, 13)            # stqx   $13, $4, $12     record store, stream 0
b += rr(0x0C0, 2, 8, 2)              # a      $2, $8, $2       counter += step
b += rr(0x144, 12, 19, 34)           # stqx   $34, $19, $12     stream 1
b += rr(0x0C0, 9, 10, 10)            # a      $10, $10, $9      (input offset, kept)
b += rr(0x144, 12, 23, 35)           # stqx   $35, $23, $12     stream 2
b += rr(0x144, 12, 27, 36)           # stqx   $36, $27, $12     stream 3
b += selb(37, 15, 0, 37)            # selb   $37, $15, $0, $37  loop-or-exit
b += rr(0x0C0, 14, 12, 12)           # a      $12, $14, $12     offset += step
b += ri10(0x1C, 0, 9, 14)            # ai     $14, $9, 0        (kept)
b += w((0x1A8 << 21) | (37 << 7))     # bi     $37              indirect branch idiom (reg in RA field)
while len(b) < EXIT - BODY:
    b += LNOP

# ---- exit stub: reached only when selb picks r0 (counter hit zero) ----
e = w(0x00000000)                    # stop

prog = h + b + e
assert len(prog) == 0xC4, hex(len(prog))
elf = wrap(prog, base=0, entry=HEAD,
           symbols=[{"name": "main", "addr": HEAD, "size": len(h)},
                    {"name": "scatter_body", "addr": BODY, "size": len(b)},
                    {"name": "exit_stub", "addr": EXIT, "size": len(e)}])
open(os.path.join(HERE, "test_loop_bounds.elf"), "wb").write(elf)
print(f"Wrote test_loop_bounds.elf ({len(prog)} code, head@{HEAD:#x} body@{BODY:#x} exit@{EXIT:#x})")
