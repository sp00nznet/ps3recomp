#!/usr/bin/env python3
"""SPU bring-up rung 5 brick — lifted-job adapter / SPURS task ABI.

The SPU job receives its argument (the task descriptor effective address) in r3 —
the SPURS task ABI the lv2 SPU-thread layer + spu_run_lifted_job set up. It DMA-GETs
16 bytes from [r3], echoes them to [r3+0x40] (a result slot relative to the arg), and
raises a completion event. The test drives it via spu_run_lifted_job (the lv2<->lifted
bridge helper), proving a lifted job runs with a PPU-supplied arg pointer.
"""
import struct, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "tools"))
from wrap_spu_elf import wrap

def w(v): return struct.pack(">I", v & 0xFFFFFFFF)
def ri16(op9, i16, rt): return w(((op9 & 0x1FF) << 23) | ((i16 & 0xFFFF) << 7) | (rt & 0x7F))
def ri10(op8, i10, ra, rt): return w(((op8 & 0xFF) << 24) | ((i10 & 0x3FF) << 14) | ((ra & 0x7F) << 7) | (rt & 0x7F))
def rr(op11, rb, ra, rt): return w(((op11 & 0x7FF) << 21) | ((rb & 0x7F) << 14) | ((ra & 0x7F) << 7) | (rt & 0x7F))
def ch(op11, channel, rt): return w(((op11 & 0x7FF) << 21) | ((channel & 0x1F) << 7) | (rt & 0x7F))

IL, AI, WRCH, STOP = 0x81, 0x1C, 0x10D, 0x000
MFC_LSA, MFC_EAH, MFC_EAL, MFC_Size, MFC_TagID, MFC_Cmd = 16, 17, 18, 19, 20, 21
WrOutIntrMbox = 30
MFC_GET, MFC_PUT = 0x40, 0x20

b = b""
b += ri10(AI, 0, 3, 10)              # ai  r10, r3, 0     ; r10 = args_ea (task arg in r3)
# DMA GET: LS+0x200 <- [args_ea], 16 bytes
b += ri16(IL, 0x200, 3) + ri16(IL, 0, 4) + ri10(AI, 0, 10, 5)   # LSA / EAH / EAL=args_ea
b += ri16(IL, 16, 6) + ri16(IL, 0, 7) + ri16(IL, MFC_GET, 8)
b += ch(WRCH, MFC_LSA, 3) + ch(WRCH, MFC_EAH, 4) + ch(WRCH, MFC_EAL, 5)
b += ch(WRCH, MFC_Size, 6) + ch(WRCH, MFC_TagID, 7) + ch(WRCH, MFC_Cmd, 8)
# DMA PUT: LS+0x200 -> [args_ea + 0x40]
b += ri10(AI, 0x40, 10, 5) + ri16(IL, MFC_PUT, 8)               # EAL = args_ea + 0x40
b += ch(WRCH, MFC_LSA, 3) + ch(WRCH, MFC_EAH, 4) + ch(WRCH, MFC_EAL, 5)
b += ch(WRCH, MFC_Size, 6) + ch(WRCH, MFC_TagID, 7) + ch(WRCH, MFC_Cmd, 8)
# completion event
b += ri16(IL, 0x0ADA, 11) + ch(WRCH, WrOutIntrMbox, 11)
b += rr(STOP, 0, 0, 0)

elf = wrap(b, base=0, entry=0, symbols=[{"name": "main", "addr": 0, "size": len(b)}])
open(os.path.join(HERE, "test_adapter.elf"), "wb").write(elf)
print(f"Wrote test_adapter.elf ({len(b)} bytes code)")
