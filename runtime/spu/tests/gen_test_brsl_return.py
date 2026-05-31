#!/usr/bin/env python3
"""Test #3: brsl + bi $r0 (SPU ABI call/return)."""
import struct, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "tools"))
from wrap_spu_elf import wrap

def w(v): return struct.pack(">I", v & 0xFFFFFFFF)
def ri16(op9, i16, rt): return w(((op9 & 0x1FF) << 23) | ((i16 & 0xFFFF) << 7) | (rt & 0x7F))
def rr(op11, rb, ra, rt): return w(((op11 & 0x7FF) << 21) | ((rb & 0x7F) << 14) | ((ra & 0x7F) << 7) | (rt & 0x7F))
def ch(op11, channel, rt): return w(((op11 & 0x7FF) << 21) | ((channel & 0x1F) << 7) | (rt & 0x7F))

b = b""
b += ri16(0x81, 42, 3)                  # 0x00 il   r3, 42
b += ri16(0x66, (0x10-0x04)//4, 0)      # 0x04 brsl r0, 0x10
b += ch(0x10D, 28, 3)                   # 0x08 wrch SPU_WrOutMbox, r3
b += rr(0x000, 0, 0, 0)                 # 0x0C stop
b += ri16(0x81, 100, 4)                 # 0x10 il   r4, 100
b += rr(0x0C0, 4, 3, 3)                 # 0x14 a    r3, r3, r4
b += rr(0x1A8, 0, 0, 0)                 # 0x18 bi   r0

elf = wrap(b, base=0, entry=0,
           symbols=[{"name":"main",  "addr":0x00, "size":0x10},
                    {"name":"add100","addr":0x10, "size":0x0C}])
open(os.path.join(HERE, "test_brsl_return.elf"), "wb").write(elf)
print(f"Wrote test_brsl_return.elf ({len(b)} code)")
