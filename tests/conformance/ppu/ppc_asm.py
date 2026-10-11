"""Tiny PowerPC encoder for the conformance generator: just enough forms to
write the harness code (loads/stores, syscalls, branches). The instructions
UNDER TEST are encoded from the spec table in ppu_isa.py, not here."""
import struct


def D(op, rt, ra, d):   return (op << 26) | (rt << 21) | (ra << 16) | (d & 0xFFFF)
def DS(op, rt, ra, ds, xo): return (op << 26) | (rt << 21) | (ra << 16) | (ds & 0xFFFC) | xo
def X(op, rt, ra, rb, xo, rc=0): return (op << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (xo << 1) | rc


def addi(rt, ra, si):  return D(14, rt, ra, si)
def addis(rt, ra, si): return D(15, rt, ra, si)
def ori(ra, rs, ui):   return D(24, rs, ra, ui)
def oris(ra, rs, ui):  return D(25, rs, ra, ui)
def ld(rt, ra, ds):    return DS(58, rt, ra, ds, 0)
def std(rs, ra, ds):   return DS(62, rs, ra, ds, 0)
def stdu(rs, ra, ds):   return DS(62, rs, ra, ds, 1)
def lwz(rt, ra, d):    return D(32, rt, ra, d)
def stw(rs, ra, d):    return D(36, rs, ra, d)
def sth(rs, ra, d):    return D(44, rs, ra, d)
def lfd(frt, ra, d):   return D(50, frt, ra, d)
def stfd(frs, ra, d):  return D(54, frs, ra, d)
def lvx(vrt, ra, rb):  return X(31, vrt, ra, rb, 103)
def stvx(vrs, ra, rb): return X(31, vrs, ra, rb, 231)
def mtcrf(rs, fxm=0xFF): return (31 << 26) | (rs << 21) | (fxm << 12) | (144 << 1)
def mfcr(rt):          return X(31, rt, 0, 0, 19)
def mtspr(spr, rs):    return (31 << 26) | (rs << 21) | (((spr & 0x1F) << 5 | (spr >> 5)) << 11) | (467 << 1)
def mfspr(rt, spr):    return (31 << 26) | (rt << 21) | (((spr & 0x1F) << 5 | (spr >> 5)) << 11) | (339 << 1)
def mtfsf(frb, flm=0xFF): return (63 << 26) | (flm << 17) | (frb << 11) | (711 << 1)
def mffs(frt):         return X(63, frt, 0, 0, 583)
def mtvscr(vrb):       return (4 << 26) | (vrb << 11) | 1604
def mfvscr(vrt):       return (4 << 26) | (vrt << 21) | 1540
def sc():              return 0x44000002
def b(off, lk=0):      return (18 << 26) | (off & 0x3FFFFFC) | lk
def blr():             return 0x4E800020
def nop():             return 0x60000000


def li32(rt, v):
    """rt = v for 0 <= v < 0x80000000 (addis sign-extends bit 31)."""
    assert 0 <= v < 0x80000000
    return [addis(rt, 0, (v >> 16) & 0xFFFF), ori(rt, rt, v & 0xFFFF)]


def words(ws): return b"".join(struct.pack(">I", w & 0xFFFFFFFF) for w in ws)
