#!/usr/bin/env python3
"""Generate the multicore conformance ELF: PPU <-> SPU interaction over raw lv2
syscalls (no imports), checked by its printed transcript.

Tests (each prints hex lines on tty 0, deterministic by construction):
  A  DMA: one SPU GETs then PUTs a pattern buffer for every (size, alignment)
     in a table (1..16 KB, naturally aligned small transfers), then a GETL
     gather; the PPU prints the destination buffers.
  B  atomics: 6 SPUs each increment all four words of a 128-byte line N times
     with GETLLAR/PUTLLC retry loops while the PPU increments word 0 N times
     with lwarx/stwcx. -- expected word0 = 7N, words 1..3 = 6N.
  C  mailbox + signals: the PPU writes four values into the SPU's inbound
     mailbox and two SNR signals; the SPU sums them and exits with the sum.
  D  event ports: an SPU sends four sys_spu_thread_send_event events (each
     answered through its inbound mailbox) and one throw_event to a PPU event
     queue; the PPU prints every received event (r4..r7, the SPU thread id
     printed relative to the real one).
  E  PPU threads + kernel sync: 4 threads increment a shared counter under a
     sys_mutex (yielding inside the critical section) and post a semaphore;
     a sys_cond ping-pong between the main thread and a worker writes an
     alternating log; an event flag AND-wait wakes only when both bits are set.
  F  more MFC: GETLLAR/PUTLLC/PUTLLUC atomic status values, immediate tag
     status, and a GETL whose middle element has stall-and-notify set.
  G  PPU -> SPU events: the PPU sends three sys_event_port_send events through
     a local port to an SPU queue bound to the thread; the SPU receives them
     with sys_spu_thread_receive_event (stop 0x110) and PUTs status + data.
     The group's RUN event queue reports the start.
  Every group join prints (cause, status) and each thread's exit status.

The same bytes run on RPCS3 and through ps3recomp; run_mc_conform.py diffs.
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "ppu"))
import ppc_asm as P          # noqa: E402
import spu_asm as S          # noqa: E402
from gen_ppu_conform import Asm, write_elf, emit_routines, TEXT_BASE  # noqa: E402

SC = dict(process_exit=3, tty_write=403, spu_initialize=169, image_import=157,
          group_create=170, thread_initialize=172, group_start=173, group_join=178,
          get_exit_status=165, write_snr=184, write_spu_mb=190,
          equeue_create=128, equeue_receive=130, connect_event=191,
          thread_create=52, thread_start=53, thread_join=44, thread_exit=41, thread_yield=43,
          mutex_create=100, mutex_lock=102, mutex_unlock=104,
          cond_create=105, cond_wait=107, cond_signal=108,
          sem_create=90, sem_wait=92, sem_post=94,
          eflag_create=82, eflag_wait=85, eflag_set=87,
          group_connect_event=185, bind_queue=193, port_create=134,
          port_connect_local=136, port_send=138)
SPUQ = 0x20             # SPU queue number for test G
SPUP = 5                # SPU event port used by test D
N_MUTEX = 200           # increments per PPU thread in test E
N_PING = 16             # cond ping-pong rounds

N_ATOMIC = 400
SPU_BASE = 0x0          # SPU programs load at LS 0
LS_BUF = 0x10000        # data area in LS
DMA_CASES = ([(1, o) for o in (0, 1, 7, 15)] + [(2, o) for o in (0, 2, 14)] + [(4, o) for o in (0, 4, 12)] +
             [(8, o) for o in (0, 8)] + [(16 * k, 0) for k in (1, 2, 3, 7, 8, 64, 255, 1024)])


# ------------------------------------------------------------------ SPU programs

def spu_dma_prog():
    """arg1 (r3 pref word) = EA of a param block:
         +0 src EA, +4 dst EA, +8 count, then count x {size, src_off, dst_off, ls_off}
       For each entry: GET size bytes src+src_off -> LS ls_off, then PUT LS ls_off
       -> dst+dst_off. Then a GETL gather of the list at +0x400 into LS 0x30000 and
       a PUT of the gathered bytes to dst+0x8000. Exits with 0x600D."""
    a = S.SpuAsm(SPU_BASE)
    a.rotqbyi(3, 3, 4)                          # arg1 is a u64: low word -> preferred slot
    # GET the param block (2 KB: entries + the GETL list at +0x400) to LS 0x20000
    a.ila(10, 0x20000); a.il(11, 0x800)
    a.mfc(S.GET, 10, 3, 11, 1, 12); a.wait_tag(1, 12)
    a.lqa(20, 0x20000)                         # w0 src, w1 dst, w2 count
    a.ori(21, 20, 0)                            # r21 pref = src
    a.rotqbyi(22, 20, 4)                        # r22 pref = dst
    a.rotqbyi(23, 20, 8)                        # r23 pref = count
    a.ila(24, 0x20010)                          # r24 = LS ptr to entries
    a.label("loop")
    a.lqd(25, 24, 0)                            # size, src_off, dst_off, ls_off
    a.rotqbyi(26, 25, 4); a.rotqbyi(27, 25, 8); a.rotqbyi(28, 25, 12)
    a.a(29, 21, 26)                             # src EA
    a.mfc(S.GET, 28, 29, 25, 2, 12); a.wait_tag(2, 12)
    a.a(29, 22, 27)                             # dst EA
    a.mfc(S.PUT, 28, 29, 25, 2, 12); a.wait_tag(2, 12)
    a.ai(24, 24, 16); a.ai(23, 23, -1); a.brnz(23, "loop")
    # GETL: list at LS 0x20400 (copied in with the param block), 8 elements
    a.ila(10, 0x30000); a.ila(29, 0x20400); a.il(11, 8 * 8)
    a.wrch(S.MFC_LSA, 10)
    a.il(12, 0); a.wrch(S.MFC_EAH, 12)          # list elements' EA high word
    a.wrch(S.MFC_EAL, 29); a.wrch(S.MFC_Size, 11)
    a.il(12, 3); a.wrch(S.MFC_TagID, 12); a.il(12, S.GETL); a.wrch(S.MFC_Cmd, 12)
    a.wait_tag(3, 12)
    a.ila(10, 0x30000); a.ilhu(29, 0); a.iohl(29, 0x8000); a.a(29, 22, 29); a.il(11, 8 * 0x40)
    a.mfc(S.PUT, 10, 29, 11, 3, 12); a.wait_tag(3, 12)
    a.il(30, 0x600D); a.wrch(S.SPU_WrOutMbox, 30); a.stop(0x102)
    return a.bytes()


def spu_atomic_prog():
    """arg1 = EA of the 128-byte line, arg2 = thread index. N GETLLAR/PUTLLC
    increments of all four words of the line's first quadword. Exits with
    0xA000 | index."""
    a = S.SpuAsm(SPU_BASE)
    a.rotqbyi(3, 3, 4); a.rotqbyi(4, 4, 4)      # u64 args: low words -> preferred slots
    a.ila(10, 0x10000)                          # LS line buffer (128-aligned)
    a.ori(5, 4, 0)                              # keep index (r4 pref)
    a.il(20, N_ATOMIC); a.il(22, 1)
    a.label("again")
    a.wrch(S.MFC_LSA, 10); a.il(12, 0); a.wrch(S.MFC_EAH, 12); a.wrch(S.MFC_EAL, 3)
    a.il(12, S.GETLLAR); a.wrch(S.MFC_Cmd, 12); a.rdch(12, S.MFC_RdAtomicStat)
    a.lqd(21, 10, 0); a.a(21, 21, 22); a.stqd(21, 10, 0)
    a.wrch(S.MFC_LSA, 10); a.il(12, 0); a.wrch(S.MFC_EAH, 12); a.wrch(S.MFC_EAL, 3)
    a.il(12, S.PUTLLC); a.wrch(S.MFC_Cmd, 12); a.rdch(13, S.MFC_RdAtomicStat)
    a.brnz(13, "again")                         # bit 0 set: reservation lost, retry
    a.ai(20, 20, -1); a.brnz(20, "again")
    a.ilhu(30, 0); a.iohl(30, 0xA000); a.or_(30, 30, 5)
    a.wrch(S.SPU_WrOutMbox, 30); a.stop(0x102)
    return a.bytes()


def spu_mbox_prog():
    """Sum four inbound-mailbox words and both signal-notification registers;
    exit with the sum."""
    a = S.SpuAsm(SPU_BASE)
    a.il(30, 0)
    for _ in range(4):
        a.rdch(31, S.SPU_RdInMbox); a.a(30, 30, 31)
    a.rdch(31, S.SPU_RdSigNotify1); a.a(30, 30, 31)
    a.rdch(31, S.SPU_RdSigNotify2); a.a(30, 30, 31)
    a.wrch(S.SPU_WrOutMbox, 30); a.stop(0x102)
    return a.bytes()


def spu_event_prog():
    """Four sys_spu_thread_send_event (data0 0x100+k, data1 0x1000+k), each
    reply read from the inbound mailbox, then one throw_event. Exits with
    0xE000 + the sum of the replies (CELL_OK each)."""
    a = S.SpuAsm(SPU_BASE)
    a.il(20, 0)
    for k in range(4):
        a.li32(10, 0x1000 + k); a.wrch(S.SPU_WrOutMbox, 10)
        a.li32(11, (SPUP << 24) | (0x100 + k)); a.wrch(S.SPU_WrOutIntrMbox, 11)
        a.rdch(12, S.SPU_RdInMbox); a.a(20, 20, 12)
    a.li32(10, 0xBEEF); a.wrch(S.SPU_WrOutMbox, 10)
    a.li32(11, ((64 + SPUP) << 24) | 0x77); a.wrch(S.SPU_WrOutIntrMbox, 11)
    a.li32(30, 0xE000); a.a(30, 30, 20)
    a.wrch(S.SPU_WrOutMbox, 30); a.stop(0x102)
    return a.bytes()


def spu_mfc2_prog():
    """arg1 = EA of a block: +0x000 a 128-byte line, +0x080 a 3-element DMA
    list (filled by the PPU), +0x100 its source, +0x200 results. Records the
    atomic status after GETLLAR, PUTLLC (held), PUTLLC (no reservation) and
    PUTLLUC, the immediate tag status after a completed PUT, and the list
    stall status of a GETL whose element 1 has stall-and-notify; then PUTs the
    six status quadwords to +0x200 and the gathered bytes to +0x260."""
    a = S.SpuAsm(SPU_BASE)
    a.rotqbyi(3, 3, 4)
    a.ila(10, 0x10000)                                    # LS line
    a.il(0, 0)

    def atomic(cmd):
        a.wrch(S.MFC_LSA, 10); a.wrch(S.MFC_EAH, 0); a.wrch(S.MFC_EAL, 3)
        a.il(12, cmd); a.wrch(S.MFC_Cmd, 12)

    def save(r, i):
        a.ila(13, 0x13000 + 16 * i); a.stqd(r, 13, 0)

    atomic(S.GETLLAR); a.rdch(40, S.MFC_RdAtomicStat); save(40, 0)
    a.lqd(21, 10, 0); a.il(22, 1); a.a(21, 21, 22); a.stqd(21, 10, 0)
    atomic(S.PUTLLC); a.rdch(41, S.MFC_RdAtomicStat); save(41, 1)
    atomic(S.PUTLLC); a.rdch(42, S.MFC_RdAtomicStat); save(42, 2)   # no reservation: fails
    a.lqd(21, 10, 0); a.il(22, 0x10); a.a(21, 21, 22); a.stqd(21, 10, 0)
    atomic(S.PUTLLUC); a.rdch(43, S.MFC_RdAtomicStat); save(43, 3)
    # immediate tag status after a completed PUT on tag 5
    a.il(28, 0x2F0); a.a(29, 3, 28); a.il(11, 16)
    a.mfc(S.PUT, 10, 29, 11, 5, 12); a.wait_tag(5, 12)
    a.il(12, 1 << 5); a.wrch(S.MFC_WrTagMask, 12)
    a.il(12, 0); a.wrch(S.MFC_WrTagUpdate, 12); a.rdch(44, S.MFC_RdTagStat); save(44, 4)
    # GETL with stall-and-notify on element 1
    a.ila(14, 0x11000); a.ai(29, 3, 0x80); a.il(11, 32)
    a.mfc(S.GET, 14, 29, 11, 6, 12); a.wait_tag(6, 12)
    a.ila(15, 0x12000); a.il(11, 24)
    a.wrch(S.MFC_LSA, 15); a.wrch(S.MFC_EAH, 0); a.wrch(S.MFC_EAL, 14); a.wrch(S.MFC_Size, 11)
    a.il(12, 7); a.wrch(S.MFC_TagID, 12); a.il(12, S.GETL); a.wrch(S.MFC_Cmd, 12)
    a.rdch(45, S.MFC_RdListStallStat); save(45, 5)
    a.il(12, 7); a.wrch(S.MFC_WrListStallAck, 12)
    a.wait_tag(7, 12)
    # results out
    a.ila(16, 0x13000); a.il(28, 0x200); a.a(29, 3, 28); a.il(11, 0x60)
    a.mfc(S.PUT, 16, 29, 11, 8, 12); a.wait_tag(8, 12)
    a.il(28, 0x260); a.a(29, 3, 28); a.il(11, 0x30)
    a.mfc(S.PUT, 15, 29, 11, 8, 12); a.wait_tag(8, 12)
    a.li32(30, 0x5000); a.wrch(S.SPU_WrOutMbox, 30); a.stop(0x102)
    return a.bytes()


def spu_recv_prog():
    """arg1 = EA of 192 result bytes. Three sys_spu_thread_receive_event
    (spuq in SPU_WrOutMbox, stop 0x110, then status/data1/data2/data3 from the
    inbound mailbox); each value is stored as its own quadword (rdch zeroes
    words 1-3) and the twelve are PUT out. Exits 0x7000."""
    a = S.SpuAsm(SPU_BASE)
    a.rotqbyi(3, 3, 4)
    a.ila(13, 0x10000)
    for k in range(3):
        a.il(10, SPUQ); a.wrch(S.SPU_WrOutMbox, 10); a.stop(0x110)
        for w in range(4):
            a.rdch(20, S.SPU_RdInMbox); a.stqd(20, 13, 16 * (4 * k + w))
    a.il(11, 192)
    a.mfc(S.PUT, 13, 3, 11, 2, 12); a.wait_tag(2, 12)
    a.li32(30, 0x7000); a.wrch(S.SPU_WrOutMbox, 30); a.stop(0x102)
    return a.bytes()


def wrap_spu(code, tmpdir, name, base=SPU_BASE):
    raw = os.path.join(tmpdir, name + ".bin")
    elf = os.path.join(tmpdir, name + ".elf")
    open(raw, "wb").write(code)
    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "wrap_spu_elf.py"), raw,
                    "--entry", hex(base), "--base", hex(base), "--out", elf],
                   check=True, capture_output=True)
    return open(elf, "rb").read()


# ------------------------------------------------------------------ PPU program

FUNCS = ["_start", "hexdump", "puts", "reset_scratch", "load_state", "save_state", "dump_scratch",
         "worker_mutex", "worker_cond", "worker_eflag"]


def _sc(t, num):
    t.emit(P.addi(11, 0, num), P.sc())


def _cmpwi(r, v):
    return P.D(11, 0, r, v)


def ping(t, d, mine, theirs, base):
    """One side of the sys_cond ping-pong: N_PING rounds of lock; wait until
    turn == mine; append base+i to the log; turn = theirs; signal; unlock.
    r20 = round counter, r21 = value. Uses only syscalls (no stack)."""
    tag = "ping%x" % base
    t.emit(P.li32(20, N_PING), P.li32(21, base))
    t.label(tag + "_round")
    t.emit(P.li32(3, d["mtx2_id"]), P.lwz(3, 3, 0), P.addi(4, 0, 0)); _sc(t, SC["mutex_lock"])
    t.label(tag + "_check")
    t.emit(P.li32(9, d["turn"]), P.lwz(9, 9, 0), _cmpwi(9, mine))
    t.bc(tag + "_go", 12, 2)                                  # beq
    t.emit(P.li32(3, d["cv_id"]), P.lwz(3, 3, 0), P.addi(4, 0, 0)); _sc(t, SC["cond_wait"])
    t.b(tag + "_check")
    t.label(tag + "_go")
    t.emit(P.li32(9, d["logpos"]), P.lwz(10, 9, 0), P.li32(8, d["log"]),
           P.X(31, 21, 8, 10, 151),                           # stwx r21,r8,r10
           P.addi(10, 10, 4), P.stw(10, 9, 0),
           P.addi(21, 21, 1),
           P.li32(9, d["turn"]), P.addi(10, 0, theirs), P.stw(10, 9, 0))
    t.emit(P.li32(3, d["cv_id"]), P.lwz(3, 3, 0)); _sc(t, SC["cond_signal"])
    t.emit(P.li32(3, d["mtx2_id"]), P.lwz(3, 3, 0)); _sc(t, SC["mutex_unlock"])
    t.emit(P.addi(20, 20, -1), _cmpwi(20, 0))
    t.bc(tag + "_round", 4, 2)                                # bne


def emit_workers(t, d):
    # worker_mutex(r3 = index): N_MUTEX locked read-yield-write increments,
    # then post the semaphore and exit with 0x50 + index.
    t.label("worker_mutex")
    t.emit(P.addi(14, 3, 0), P.li32(15, N_MUTEX))
    t.label("wm_loop")
    t.emit(P.li32(3, d["mtx_id"]), P.lwz(3, 3, 0), P.addi(4, 0, 0)); _sc(t, SC["mutex_lock"])
    t.emit(P.li32(17, d["counter"]), P.lwz(18, 17, 0))
    _sc(t, SC["thread_yield"])
    t.emit(P.li32(17, d["counter"]), P.addi(18, 18, 1), P.stw(18, 17, 0))
    t.emit(P.li32(3, d["mtx_id"]), P.lwz(3, 3, 0)); _sc(t, SC["mutex_unlock"])
    t.emit(P.addi(15, 15, -1), _cmpwi(15, 0))
    t.bc("wm_loop", 4, 2)
    t.emit(P.li32(3, d["sem_id"]), P.lwz(3, 3, 0), P.addi(4, 0, 1)); _sc(t, SC["sem_post"])
    t.emit(P.addi(3, 14, 0x50)); _sc(t, SC["thread_exit"])
    t.emit(P.b(0))
    # worker_cond: the other side of the ping-pong; exits with 0x60.
    t.label("worker_cond")
    ping(t, d, 1, 0, 0xB00)
    t.emit(P.addi(3, 0, 0x60)); _sc(t, SC["thread_exit"])
    t.emit(P.b(0))
    # worker_eflag: AND-wait for 0x5 with clear; store the result pattern and
    # the return code; exit with 0x70.
    t.label("worker_eflag")
    t.emit(P.li32(3, d["eflag_id"]), P.lwz(3, 3, 0), P.addi(4, 0, 5), P.addi(5, 0, 0x11),
           P.li32(6, d["ef_res"]), P.addi(7, 0, 0)); _sc(t, SC["eflag_wait"])
    t.emit(P.li32(9, d["ef_res"]), P.stw(3, 9, 12))
    t.emit(P.addi(3, 0, 0x70)); _sc(t, SC["thread_exit"])
    t.emit(P.b(0))


class Data:
    def __init__(self):
        self.items, self.off = [], 0

    def take(self, name, size, align=16, init=b""):
        self.off = (self.off + align - 1) & ~(align - 1)
        self.items.append((name, self.off, size, init))
        self.off += size
        return name

    def layout(self, base):
        return {n: base + o for n, o, _, _ in self.items}


def build(out_path):
    tmp = tempfile.mkdtemp(prefix="mcconf")
    imgs = {"dma": wrap_spu(spu_dma_prog(), tmp, "dma"),
            "atomic": wrap_spu(spu_atomic_prog(), tmp, "atomic"),
            "mbox": wrap_spu(spu_mbox_prog(), tmp, "mbox"),
            "event": wrap_spu(spu_event_prog(), tmp, "event"),
            "mfc2": wrap_spu(spu_mfc2_prog(), tmp, "mfc2"),
            "recv": wrap_spu(spu_recv_prog(), tmp, "recv")}
    spu_dir = os.path.splitext(out_path)[0] + "_spu"          # for --lifted
    os.makedirs(spu_dir, exist_ok=True)
    for k, b in imgs.items():
        open(os.path.join(spu_dir, "mc_%s.elf" % k), "wb").write(b)

    D = Data()
    D.take("opd", 8 * len(FUNCS))
    for k in ("hex", "written", "saved_r1", "group_lr", "hdr", "end"):
        D.take(k, 64)
    D.take("line", 2 * 0x400 + 16)
    D.take("pristine", 16); D.take("scratch", 16)
    for k, b in imgs.items():
        D.take("img_" + k, len(b), 128, b)
        D.take("imgs_" + k, 32)                            # sys_spu_image
    D.take("gname", 16, 16, b"conform\0")
    D.take("gattr", 16); D.take("tattr", 16); D.take("targ", 32)
    D.take("ids", 64)                                      # group id, thread ids
    D.take("join", 16)                                     # cause, status
    D.take("exst", 64)                                     # exit statuses
    # DMA test buffers
    src = bytes((i * 7 + (i >> 8) * 13 + 1) & 0xFF for i in range(0x10000))
    D.take("dma_src", 0x10000, 128, src)
    D.take("dma_dst", 0x10000, 128)
    D.take("dma_param", 0x800, 128)
    # atomic line
    D.take("line128", 128, 128)
    # D: event queue
    D.take("eq_attr", 16); D.take("eq_id", 16); D.take("events", 5 * 32)
    # E: PPU threads + sync
    for k in ("mtx_attr", "sem_attr", "cv_attr", "ef_attr"):
        D.take(k, 48)
    for k in ("mtx", "sem", "mtx2", "cv", "eflag"):
        D.take(k + "_id", 16)
    D.take("tparam", 16); D.take("tname", 16, 16, b"conf\0")
    D.take("tids", 64); D.take("tret", 64)
    D.take("counter", 16); D.take("turn", 16); D.take("logpos", 16); D.take("log", 8 * N_PING)
    D.take("ef_res", 16)
    # F: MFC block
    D.take("mfc2", 0x400, 128)
    # G: PPU -> SPU events
    D.take("spuq_attr", 16); D.take("spuq_id", 16); D.take("grpq_id", 16)
    D.take("port_id", 16); D.take("grp_ev", 32); D.take("recv_out", 192, 128)
    HDR = b"MCCONF BEGIN\n"
    END = b"MCCONF END\n"

    def emit(d):
        t = Asm(TEXT_BASE)

        def sc(num, *args):
            for i, v in enumerate(args):
                t.emit(P.li32(3 + i, v & 0x7FFFFFFF) if v >= 0 else [P.addi(3 + i, 0, v)])
            t.emit(P.addi(11, 0, num), P.sc())

        def lwz_arg(reg, addr):
            t.emit(P.li32(reg, addr), P.lwz(reg, reg, 0))

        def puts(addr, n):
            t.emit(P.li32(3, addr), P.addi(4, 0, n)); t.bl("puts")

        def dump(addr, n):
            for o in range(0, n, 0x400):
                t.emit(P.li32(3, addr + o), P.addi(4, 0, min(0x400, n - o))); t.bl("hexdump")

        def group(img, nthreads, args_for, before_start=None):
            """create group, init threads (args_for(i) -> (arg1, arg2)), start."""
            sc(SC["group_create"], d["ids"], nthreads, 100, d["gattr"])
            for i in range(nthreads):
                a1, a2 = args_for(i)
                # sys_spu_thread_argument: arg1/arg2 as u64 (high word 0)
                t.emit(P.li32(5, d["targ"]), P.li32(6, a1), P.stw(6, 5, 4), P.li32(6, a2), P.stw(6, 5, 12))
                lwz_arg(4, d["ids"])
                t.emit(P.li32(3, d["ids"] + 4 + 4 * i), P.addi(5, 0, i), P.li32(6, d["imgs_" + img]),
                       P.li32(7, d["tattr"]), P.li32(8, d["targ"]), P.addi(11, 0, SC["thread_initialize"]), P.sc())
            if before_start:
                before_start()
            lwz_arg(3, d["ids"])
            t.emit(P.addi(11, 0, SC["group_start"]), P.sc())

        def join_and_report(nthreads):
            lwz_arg(3, d["ids"])
            t.emit(P.li32(4, d["join"]), P.li32(5, d["join"] + 4), P.addi(11, 0, SC["group_join"]), P.sc())
            for i in range(nthreads):
                lwz_arg(3, d["ids"] + 4 + 4 * i)
                t.emit(P.li32(4, d["exst"] + 4 * i), P.addi(11, 0, SC["get_exit_status"]), P.sc())
            dump(d["join"], 8)
            dump(d["exst"], 4 * nthreads)

        t.label("_start")
        t.emit(P.li32(30, d["saved_r1"]), P.std(1, 30, 0))
        puts(d["hdr"], len(HDR))
        sc(SC["spu_initialize"], 6, 0)
        for k in imgs:
            sc(SC["image_import"], d["imgs_" + k], d["img_" + k], len(imgs[k]), 0)

        # A: DMA
        group("dma", 1, lambda i: (d["dma_param"], 0))
        join_and_report(1)
        dump(d["dma_dst"], 0x8000 + 8 * 0x40)

        # B: atomics (6 SPUs + this PPU thread)
        group("atomic", 6, lambda i: (d["line128"], i))
        t.emit(P.li32(29, d["line128"]), P.li32(28, N_ATOMIC))
        t.label("ppu_atomic")
        t.emit(P.X(31, 27, 0, 29, 20),                          # lwarx r27,0,r29
               P.addi(27, 27, 1),
               P.X(31, 27, 0, 29, 150, 1))                      # stwcx. r27,0,r29
        t.emit((16 << 26) | (4 << 21) | (2 << 16) | ((-12) & 0xFFFC))  # bne- lwarx (retry)
        t.emit(P.addi(28, 28, -1), P.D(11, 0, 28, 0))           # cmpwi r28,0
        here = t.pc
        t.emit((16 << 26) | (4 << 21) | (2 << 16) | ((t.labels["ppu_atomic"] - here) & 0xFFFC))  # bne loop
        join_and_report(6)
        dump(d["line128"], 16)

        # C: mailbox + signals
        group("mbox", 1, lambda i: (0, 0))
        for v in (0x11, 0x222, 0x3333, 0x44444):
            lwz_arg(3, d["ids"] + 4)
            t.emit(P.li32(4, v), P.addi(11, 0, SC["write_spu_mb"]), P.sc())
        for n, v in ((0, 0x500000), (1, 0x6000000)):
            lwz_arg(3, d["ids"] + 4)
            t.emit(P.addi(4, 0, n), P.li32(5, v), P.addi(11, 0, SC["write_snr"]), P.sc())
        join_and_report(1)

        # D: event ports
        sc(SC["equeue_create"], d["eq_id"], d["eq_attr"], 0, 32)
        def connect():
            lwz_arg(3, d["ids"] + 4); lwz_arg(4, d["eq_id"])
            t.emit(P.addi(5, 0, 1), P.addi(6, 0, SPUP), P.addi(11, 0, SC["connect_event"]), P.sc())
        group("event", 1, lambda i: (0, 0), connect)
        for k in range(5):
            lwz_arg(3, d["eq_id"])
            t.emit(P.li32(4, d["events"] + 32 * k), P.addi(5, 0, 0),
                   P.addi(11, 0, SC["equeue_receive"]), P.sc())
            t.emit(P.li32(9, d["events"] + 32 * k), P.std(4, 9, 0))
            lwz_arg(10, d["ids"] + 4)
            t.emit(P.X(31, 5, 10, 5, 40),                        # r5 -= tid (ids differ by implementation)
                   P.std(5, 9, 8), P.std(6, 9, 16), P.std(7, 9, 24))
        join_and_report(1)
        dump(d["events"], 5 * 32)

        # E: PPU threads + kernel sync
        def create_thread(entry, arg, slot):
            t.emit(P.li32(5, d["tparam"]), P.li32(6, d["opd"] + 8 * FUNCS.index(entry)), P.stw(6, 5, 0))
            sc(SC["thread_create"], d["tids"] + 8 * slot, d["tparam"], arg, 0, 1000, 0x4000, 1, d["tname"])
            lwz_arg(3, d["tids"] + 8 * slot + 4)
            t.emit(P.addi(11, 0, SC["thread_start"]), P.sc())

        def join_thread(slot):
            lwz_arg(3, d["tids"] + 8 * slot + 4)
            t.emit(P.li32(4, d["tret"] + 8 * slot), P.addi(11, 0, SC["thread_join"]), P.sc())

        sc(SC["mutex_create"], d["mtx_id"], d["mtx_attr"])
        sc(SC["sem_create"], d["sem_id"], d["sem_attr"], 0, 4)
        for i in range(4):
            create_thread("worker_mutex", i, i)
        for i in range(4):
            lwz_arg(3, d["sem_id"]); t.emit(P.addi(4, 0, 0), P.addi(11, 0, SC["sem_wait"]), P.sc())
        for i in range(4):
            join_thread(i)
        dump(d["tret"], 32)
        dump(d["counter"], 4)

        sc(SC["mutex_create"], d["mtx2_id"], d["mtx_attr"])
        lwz_arg(4, d["mtx2_id"])
        t.emit(P.li32(3, d["cv_id"]), P.li32(5, d["cv_attr"]), P.addi(11, 0, SC["cond_create"]), P.sc())
        create_thread("worker_cond", 0, 4)
        ping(t, d, 0, 1, 0xA00)
        join_thread(4)
        dump(d["log"], 8 * N_PING)

        sc(SC["eflag_create"], d["eflag_id"], d["ef_attr"], 0)
        create_thread("worker_eflag", 0, 5)
        for bit in (1, 4):
            for _ in range(8):
                t.emit(P.addi(11, 0, SC["thread_yield"]), P.sc())
            lwz_arg(3, d["eflag_id"]); t.emit(P.addi(4, 0, bit), P.addi(11, 0, SC["eflag_set"]), P.sc())
        join_thread(5)
        dump(d["ef_res"], 16)
        dump(d["tret"] + 32, 16)

        # F: more MFC
        group("mfc2", 1, lambda i: (d["mfc2"], 0))
        join_and_report(1)
        dump(d["mfc2"], 0x300)

        # G: PPU -> SPU events
        sc(SC["equeue_create"], d["spuq_id"], d["spuq_attr"], 0, 16)
        sc(SC["equeue_create"], d["grpq_id"], d["eq_attr"], 0, 16)
        sc(SC["port_create"], d["port_id"], 1, 0)
        lwz_arg(3, d["port_id"]); lwz_arg(4, d["spuq_id"])
        t.emit(P.addi(11, 0, SC["port_connect_local"]), P.sc())
        def bind():
            lwz_arg(3, d["ids"]); lwz_arg(4, d["grpq_id"])
            t.emit(P.addi(5, 0, 1), P.addi(11, 0, SC["group_connect_event"]), P.sc())
            lwz_arg(3, d["ids"] + 4); lwz_arg(4, d["spuq_id"])
            t.emit(P.addi(5, 0, SPUQ), P.addi(11, 0, SC["bind_queue"]), P.sc())
        group("recv", 1, lambda i: (d["recv_out"], 0), bind)
        lwz_arg(3, d["grpq_id"])
        t.emit(P.li32(4, d["grp_ev"]), P.addi(5, 0, 0), P.addi(11, 0, SC["equeue_receive"]), P.sc())
        lwz_arg(10, d["ids"])
        t.emit(P.X(31, 5, 10, 5, 40),                            # r5 -= group id
               P.li32(9, d["grp_ev"]), P.std(4, 9, 0), P.std(5, 9, 8), P.std(6, 9, 16), P.std(7, 9, 24))
        for k in range(3):
            lwz_arg(3, d["port_id"])
            t.emit(P.li32(4, 0x100 + k), P.li32(5, 0x2000 + k), P.li32(6, 0x30000 + k),
                   P.addi(11, 0, SC["port_send"]), P.sc())
        join_and_report(1)
        dump(d["grp_ev"], 32)
        dump(d["recv_out"], 192)

        puts(d["end"], len(END))
        t.emit(P.addi(3, 0, 0), P.addi(11, 0, SC["process_exit"]), P.sc(), P.b(0))
        emit_routines(t, d)
        emit_workers(t, d)
        return t

    d = D.layout(0x10000000)
    t = emit(d)
    text = t.bytes()
    data_base = (TEXT_BASE + len(text) + 0xFFFF) & ~0xFFFF
    d = D.layout(data_base)
    t = emit(d)
    text = t.bytes()

    data = bytearray(D.off)
    for n, o, size, init in D.items:
        data[o:o + len(init)] = init
    def put(addr, b):
        data[addr - data_base:addr - data_base + len(b)] = b
    toc = d["opd"] + 0x8000
    put(d["opd"], b"".join(struct.pack(">II", t.labels[f], toc) for f in FUNCS))
    put(d["hex"], b"0123456789abcdef")
    put(d["hdr"], HDR); put(d["end"], END)
    put(d["gattr"], struct.pack(">IIiI", 8, d["gname"], 0, 0))
    put(d["tattr"], struct.pack(">III", d["gname"], 8, 0))
    put(d["eq_attr"], struct.pack(">Ii8s", 1, 1, b"confq"))     # SYS_SYNC_FIFO, SYS_PPU_QUEUE
    put(d["spuq_attr"], struct.pack(">Ii8s", 1, 2, b"confsq"))  # SYS_SYNC_FIFO, SYS_SPU_QUEUE
    # FIFO / not recursive / not process-shared / not adaptive
    put(d["mtx_attr"], struct.pack(">IIIIQiI8s", 1, 0x20, 0x200, 0x2000, 0, 0, 0, b"confm"))
    put(d["sem_attr"], struct.pack(">IIQiI8s", 1, 0x200, 0, 0, 0, b"confs"))
    put(d["cv_attr"], struct.pack(">IiQ8s", 0x200, 0, 0, b"confc"))
    put(d["ef_attr"], struct.pack(">IIQii8s", 1, 0x200, 0, 0, 0x10000, b"confe"))  # SYS_SYNC_WAITER_SINGLE
    mfc2 = d["mfc2"]
    put(mfc2 + 0x80, struct.pack(">IIIIII", 0x10, mfc2 + 0x100, 0x80000010, mfc2 + 0x110, 0x10, mfc2 + 0x120))
    put(mfc2 + 0x100, bytes(range(0x40, 0x70)))
    # DMA param block: src, dst, count, entries; GETL list at +0x400
    ents, ls, dst_off, src_off = [], LS_BUF, 0, 0x100
    for size, al in DMA_CASES:
        so = (src_off + 15) & ~15 | al
        do = (dst_off + 15) & ~15 | al
        lo = (ls + 15) & ~15 | al
        ents.append((size, so, do, lo))
        src_off, dst_off, ls = so + size + 3, do + size + 3, lo + size
    assert dst_off < 0x8000 and ls < 0x20000
    blk = struct.pack(">III", d["dma_src"], d["dma_dst"], len(ents)).ljust(16, b"\0")
    blk += b"".join(struct.pack(">IIII", *e) for e in ents)
    blk = blk.ljust(0x400, b"\0")
    blk += b"".join(struct.pack(">II", 0x40, d["dma_src"] + 0x9000 + 0x300 * i) for i in range(8))
    put(d["dma_param"], blk)
    elf = write_elf(text, data_base, bytes(data), d["opd"], 8 * len(FUNCS))
    open(out_path, "wb").write(elf)
    print("wrote %s (%d DMA cases, %d atomic increments per agent)" % (out_path, len(ents), N_ATOMIC))


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--out", required=True)
    build(ap.parse_args().out)
