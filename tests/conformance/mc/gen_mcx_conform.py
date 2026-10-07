#!/usr/bin/env python3
"""Extended multicore conformance program (suite "mcx").

The mc suite checks that each SPU<->PPU mechanism works once. These cases are
the multithreading corners where a runtime that "works" can still be wrong:
each one drives a mechanism while another agent acts on the same state, and
records only what the architecture defines, so the transcript does not depend
on timing and RPCS3's output is the exact expectation.

  X1 reservation granule. The PPU takes a lwarx/ldarx reservation on line L,
     then an SPU writes -- the same word, the same line elsewhere, the next
     line; by PUT, PUTLLC and PUTLLUC -- and the PPU's stwcx./stdcx. result
     is recorded. On Cell a reservation is the whole 128-byte line.
  X2 inbound mailbox depth. The PPU writes six words before the SPU reads
     any: the return code of each sys_spu_thread_write_spu_mb, the SPU's
     channel count, and the words it then reads.
  X3 signal notification modes. SNR1 in OR mode, SNR2 overwriting: several
     writes each, then the counts and values the SPU reads.
  X4 lock-line-lost event. The SPU reserves a line, enables MFC_LLR_LOST and
     blocks in rdch SPU_RdEventStat; the PPU stores to the line (another word
     of it, then a stwcx. to it); the event status the SPU wakes with, and the
     count after it acknowledges.
  X5 thread-group lifecycle. Thirty create / start / join / destroy rounds of
     four atomically-incrementing SPUs, then ten rounds of terminating a group
     whose SPUs are blocked in a channel read: every join cause and status,
     and the counter.
  X6 mixed-agent atomics on one line. Six SPUs each GETLLAR/PUTLLC their own
     word of a line while the PPU lwarx/stwcx.'s another word of it; every
     word's final count.
  X7 code the SPU writes. A routine copied within local store and run,
     patched and run again, and one DMA'd in from main memory and run.

Records are hexdumps between MCXCONF BEGIN and MCXCONF END. The SPU images are
also written to <out>_spu/ so a runner can lift them (--lifted).
"""
import argparse
import os
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "ppu"))
import ppc_asm as P          # noqa: E402
import spu_asm as S          # noqa: E402
from gen_ppu_conform import Asm, write_elf, emit_routines, TEXT_BASE  # noqa: E402
from gen_mc_conform import wrap_spu, Data  # noqa: E402

SC = dict(process_exit=3, tty_write=403, spu_initialize=169, image_import=157,
          group_create=170, group_destroy=171, thread_initialize=172, group_start=173,
          group_terminate=177, group_join=178, get_exit_status=165,
          write_snr=184, set_spu_cfg=187, write_spu_mb=190, thread_yield=43)

SPU_WrEventMask, SPU_WrEventAck, SPU_RdEventStat = 1, 2, 0
LR_EVENT = 0x400
N_CHURN, N_CHURN_ATOMIC, N_TERM = 30, 50, 10
N_MIX = 300
FUNCS = ["_start", "hexdump", "puts", "reset_scratch", "load_state", "save_state", "dump_scratch"]

# X1 cases: (reservation width, SPU op, target offset from line L)
OP_PUT, OP_PUTLLC, OP_PUTLLUC = 1, 2, 3
X1_CASES = [(32, OP_PUT, 0x00), (32, OP_PUT, 0x40), (32, OP_PUT, 0x80),
            (32, OP_PUTLLC, 0x00), (32, OP_PUTLLUC, 0x00), (32, OP_PUTLLC, 0x80),
            (64, OP_PUT, 0x40), (64, OP_PUTLLC, 0x00), (64, OP_PUT, 0x100)]


# ------------------------------------------------------------------ SPU programs

def _put_flag(a, r_val, r_ea, r_tmp):
    """PUT the quadword whose preferred word is r_val to r_ea (tag 9)."""
    a.ila(16, 0x13F00); a.stqd(r_val, 16, 0); a.il(17, 16)
    a.mfc(S.PUT, 16, r_ea, 17, 9, r_tmp); a.wait_tag(9, r_tmp)


