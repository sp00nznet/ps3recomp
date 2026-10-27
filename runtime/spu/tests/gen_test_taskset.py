#!/usr/bin/env python3
"""SPU bring-up rung 4 — cellSpurs taskset / job-queue throughput.

The SPU job is a stateless task body: DMA-GET a work item from WORK_EA, produce a
result at RESULT_EA, raise a completion event. A cellSpurs-style taskset (host
side) drains a cellSync LFQueue and invokes this lifted job once per work item —
the SPURS work-engine model (one lifted task processing a queue of workloads),
bridging the lv2 SPU-thread layer to the lifted-execution layer.
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
WORK_EA, RESULT_EA, LS_SCRATCH = 0x180, 0x1C0, 0x200

b = b""
# DMA GET: work item (main mem WORK_EA) -> LS_SCRATCH
b += ri16(IL, LS_SCRATCH, 3) + ri16(IL, 0, 4) + ri16(IL, WORK_EA, 5)
b += ri16(IL, 16, 6) + ri16(IL, 0, 7) + ri16(IL, MFC_GET, 8)
b += ch(WRCH, MFC_LSA, 3) + ch(WRCH, MFC_EAH, 4) + ch(WRCH, MFC_EAL, 5)
b += ch(WRCH, MFC_Size, 6) + ch(WRCH, MFC_TagID, 7) + ch(WRCH, MFC_Cmd, 8)
# DMA PUT: LS_SCRATCH -> result (main mem RESULT_EA)
b += ri16(IL, RESULT_EA, 5) + ri16(IL, MFC_PUT, 8)
b += ch(WRCH, MFC_LSA, 3) + ch(WRCH, MFC_EAH, 4) + ch(WRCH, MFC_EAL, 5)
b += ch(WRCH, MFC_Size, 6) + ch(WRCH, MFC_TagID, 7) + ch(WRCH, MFC_Cmd, 8)
# raise completion event
b += ri16(IL, 0x7A5C, 11)            # il r11, taskset completion code
b += ch(WRCH, WrOutIntrMbox, 11)
b += rr(STOP, 0, 0, 0)

elf = wrap(b, base=0, entry=0, symbols=[{"name": "main", "addr": 0, "size": len(b)}])
open(os.path.join(HERE, "test_taskset.elf"), "wb").write(elf)
print(f"Wrote test_taskset.elf ({len(b)} bytes code)")
