#!/usr/bin/env python3
"""SPU bring-up rung 2 — event-driven completion.

Like the bring-up test, the SPU produces a result in shared memory via DMA PUT,
but signals completion via the *interrupt* mailbox (SPU_WrOutIntrMbox, ch 30) —
the channel that, on a connected SPU thread group, delivers a SYS_SPU_THREAD_GROUP
event to a PPU blocked in sys_event_queue_receive. The host harness models that
delivery (mirroring runtime/syscalls/sys_event.c's sys_event_queue_push_by_id +
sys_event_queue_receive) and verifies the PPU receives the completion event.
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

IL, STQD, WRCH, STOP = 0x81, 0x24, 0x10D, 0x000
MFC_LSA, MFC_EAH, MFC_EAL, MFC_Size, MFC_TagID, MFC_Cmd = 16, 17, 18, 19, 20, 21
WrOutIntrMbox = 30
MFC_PUT = 0x20

b = b""
# --- produce result 0x1234 in LS+0x100 ---
b += ri16(IL, 0x1234,  11)        # il   r11, 0x1234
b += ri16(IL, 0,        9)        # il   r9, 0
b += ri10(STQD, 0x10,  9, 11)     # stqd r11, 0x100(r9)
# --- DMA PUT: LS+0x100 -> main memory EA 0 ---
b += ri16(IL, 0x100,    3)        # il   r3, 0x100   (LSA)
b += ri16(IL, 0,        4)        # il   r4, 0       (EAH)
b += ri16(IL, 0,        5)        # il   r5, 0       (EAL)
b += ri16(IL, 16,       6)        # il   r6, 16      (Size)
b += ri16(IL, 0,        7)        # il   r7, 0       (TagID)
b += ri16(IL, MFC_PUT,  8)        # il   r8, 0x20    (PUT)
b += ch(WRCH, MFC_LSA,   3)
b += ch(WRCH, MFC_EAH,   4)
b += ch(WRCH, MFC_EAL,   5)
b += ch(WRCH, MFC_Size,  6)
b += ch(WRCH, MFC_TagID, 7)
b += ch(WRCH, MFC_Cmd,   8)       # issue PUT
# --- raise completion EVENT via the interrupt mailbox ---
b += ch(WRCH, WrOutIntrMbox, 11)  # wrch SPU_WrOutIntrMbox, r11
b += rr(STOP, 0, 0, 0)            # stop

elf = wrap(b, base=0, entry=0, symbols=[{"name": "main", "addr": 0, "size": len(b)}])
open(os.path.join(HERE, "test_event.elf"), "wb").write(elf)
print(f"Wrote test_event.elf ({len(b)} bytes code)")