def spu_poke_prog():
    """X1 agent. arg1 = EA of a command line (+0 seq, +4 target EA, +8 op),
    arg2 = EA of its flag quadword. Commands arrive through memory, not a
    syscall, so the PPU can issue one while it holds a reservation. For each
    seq: PUT a 16-byte pattern, or GETLLAR+PUTLLC / PUTLLUC the target line,
    then publish seq in the flag. op 0 ends."""
    a = S.SpuAsm(0)
    a.rotqbyi(3, 3, 4); a.rotqbyi(4, 4, 4)
    a.il(22, 0)                                      # last seq done
    a.ila(10, 0x10000)                               # LS line buffer
    a.li32(23, 0x5A5A5A5A); a.ila(11, 0x11000); a.stqd(23, 11, 0)
    a.ila(14, 0x12000); a.il(15, 16)
    a.label("loop")
    a.ai(24, 22, 1)                                  # seq wanted
    a.label("poll")
    a.mfc(S.GET, 14, 3, 15, 1, 12); a.wait_tag(1, 12)
    a.lqd(25, 14, 0); a.ceq(26, 25, 24); a.brz(26, "poll")
    a.rotqbyi(20, 25, 4)                             # target EA
    a.rotqbyi(21, 25, 8)                             # op
    a.ori(22, 24, 0)
    a.brz(21, "done")
    a.ceqi(26, 21, OP_PUT); a.brnz(26, "put")
    a.ceqi(26, 21, OP_PUTLLC); a.brnz(26, "putllc")
    a.wrch(S.MFC_LSA, 10); a.il(12, 0); a.wrch(S.MFC_EAH, 12); a.wrch(S.MFC_EAL, 20)
    a.il(12, S.PUTLLUC); a.wrch(S.MFC_Cmd, 12); a.rdch(12, S.MFC_RdAtomicStat)
    a.br("next")
    a.label("put")
    a.il(17, 16); a.mfc(S.PUT, 11, 20, 17, 2, 12); a.wait_tag(2, 12)
    a.br("next")
    a.label("putllc")
    a.wrch(S.MFC_LSA, 10); a.il(12, 0); a.wrch(S.MFC_EAH, 12); a.wrch(S.MFC_EAL, 20)
    a.il(12, S.GETLLAR); a.wrch(S.MFC_Cmd, 12); a.rdch(12, S.MFC_RdAtomicStat)
    a.lqd(25, 10, 0); a.il(26, 1); a.a(25, 25, 26); a.stqd(25, 10, 0)
    a.wrch(S.MFC_LSA, 10); a.il(12, 0); a.wrch(S.MFC_EAH, 12); a.wrch(S.MFC_EAL, 20)
    a.il(12, S.PUTLLC); a.wrch(S.MFC_Cmd, 12); a.rdch(13, S.MFC_RdAtomicStat)
    a.brnz(13, "putllc")
    a.label("next")
    _put_flag(a, 22, 4, 12)
    a.br("loop")
    a.label("done")
    a.li32(30, 0x1000); a.wrch(S.SPU_WrOutMbox, 30); a.stop(0x102)
    return a.bytes()


def spu_mbdepth_prog():
    """X2. arg1 = EA of 128 result bytes. Wait for SNR1, then record the
    inbound mailbox count, every word that count promises, and the count after;
    PUT them out as quadwords."""
    a = S.SpuAsm(0)
    a.rotqbyi(3, 3, 4)
    a.ila(13, 0x10000)
    a.rdch(20, S.SPU_RdSigNotify1)
    a.rchcnt(21, S.SPU_RdInMbox); a.stqd(21, 13, 0)
    a.ori(22, 21, 0); a.ila(14, 0x10010)
    a.brz(22, "after")
    a.label("rd")
    a.rdch(23, S.SPU_RdInMbox); a.stqd(23, 14, 0); a.ai(14, 14, 16)
    a.ai(22, 22, -1); a.brnz(22, "rd")
    a.label("after")
    a.rchcnt(21, S.SPU_RdInMbox); a.ila(14, 0x10070); a.stqd(21, 14, 0)
    a.il(11, 0x80); a.mfc(S.PUT, 13, 3, 11, 2, 12); a.wait_tag(2, 12)
    a.li32(30, 0x2000); a.wrch(S.SPU_WrOutMbox, 30); a.stop(0x102)
    return a.bytes()


