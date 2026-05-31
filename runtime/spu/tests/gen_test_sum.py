#!/usr/bin/env python3
"""Test #1: sum loop -- reads N values from inbound mailbox, sums them,
writes the result to outbound mailbox."""
import struct, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "tools"))
from wrap_spu_elf import wrap

def w(v): return struct.pack(">I", v & 0xFFFFFFFF)
def ri16(op9, i16, rt): return w(((op9 & 0x1FF) << 23) | ((i16 & 0xFFFF) << 7) | (rt & 0x7F))
def ri10(op8, i10, ra, rt): return w(((op8 & 0xFF) << 24) | ((i10 & 0x3FF) << 14) | ((ra & 0x7F) << 7) | (rt & 0x7F))
def rr(op11, rb, ra, rt): return w(((op11 & 0x7FF) << 21) | ((rb & 0x7F) << 14) | ((ra & 0x7F) << 7) | (rt & 0x7F))
def ch(op11, channel, rt): return w(((op11 & 0x7FF) << 21) | ((channel & 0x1F) << 7) | (rt & 0x7F))

N = 4
LOOP = 0x08
b = b""
b += ri16(0x81, 0, 3)                         # 0x00 il   r3, 0
b += ri16(0x81, N, 4)                         # 0x04 il   r4, N
b += ch(0x00D, 29, 6)                         # 0x08 rdch r6, SPU_RdInMbox
b += rr(0x0C0, 6, 3, 3)                       # 0x0C a    r3, r3, r6
b += ri10(0x1C, -1 & 0x3FF, 4, 4)             # 0x10 ai   r4, r4, -1
b += ri16(0x42, (LOOP - 0x14)//4 & 0xFFFF, 4) # 0x14 brnz r4, loop
b += ch(0x10D, 28, 3)                         # 0x18 wrch SPU_WrOutMbox, r3
b += rr(0x000, 0, 0, 0)                       # 0x1C stop

elf = wrap(b, base=0, entry=0,
           symbols=[{"name":"main","addr":0,"size":len(b)}])
open(os.path.join(HERE, "test_sum.elf"), "wb").write(elf)
print(f"Wrote test_sum.elf ({len(b)} code, {len(b)//4} insns)")
