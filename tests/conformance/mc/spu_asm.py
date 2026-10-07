"""Minimal SPU encoder (Cell BE SPU ISA instruction formats) for the multicore
conformance programs. Labels resolve in a second pass; branch offsets are in
words. Only the instructions the programs use are here."""
import struct

# channels
SPU_RdSigNotify1, SPU_RdSigNotify2 = 3, 4
MFC_LSA, MFC_EAH, MFC_EAL, MFC_Size, MFC_TagID, MFC_Cmd = 16, 17, 18, 19, 20, 21
MFC_WrTagMask, MFC_WrTagUpdate, MFC_RdTagStat = 22, 23, 24
MFC_RdListStallStat, MFC_WrListStallAck, MFC_RdAtomicStat = 25, 26, 27
SPU_WrOutMbox, SPU_RdInMbox, SPU_WrOutIntrMbox = 28, 29, 30
# MFC commands
PUT, PUTB, PUTF, GET, GETB, GETF = 0x20, 0x21, 0x22, 0x40, 0x41, 0x42
PUTL, GETL, GETLLAR, PUTLLC, PUTLLUC = 0x24, 0x44, 0xD0, 0xB4, 0xB0


class SpuAsm:
    def __init__(self, base=0):
        self.base, self.w, self.labels, self.fix = base, [], {}, []

    @property
    def pc(self):
        return self.base + 4 * len(self.w)

    def label(self, n):
        self.labels[n] = self.pc

    def _e(self, w):
        self.w.append(w & 0xFFFFFFFF)

    # formats
    def rr(self, op11, rt, ra, rb):    self._e(op11 << 21 | (rb & 127) << 14 | (ra & 127) << 7 | (rt & 127))
    def rrr(self, op4, rt, ra, rb, rc): self._e(op4 << 28 | (rt & 127) << 21 | (rb & 127) << 14 | (ra & 127) << 7 | (rc & 127))
    def ri7(self, op11, rt, ra, i7):   self._e(op11 << 21 | (i7 & 127) << 14 | (ra & 127) << 7 | (rt & 127))
    def ri10(self, op8, rt, ra, i10):  self._e(op8 << 24 | (i10 & 0x3FF) << 14 | (ra & 127) << 7 | (rt & 127))
    def ri16(self, op9, rt, i16):      self._e(op9 << 23 | (i16 & 0xFFFF) << 7 | (rt & 127))
    def ri18(self, op7, rt, i18):      self._e(op7 << 25 | (i18 & 0x3FFFF) << 7 | (rt & 127))

    # constants
    def il(self, rt, i16):   self.ri16(0x081, rt, i16)
    def fsmbi(self, rt, i16): self.ri16(0x065, rt, i16)
    def ilhu(self, rt, i16): self.ri16(0x082, rt, i16)
    def iohl(self, rt, i16): self.ri16(0x0C1, rt, i16)
    def ila(self, rt, i18):  self.ri18(0x21, rt, i18)

    def li32(self, rt, v):
        """rt = v (32-bit) in every word slot."""
        v &= 0xFFFFFFFF
        self.ilhu(rt, v >> 16)
        self.iohl(rt, v & 0xFFFF)

    # arithmetic / logic
    def a(self, rt, ra, rb):   self.rr(0x0C0, rt, ra, rb)
    def sf(self, rt, ra, rb):  self.rr(0x040, rt, ra, rb)
    def ai(self, rt, ra, i):   self.ri10(0x1C, rt, ra, i)
    def and_(self, rt, ra, rb): self.rr(0x0C1, rt, ra, rb)
    def or_(self, rt, ra, rb): self.rr(0x041, rt, ra, rb)
    def ori(self, rt, ra, i):  self.ri10(0x04, rt, ra, i)
    def xor(self, rt, ra, rb): self.rr(0x241, rt, ra, rb)
    def ceq(self, rt, ra, rb): self.rr(0x3C0, rt, ra, rb)
    def clgt(self, rt, ra, rb): self.rr(0x2C0, rt, ra, rb)
    def cgt(self, rt, ra, rb): self.rr(0x240, rt, ra, rb)
    def selb(self, rt, ra, rb, rc): self.rrr(0x8, rt, ra, rb, rc)
    def sfi(self, rt, ra, i): self.ri10(0x0C, rt, ra, i)
    def ceqi(self, rt, ra, i): self.ri10(0x7C, rt, ra, i)
    def cgti(self, rt, ra, i): self.ri10(0x4C, rt, ra, i)
    def clgti(self, rt, ra, i): self.ri10(0x5C, rt, ra, i)
    def shli(self, rt, ra, i): self.ri7(0x07B, rt, ra, i)
    def rotmi(self, rt, ra, n): self.ri7(0x079, rt, ra, -n)     # logical shift right by n
    def andi(self, rt, ra, i): self.ri10(0x14, rt, ra, i)

    # quadword shuffles
    def rotqbyi(self, rt, ra, i): self.ri7(0x1FC, rt, ra, i)
    def shlqbyi(self, rt, ra, i): self.ri7(0x1FF, rt, ra, i)
    def rotqby(self, rt, ra, rb): self.rr(0x1DC, rt, ra, rb)

    # loads / stores
    def lqd(self, rt, ra, d):  self.ri10(0x34, rt, ra, d >> 4)
    def stqd(self, rt, ra, d): self.ri10(0x24, rt, ra, d >> 4)
    def lqa(self, rt, addr):   self.ri16(0x061, rt, addr >> 2)
    def stqa(self, rt, addr):  self.ri16(0x041, rt, addr >> 2)
    def lqx(self, rt, ra, rb): self.rr(0x1C4, rt, ra, rb)
    def stqx(self, rt, ra, rb): self.rr(0x144, rt, ra, rb)

    # channels
    def rdch(self, rt, ch):   self.rr(0x00D, rt, ch, 0)
    def rchcnt(self, rt, ch): self.rr(0x00F, rt, ch, 0)
    def wrch(self, ch, rt):   self.rr(0x10D, rt, ch, 0)

    # control
    def stop(self, code): self._e(code & 0x3FFF)
    def nop(self):        self._e(0x40200000)

    def _br(self, op9, rt, lab):
        self.fix.append((len(self.w), op9, rt, lab))
        self._e(0)

    def br(self, lab):         self._br(0x064, 0, lab)
    def brz(self, rt, lab):    self._br(0x040, rt, lab)
    def brnz(self, rt, lab):   self._br(0x042, rt, lab)
    def brsl(self, rt, lab):   self._br(0x066, rt, lab)
    def bi(self, ra):          self.rr(0x1A8, 0, ra, 0)
    def bisl(self, rt, ra):    self.rr(0x1A9, rt, ra, 0)

    def bytes(self):
        for i, op9, rt, lab in self.fix:
            off = (self.labels[lab] - (self.base + 4 * i)) >> 2
            self.w[i] = op9 << 23 | (off & 0xFFFF) << 7 | (rt & 127)
        return b"".join(struct.pack(">I", x) for x in self.w)

    # ---- MFC helpers (each issues one command and waits for its tag) ----
    def mfc(self, cmd, r_lsa, r_eal, r_size, tag, r_tmp):
        """Issue an MFC command; LSA/EAL/size come from registers (preferred slot)."""
        self.wrch(MFC_LSA, r_lsa)
        self.il(r_tmp, 0)
        self.wrch(MFC_EAH, r_tmp)
        self.wrch(MFC_EAL, r_eal)
        self.wrch(MFC_Size, r_size)
        self.il(r_tmp, tag)
        self.wrch(MFC_TagID, r_tmp)
        self.il(r_tmp, cmd)
        self.wrch(MFC_Cmd, r_tmp)

    def wait_tag(self, tag, r_tmp):
        self.il(r_tmp, 1 << tag)
        self.wrch(MFC_WrTagMask, r_tmp)
        self.il(r_tmp, 2)                      # MFC_TAG_UPDATE_ALL
        self.wrch(MFC_WrTagUpdate, r_tmp)
        self.rdch(r_tmp, MFC_RdTagStat)