def spu_snr_prog():
    """X3. arg1 = EA of 64 result bytes. Wait for one inbound mailbox word,
    then record count and value of SNR1 and of SNR2."""
    a = S.SpuAsm(0)
    a.rotqbyi(3, 3, 4)
    a.ila(13, 0x10000)
    a.rdch(20, S.SPU_RdInMbox)
    a.rchcnt(21, S.SPU_RdSigNotify1); a.stqd(21, 13, 0)
    a.rdch(21, S.SPU_RdSigNotify1); a.stqd(21, 13, 16)
    a.rchcnt(21, S.SPU_RdSigNotify2); a.stqd(21, 13, 32)
    a.rdch(21, S.SPU_RdSigNotify2); a.stqd(21, 13, 48)
    a.il(11, 64); a.mfc(S.PUT, 13, 3, 11, 2, 12); a.wait_tag(2, 12)
    a.li32(30, 0x3000); a.wrch(S.SPU_WrOutMbox, 30); a.stop(0x102)
    return a.bytes()


def spu_lrwait_prog():
    """X4. arg1 = EA of line L, arg2 = EA of a flag quadword (+0x80: 64 result
    bytes). Twice: GETLLAR L, enable the lock-line-lost event, publish the
    phase in the flag, block in rdch SPU_RdEventStat; record the status,
    acknowledge it, record the count after."""
    a = S.SpuAsm(0)
    a.rotqbyi(3, 3, 4); a.rotqbyi(4, 4, 4)
    a.ila(10, 0x10000); a.ila(13, 0x12000)
    for phase in (1, 2):
        a.wrch(S.MFC_LSA, 10); a.il(12, 0); a.wrch(S.MFC_EAH, 12); a.wrch(S.MFC_EAL, 3)
        a.il(12, S.GETLLAR); a.wrch(S.MFC_Cmd, 12); a.rdch(12, S.MFC_RdAtomicStat)
        a.il(12, LR_EVENT); a.wrch(SPU_WrEventMask, 12)
        a.il(22, phase); _put_flag(a, 22, 4, 12)
        a.rdch(20, SPU_RdEventStat); a.stqd(20, 13, 32 * (phase - 1))
        a.wrch(SPU_WrEventAck, 20)
        a.rchcnt(21, SPU_RdEventStat); a.stqd(21, 13, 32 * (phase - 1) + 16)
    a.il(12, 0); a.wrch(SPU_WrEventMask, 12)
    a.il(28, 0x80); a.a(29, 4, 28); a.il(11, 64)
    a.mfc(S.PUT, 13, 29, 11, 2, 12); a.wait_tag(2, 12)
    a.li32(30, 0x4000); a.wrch(S.SPU_WrOutMbox, 30); a.stop(0x102)
    return a.bytes()


def spu_atomic_n_prog(n):
    """X5. arg1 = EA of a line: n GETLLAR/PUTLLC increments of its first
    quadword. Exits 0x5000."""
    a = S.SpuAsm(0)
    a.rotqbyi(3, 3, 4)
    a.ila(10, 0x10000); a.il(20, n); a.il(22, 1)
    a.label("again")
    a.wrch(S.MFC_LSA, 10); a.il(12, 0); a.wrch(S.MFC_EAH, 12); a.wrch(S.MFC_EAL, 3)
    a.il(12, S.GETLLAR); a.wrch(S.MFC_Cmd, 12); a.rdch(12, S.MFC_RdAtomicStat)
    a.lqd(21, 10, 0); a.a(21, 21, 22); a.stqd(21, 10, 0)
    a.wrch(S.MFC_LSA, 10); a.il(12, 0); a.wrch(S.MFC_EAH, 12); a.wrch(S.MFC_EAL, 3)
    a.il(12, S.PUTLLC); a.wrch(S.MFC_Cmd, 12); a.rdch(13, S.MFC_RdAtomicStat)
    a.brnz(13, "again")
    a.ai(20, 20, -1); a.brnz(20, "again")
    a.li32(30, 0x5000); a.wrch(S.SPU_WrOutMbox, 30); a.stop(0x102)
    return a.bytes()


