#!/usr/bin/env python3
"""Test #2: shufb with special selector patterns (0xC0/0xE0/0x80 magic bytes)."""
import struct, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "tools"))
from wrap_spu_elf import wrap

def w(v): return struct.pack(">I", v & 0xFFFFFFFF)
def ri16(op9, i16, rt): return w(((op9 & 0x1FF) << 23) | ((i16 & 0xFFFF) << 7) | (rt & 0x7F))
def rr(op11, rb, ra, rt): return w(((op11 & 0x7FF) << 21) | ((rb & 0x7F) << 14) | ((ra & 0x7F) << 7) | (rt & 0x7F))
def ch(op11, channel, rt): return w(((op11 & 0x7FF) << 21) | ((channel & 0x1F) << 7) | (rt & 0x7F))
def rrr(op4, rt, ra, rb, rc):
    # MSB->LSB: op4(4) | rc(7) | rb(7) | ra(7) | rt(7)
    return w(((op4 & 0xF) << 28) | ((rc & 0x7F) << 21) | ((rb & 0x7F) << 14)
             | ((ra & 0x7F) << 7) | (rt & 0x7F))

b = b""
b += ri16(0x81, 0x7FFF, 3)       # il   r3, 0x7FFF       (irrelevant)
b += ri16(0x81, 0x7FFE, 4)       # il   r4, 0x7FFE       (irrelevant)
b += ri16(0x83, 0xC0E0, 5)       # ilh  r5, 0xC0E0       -> word[i] = 0xC0E0C0E0
b += rrr(0x8, 6, 3, 4, 5)        # shufb r6, r3, r4, r5
b += ch(0x10D, 28, 6)            # wrch SPU_WrOutMbox, r6
b += rr(0x000, 0, 0, 0)          # stop

elf = wrap(b, base=0, entry=0,
           symbols=[{"name":"main","addr":0,"size":len(b)}])
open(os.path.join(HERE, "test_shufb.elf"), "wb").write(elf)
print(f"Wrote test_shufb.elf ({len(b)} code)")
