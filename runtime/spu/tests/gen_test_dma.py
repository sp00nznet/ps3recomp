#!/usr/bin/env python3
"""Test #4: MFC DMA GET round-trip (vm_base+0 -> LS+0x100 -> lqd -> mbox)."""
import struct, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "tools"))
from wrap_spu_elf import wrap

def w(v): return struct.pack(">I", v & 0xFFFFFFFF)
def ri16(op9, i16, rt): return w(((op9 & 0x1FF) << 23) | ((i16 & 0xFFFF) << 7) | (rt & 0x7F))
def ri10(op8, i10, ra, rt): return w(((op8 & 0xFF) << 24) | ((i10 & 0x3FF) << 14) | ((ra & 0x7F) << 7) | (rt & 0x7F))
def rr(op11, rb, ra, rt): return w(((op11 & 0x7FF) << 21) | ((rb & 0x7F) << 14) | ((ra & 0x7F) << 7) | (rt & 0x7F))
def ch(op11, channel, rt): return w(((op11 & 0x7FF) << 21) | ((channel & 0x1F) << 7) | (rt & 0x7F))

MFC_LSA, MFC_EAH, MFC_EAL, MFC_Size, MFC_TagID, MFC_Cmd = 16, 17, 18, 19, 20, 21
WrOutMbox = 28
MFC_GET   = 0x40

b = b""
b += ri16(0x81, 0x100,    3)        # 0x00 il   r3, 0x100
b += ri16(0x81, 0,        4)        # 0x04 il   r4, 0
b += ri16(0x81, 0,        5)        # 0x08 il   r5, 0
b += ri16(0x81, 16,       6)        # 0x0C il   r6, 16
b += ri16(0x81, 0,        7)        # 0x10 il   r7, 0
b += ri16(0x81, MFC_GET,  8)        # 0x14 il   r8, 0x40
b += ch(0x10D, MFC_LSA,   3)        # 0x18 wrch MFC_LSA,   r3
b += ch(0x10D, MFC_EAH,   4)        # 0x1C wrch MFC_EAH,   r4
b += ch(0x10D, MFC_EAL,   5)        # 0x20 wrch MFC_EAL,   r5
b += ch(0x10D, MFC_Size,  6)        # 0x24 wrch MFC_Size,  r6
b += ch(0x10D, MFC_TagID, 7)        # 0x28 wrch MFC_TagID, r7
b += ch(0x10D, MFC_Cmd,   8)        # 0x2C wrch MFC_Cmd,   r8
b += ri16(0x81, 0,        9)        # 0x30 il   r9, 0
b += ri10(0x34, 0x10,    9, 10)     # 0x34 lqd  r10, 0x100(r9)
b += ch(0x10D, WrOutMbox, 10)       # 0x38 wrch SPU_WrOutMbox, r10
b += rr(0x000, 0, 0, 0)             # 0x3C stop

elf = wrap(b, base=0, entry=0,
           symbols=[{"name":"main","addr":0,"size":len(b)}])
open(os.path.join(HERE, "test_dma.elf"), "wb").write(elf)
print(f"Wrote test_dma.elf ({len(b)} code)")