def spu_block_prog():
    """X5 terminate case. arg1 = EA of a counter line: atomically add 1 to its
    first word (the PPU waits for every SPU to have done so), then block in
    rdch SPU_RdInMbox forever."""
    a = S.SpuAsm(0)
    a.rotqbyi(3, 3, 4)
    a.ila(10, 0x10000); a.il(22, 1)
    a.label("again")
    a.wrch(S.MFC_LSA, 10); a.il(12, 0); a.wrch(S.MFC_EAH, 12); a.wrch(S.MFC_EAL, 3)
    a.il(12, S.GETLLAR); a.wrch(S.MFC_Cmd, 12); a.rdch(12, S.MFC_RdAtomicStat)
    a.lqd(21, 10, 0); a.a(21, 21, 22); a.stqd(21, 10, 0)
    a.wrch(S.MFC_LSA, 10); a.il(12, 0); a.wrch(S.MFC_EAH, 12); a.wrch(S.MFC_EAL, 3)
    a.il(12, S.PUTLLC); a.wrch(S.MFC_Cmd, 12); a.rdch(13, S.MFC_RdAtomicStat)
    a.brnz(13, "again")
    a.label("top")
    a.rdch(20, S.SPU_RdInMbox)
    a.br("top")
    return a.bytes()


def spu_atomw_prog(word):
    """X6. arg1 = EA of a line: N_MIX GETLLAR/PUTLLC increments of word `word`
    only. Exits 0x6000 | word."""
    a = S.SpuAsm(0)
    a.rotqbyi(3, 3, 4)
    q, slot = divmod(word, 4)
    a.ila(10, 0x10000); a.il(20, N_MIX)
    a.il(22, 1); a.fsmbi(23, 0xF000 >> (4 * slot)); a.and_(22, 22, 23)   # 1 in slot `slot` only
    a.label("again")
    a.wrch(S.MFC_LSA, 10); a.il(12, 0); a.wrch(S.MFC_EAH, 12); a.wrch(S.MFC_EAL, 3)
    a.il(12, S.GETLLAR); a.wrch(S.MFC_Cmd, 12); a.rdch(12, S.MFC_RdAtomicStat)
    a.lqd(21, 10, 16 * q); a.a(21, 21, 22); a.stqd(21, 10, 16 * q)
    a.wrch(S.MFC_LSA, 10); a.il(12, 0); a.wrch(S.MFC_EAH, 12); a.wrch(S.MFC_EAL, 3)
    a.il(12, S.PUTLLC); a.wrch(S.MFC_Cmd, 12); a.rdch(13, S.MFC_RdAtomicStat)
    a.brnz(13, "again")
    a.ai(20, 20, -1); a.brnz(20, "again")
    a.li32(30, 0x6000 | word); a.wrch(S.SPU_WrOutMbox, 30); a.stop(0x102)
    return a.bytes()


