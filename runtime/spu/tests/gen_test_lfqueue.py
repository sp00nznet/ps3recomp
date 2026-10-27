#!/usr/bin/env python3
"""SPU bring-up rung 3 — cellSync LFQueue work channel.

The PPU enqueues a work item into a real cellSync LFQueue (libs/sync/cellSync.c)
whose data buffer lives in shared "main memory". The SPU DMA-reads the queued
item, "processes" it (echo, for the prototype) into a result slot, and raises a
completion event. The PPU receives the event, reads the result, and drains the
queue with cellSyncLFQueuePop. This is the PPU<->SPU work-distribution path flOw
uses (cellSyncLFQueue* + SPU DMA), in miniature.
"""
import struct, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "tools"))
from wrap_spu_elf import wrap

def w(v): return struct.pack(">I", v & 0xFFFFFFFF)
def ri16(op9, i16, rt): return w(((op9 & 0x1FF) << 23) | ((i16 & 0xFFFF) << 7) | (rt & 0x7F))
def rr(op11, rb, ra, rt): return w(((op11 & 0x7FF) << 21) | ((rb & 0x7F) << 14) | ((ra & 0x7F) << 7) | (rt & 0x7F))
def ch(op11, channel, rt): return w(((op11 & 0x7FF) << 21) | ((channel & 0x1F) << 7) | (rt & 0x7F))

IL, WRCH, STOP = 0x81, 0x10D, 0x000
MFC_LSA, MFC_EAH, MFC_EAL, MFC_Size, MFC_TagID, MFC_Cmd = 16, 17, 18, 19, 20, 21
WrOutIntrMbox = 30
MFC_GET, MFC_PUT = 0x40, 0x20
WORK_EA, RESULT_EA, LS_SCRATCH = 0x100, 0x140, 0x200   # queue buffer slot 0 / result / LS

b = b""
# --- DMA GET: queued work item (main mem WORK_EA) -> LS_SCRATCH ---
b += ri16(IL, LS_SCRATCH, 3)      # il r3, LSA
b += ri16(IL, 0,          4)      # il r4, EAH=0
b += ri16(IL, WORK_EA,    5)      # il r5, EAL = queue buffer slot 0
b += ri16(IL, 16,         6)      # il r6, Size
b += ri16(IL, 0,          7)      # il r7, Tag=0
b += ri16(IL, MFC_GET,    8)      # il r8, GET
b += ch(WRCH, MFC_LSA, 3) + ch(WRCH, MFC_EAH, 4) + ch(WRCH, MFC_EAL, 5)
b += ch(WRCH, MFC_Size, 6) + ch(WRCH, MFC_TagID, 7) + ch(WRCH, MFC_Cmd, 8)   # issue GET
# --- DMA PUT: LS_SCRATCH -> result slot (main mem RESULT_EA) ---
b += ri16(IL, RESULT_EA,  5)      # il r5, EAL = result slot
b += ri16(IL, MFC_PUT,    8)      # il r8, PUT
b += ch(WRCH, MFC_LSA, 3) + ch(WRCH, MFC_EAH, 4) + ch(WRCH, MFC_EAL, 5)
b += ch(WRCH, MFC_Size, 6) + ch(WRCH, MFC_TagID, 7) + ch(WRCH, MFC_Cmd, 8)   # issue PUT
# --- raise completion event ---
b += ri16(IL, 0x0D0E,    11)      # il r11, completion code
b += ch(WRCH, WrOutIntrMbox, 11)  # wrch SPU_WrOutIntrMbox, r11
b += rr(STOP, 0, 0, 0)            # stop

elf = wrap(b, base=0, entry=0, symbols=[{"name": "main", "addr": 0, "size": len(b)}])
open(os.path.join(HERE, "test_lfqueue.elf"), "wb").write(elf)
print(f"Wrote test_lfqueue.elf ({len(b)} bytes code)")