def spu_smc_prog():
    """X7. arg1 = EA of 16 bytes of SPU code (ai $3,$3,100; bi $0; nop; nop),
    arg2 = EA of 64 result bytes. Copy the routine at `tmpl` to LS 0x20000 and
    call it, patch its immediate and call it again, GET the main-memory
    routine to LS 0x21000 and call it; record r3 after each call."""
    a = S.SpuAsm(0)
    a.rotqbyi(3, 3, 4); a.rotqbyi(4, 4, 4)
    a.ori(40, 3, 0); a.ori(41, 4, 0)
    a.ila(13, 0x12000)
    a.br("main")
    while a.pc % 16:
        a.nop()
    a.label("tmpl")
    a.ai(3, 3, 5); a.bi(0); a.nop(); a.nop()
    a.label("main")
    a.lqa(20, a.labels["tmpl"]); a.ila(21, 0x20000); a.stqd(20, 21, 0)
    a.il(3, 1); a.bisl(0, 21); a.stqd(3, 13, 0)                 # 1 + 5
    a.lqd(20, 21, 0); a.ilhu(22, 2 << 14 >> 16); a.iohl(22, (2 << 14) & 0xFFFF)
    a.fsmbi(23, 0xF000); a.and_(22, 22, 23); a.a(20, 20, 22); a.stqd(20, 21, 0)
    a.il(3, 1); a.bisl(0, 21); a.stqd(3, 13, 16)                # 1 + 7
    a.ila(24, 0x21000); a.il(11, 16)
    a.mfc(S.GET, 24, 40, 11, 3, 12); a.wait_tag(3, 12)
    a.il(3, 1); a.bisl(0, 24); a.stqd(3, 13, 32)                # 1 + 100
    a.il(11, 48); a.mfc(S.PUT, 13, 41, 11, 2, 12); a.wait_tag(2, 12)
    a.li32(30, 0x7000); a.wrch(S.SPU_WrOutMbox, 30); a.stop(0x102)
    return a.bytes()


def smc_routine():
    a = S.SpuAsm(0x21000)
    a.ai(3, 3, 100); a.bi(0); a.nop(); a.nop()
    return a.bytes()


# ------------------------------------------------------------------ PPU program

def lwarx(rt, rb):  return P.X(31, rt, 0, rb, 20)
def ldarx(rt, rb):  return P.X(31, rt, 0, rb, 84)
def stwcx(rs, rb):  return P.X(31, rs, 0, rb, 150, 1)
def stdcx(rs, rb):  return P.X(31, rs, 0, rb, 214, 1)
def mfcr(rt):       return P.X(31, rt, 0, 0, 19)
def cr0(rt):        return (21 << 26) | (rt << 21) | (rt << 16) | (4 << 11) | (28 << 6) | (31 << 1)  # rlwinm rt,rt,4,28,31


def x1_spec(line_base):
    """X1 by the Cell Broadband Engine Architecture rather than by RPCS3: a
    lwarx/ldarx reservation is the whole 128-byte granule, and any store to the
    granule by another agent -- DMA PUT, PUTLLC, PUTLLUC -- clears it. RPCS3
    keys its stwcx./stdcx. on the reserved doubleword's value and a per-line
    timestamp, and reports PUTs into the granule (and next to it)
    inconsistently, so it is not the oracle for these records. Returns the
    expected CR0 nibbles and the expected 0x180 bytes at line_base."""
    mem = bytearray(0x180)
    ls_line = bytearray(128)
    pattern = bytes.fromhex("5a5a5a5a" * 4)
    crs = []
    for width, op, off in X1_CASES:
        n = width // 8
        resv = bytes(mem[0:n])                                   # lwarx/ldarx at L
        if op == OP_PUT:
            mem[off:off + 16] = pattern
        elif op == OP_PUTLLC:
            ls_line[:] = mem[off:off + 128]
            for w in range(4):
                v = (int.from_bytes(ls_line[4 * w:4 * w + 4], "big") + 1) & 0xFFFFFFFF
                ls_line[4 * w:4 * w + 4] = v.to_bytes(4, "big")
            mem[off:off + 128] = ls_line
        else:
            mem[off:off + 128] = ls_line
        ok = off >= 128                                          # granule untouched
        if ok:
            v = (int.from_bytes(resv, "big") + 1) & ((1 << (8 * n)) - 1)
            mem[0:n] = v.to_bytes(n, "big")
        crs.append(0x2 if ok else 0x0)
    return crs, bytes(mem)


def build(out_path):
    progs = {"poke": spu_poke_prog(), "mbdepth": spu_mbdepth_prog(), "snr": spu_snr_prog(),
             "lrwait": spu_lrwait_prog(), "churn": spu_atomic_n_prog(N_CHURN_ATOMIC),
             "block": spu_block_prog(), "smc": spu_smc_prog()}
    for w in range(6):
        progs["atomw%d" % w] = spu_atomw_prog(w)
    spu_dir = os.path.splitext(out_path)[0] + "_spu"
    os.makedirs(spu_dir, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix="mcxconf")
    imgs = {}
    for k, code in progs.items():
        imgs[k] = wrap_spu(code, tmp, k)
        open(os.path.join(spu_dir, "mcx_%s.elf" % k), "wb").write(imgs[k])

    D = Data()
    D.take("opd", 8 * len(FUNCS))
    for k in ("hex", "written", "saved_r1", "group_lr", "hdr", "end"):
        D.take(k, 64)
    D.take("line", 2 * 0x400 + 16)
    D.take("pristine", 16); D.take("scratch", 16)
    for k, b in imgs.items():
        D.take("img_" + k, len(b), 128, b)
        D.take("imgs_" + k, 32)
    D.take("gname", 16, 16, b"mcxconf\0")
    D.take("gattr", 16); D.take("tattr", 16); D.take("targ", 32)
    D.take("ids", 64); D.take("join", 16); D.take("exst", 64)
    D.take("rc", 64)
    D.take("x1_line", 0x200, 128)            # L, L+0x80, L+0x100
    D.take("x1_flag", 128, 128); D.take("x1_cmd", 128, 128)
    D.take("x1_res", 16 * len(X1_CASES), 16)
    D.take("x2_res", 128, 128); D.take("x2_rc", 32, 16)
    D.take("x3_res", 64, 128)
    D.take("x4_line", 128, 128); D.take("x4_flag", 0x100, 128)
    D.take("x5_line", 128, 128); D.take("x5_log", 8 * (N_CHURN + N_TERM), 16)
    D.take("x5_started", 128, 128); D.take("x5_trc", 4 * N_TERM, 16); D.take("x5_drc", 4 * N_TERM, 16)
    D.take("x6_line", 128, 128)
    D.take("x7_code", 16, 16, smc_routine()); D.take("x7_res", 64, 128)
    HDR, END = b"MCXCONF BEGIN\n", b"MCXCONF END\n"

    def emit(d):
        t = Asm(TEXT_BASE)
        uid = [0]

        def L(stem):
            uid[0] += 1
            return "%s_%d" % (stem, uid[0])

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

        def spin_until(addr, val):
            """Plain-load spin until the word at addr == val (no syscalls, so a
            reservation the caller holds is not disturbed)."""
            lab = L("spin")
            t.label(lab)
            t.emit(P.li32(9, addr), P.lwz(10, 9, 0), P.D(11, 0, 10, val))
            t.bc(lab, 4, 2)

        def group(img, nthreads, args_for, before_start=None):
            sc(SC["group_create"], d["ids"], nthreads, 100, d["gattr"])
            for i in range(nthreads):
                a1, a2 = args_for(i)
                t.emit(P.li32(5, d["targ"]), P.li32(6, a1), P.stw(6, 5, 4), P.li32(6, a2), P.stw(6, 5, 12))
                lwz_arg(4, d["ids"])
                im = img(i) if callable(img) else img
                t.emit(P.li32(3, d["ids"] + 4 + 4 * i), P.addi(5, 0, i), P.li32(6, d["imgs_" + im]),
                       P.li32(7, d["tattr"]), P.li32(8, d["targ"]), P.addi(11, 0, SC["thread_initialize"]), P.sc())
            if before_start:
                before_start()
            lwz_arg(3, d["ids"])
            t.emit(P.addi(11, 0, SC["group_start"]), P.sc())

        def join(nthreads, report=True):
            lwz_arg(3, d["ids"])
            t.emit(P.li32(4, d["join"]), P.li32(5, d["join"] + 4), P.addi(11, 0, SC["group_join"]), P.sc())
            for i in range(nthreads):
                lwz_arg(3, d["ids"] + 4 + 4 * i)
                t.emit(P.li32(4, d["exst"] + 4 * i), P.addi(11, 0, SC["get_exit_status"]), P.sc())
            if report:
                dump(d["join"], 8)
                dump(d["exst"], 4 * nthreads)

        def destroy():
            lwz_arg(3, d["ids"]); t.emit(P.addi(11, 0, SC["group_destroy"]), P.sc())

        def write_snr(slot, num, val):
            lwz_arg(3, d["ids"] + 4 + 4 * slot)
            t.emit(P.addi(4, 0, num), P.li32(5, val), P.addi(11, 0, SC["write_snr"]), P.sc())

        t.label("_start")
        t.emit(P.li32(30, d["saved_r1"]), P.std(1, 30, 0))
        puts(d["hdr"], len(HDR))
        sc(SC["spu_initialize"], 6, 0)
        for k in imgs:
            sc(SC["image_import"], d["imgs_" + k], d["img_" + k], len(imgs[k]), 0)

        # X1: reservation granule
        def command(seq, ea, op):
            """target and op first, then seq (lwsync between) on the command line."""
            t.emit(P.li32(9, d["x1_cmd"]), P.li32(10, ea), P.stw(10, 9, 4),
                   P.li32(10, op), P.stw(10, 9, 8), 0x7C2004AC,      # lwsync
                   P.li32(10, seq), P.stw(10, 9, 0))
        group("poke", 1, lambda i: (d["x1_cmd"], d["x1_flag"]))
        Lb = d["x1_line"]
        for k, (width, op, off) in enumerate(X1_CASES):
            t.emit(P.li32(29, Lb))
            t.emit(lwarx(27, 29) if width == 32 else ldarx(27, 29))
            command(k + 1, Lb + off, op)
            spin_until(d["x1_flag"], k + 1)
            t.emit(P.li32(29, Lb), P.addi(27, 27, 1))
            t.emit(stwcx(27, 29) if width == 32 else stdcx(27, 29))
            t.emit(mfcr(26), cr0(26), P.li32(25, d["x1_res"] + 16 * k), P.stw(26, 25, 0))
        command(len(X1_CASES) + 1, 0, 0)
        join(1)
        dump(d["x1_res"], 16 * len(X1_CASES))
        dump(d["x1_line"], 0x180)

        # X2: inbound mailbox depth
        group("mbdepth", 1, lambda i: (d["x2_res"], 0))
        for i in range(6):
            lwz_arg(3, d["ids"] + 4)
            t.emit(P.li32(4, 0x100 + i), P.addi(11, 0, SC["write_spu_mb"]), P.sc())
            t.emit(P.li32(9, d["x2_rc"] + 4 * i), P.stw(3, 9, 0))
        write_snr(0, 0, 1)
        join(1)
        dump(d["x2_rc"], 24)
        dump(d["x2_res"], 128)

        # X3: signal notification modes (SNR1 OR, SNR2 overwrite)
        def cfg():
            lwz_arg(3, d["ids"] + 4)
            t.emit(P.addi(4, 0, 1), P.addi(11, 0, SC["set_spu_cfg"]), P.sc())
        group("snr", 1, lambda i: (d["x3_res"], 0), cfg)
        for v in (0x1, 0x2, 0x10):
            write_snr(0, 0, v)
        for v in (0x100, 0x200):
            write_snr(0, 1, v)
        lwz_arg(3, d["ids"] + 4)
        t.emit(P.li32(4, 0x99), P.addi(11, 0, SC["write_spu_mb"]), P.sc())
        join(1)
        dump(d["x3_res"], 64)

        # X4: lock-line-lost event
        group("lrwait", 1, lambda i: (d["x4_line"], d["x4_flag"]))
        spin_until(d["x4_flag"], 1)
        t.emit(P.li32(9, d["x4_line"]), P.li32(10, 0x11111111), P.stw(10, 9, 0x40))
        spin_until(d["x4_flag"], 2)
        t.emit(P.li32(29, d["x4_line"]))
        lab = L("x4cas")
        t.label(lab)
        t.emit(lwarx(27, 29), P.addi(27, 27, 1), stwcx(27, 29))
        t.bc(lab, 4, 2)
        join(1)
        dump(d["x4_flag"] + 0x80, 64)
        dump(d["x4_line"], 128)

        # X5: group lifecycle -- churn, then terminate-while-blocked
        for r in range(N_CHURN):
            group("churn", 4, lambda i: (d["x5_line"], 0))
            join(4, report=False)
            t.emit(P.li32(9, d["join"]), P.lwz(10, 9, 0), P.lwz(11, 9, 4),
                   P.li32(9, d["x5_log"] + 8 * r), P.stw(10, 9, 0), P.stw(11, 9, 4))
            destroy()
        for r in range(N_TERM):
            group("block", 2, lambda i: (d["x5_started"], 0))
            spin_until(d["x5_started"], 2 * (r + 1))           # both SPUs are in rdch
            lwz_arg(3, d["ids"]); t.emit(P.li32(4, 0x55 + r), P.addi(11, 0, SC["group_terminate"]), P.sc())
            t.emit(P.li32(9, d["x5_trc"] + 4 * r), P.stw(3, 9, 0))
            join(2, report=False)
            t.emit(P.li32(9, d["join"]), P.lwz(10, 9, 0), P.lwz(11, 9, 4),
                   P.li32(9, d["x5_log"] + 8 * (N_CHURN + r)), P.stw(10, 9, 0), P.stw(11, 9, 4))
            destroy()
            t.emit(P.li32(9, d["x5_drc"] + 4 * r), P.stw(3, 9, 0))
        dump(d["x5_log"], 8 * (N_CHURN + N_TERM))
        dump(d["x5_trc"], 4 * N_TERM)
        dump(d["x5_drc"], 4 * N_TERM)
        dump(d["x5_line"], 16)

        # X6: six SPUs on words 0-5, the PPU on word 7 of the same line
        group(lambda i: "atomw%d" % i, 6, lambda i: (d["x6_line"], 0))
        t.emit(P.li32(29, d["x6_line"] + 28), P.li32(28, N_MIX))
        lab = L("x6loop"); retry = L("x6retry")
        t.label(lab); t.label(retry)
        t.emit(lwarx(27, 29), P.addi(27, 27, 1), stwcx(27, 29))
        t.bc(retry, 4, 2)
        t.emit(P.addi(28, 28, -1), P.D(11, 0, 28, 0))
        t.bc(lab, 4, 2)
        join(6)
        dump(d["x6_line"], 32)

        # X7: SPU-written code
        group("smc", 1, lambda i: (d["x7_code"], d["x7_res"]))
        join(1)
        dump(d["x7_res"], 48)

        puts(d["end"], len(END))
        t.emit(P.addi(3, 0, 0), P.addi(11, 0, SC["process_exit"]), P.sc(), P.b(0))
        emit_routines(t, d)
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
    elf = write_elf(text, data_base, bytes(data), d["opd"], 8 * len(FUNCS))
    open(out_path, "wb").write(elf)
    # Records the architecture decides rather than the oracle (see x1_spec):
    # transcript line index -> expected hexdump.
    crs, mem = x1_spec(d["x1_line"])
    spec = {2: "".join("%08x" % c + "00" * 12 for c in crs), 3: mem.hex()}
    import json
    json.dump({"by_spec": spec, "why": "X1: CBEA reservation granule (RPCS3 is value-based)"},
              open(os.path.splitext(out_path)[0] + ".spec.json", "w"), indent=1)
    print("wrote %s (%d SPU images in %s)" % (out_path, len(imgs), spu_dir))


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--out", required=True)
    build(ap.parse_args().out)
