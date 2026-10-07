"""Framework for the SPURS conformance tests.

A test module defines `body(T)`, which describes a PPU program through the
helpers on T: firmware calls (imports collected automatically), raw lv2
syscalls, PPU threads, SPU binaries (policy modules, tasks) and *records*.
Every record is a labelled snapshot of guest memory or of a call's return code,
taken at that point of the program; the program prints them all at the end, one
line each, and `run_spurs_conform.py` compares the lines against RPCS3 running
the firmware's real libsre (LLE) and reports mismatches by label.

The program is assembled in two passes: the first collects imports and data,
the second emits the final code (data lives at a fixed base, so addresses are
known in both passes; text labels resolve through Asm fixups).
"""
import os
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "ppu"))
sys.path.insert(0, os.path.join(HERE, "..", "mc"))
import ppc_asm as P                    # noqa: E402
import spu_asm as S                    # noqa: E402
from gen_ppu_conform import Asm, write_elf, emit_routines, TEXT_BASE  # noqa: E402
from gen_mc_conform import wrap_spu    # noqa: E402
from lv2_imports import Imports, SDK_VERSION  # noqa: E402

DATA_BASE = 0x00100000


def rldicl(ra, rs, sh, mb):
    return (30 << 26) | (rs << 21) | (ra << 16) | ((sh & 31) << 11) | ((mb & 31) << 6) | \
           ((mb >> 5) << 5) | (0 << 2) | ((sh >> 5) << 1)


def rldicr(ra, rs, sh, me):
    return (30 << 26) | (rs << 21) | (ra << 16) | ((sh & 31) << 11) | ((me & 31) << 6) | \
           ((me >> 5) << 5) | (1 << 2) | ((sh >> 5) << 1)
TAG = b"SPURSTEST"

# lv2 syscalls used by the harness
SYS = dict(process_exit=3, usleep=141, thread_create=52, thread_start=53, thread_join=44,
           thread_exit=41, thread_yield=43, equeue_create=128, equeue_receive=130,
           equeue_destroy=129, tty_write=403, spu_group_create=170, spu_group_destroy=171,
           spu_thread_initialize=172, spu_group_start=173, spu_group_join=178,
           spu_get_exit_status=165)

# common error codes, for readable expectations in comments
CELL_OK = 0


def module_of(fn):
    if fn.startswith("cellSysmodule"):
        return "cellSysmodule"
    if fn.startswith("cellUserTrace"):
        return "cellLibprof"
    if "Spurs" in fn:
        return "cellSpurs"
    if "cellSync" in fn:
        return "cellSync"
    return "sysPrxForUser"


class Data:
    def __init__(self, base):
        self.base, self.items, self.off, self.addr, self.sizes = base, [], 0, {}, {}

    def take(self, name, size, align=16, init=b""):
        if name in self.addr:
            assert (size, align) == self.sizes[name], "alloc name reused: " + name
            return self.addr[name]
        self.off = (self.off + align - 1) & ~(align - 1)
        self.items.append((name, self.off, size, bytes(init)))
        self.addr[name] = self.base + self.off
        self.sizes[name] = (size, align)
        self.off += max(size, 1)
        return self.addr[name]

    def __getitem__(self, name):
        return self.addr[name]


class Test:
    """The builder a test body talks to."""

    def __init__(self, name, imports=None):
        self.name = name
        self.funcs = ["_start", "hexdump", "puts", "reset_scratch", "load_state", "save_state",
                      "dump_scratch"]
        self.called = []
        self.records = []          # (label, res_offset, size)
        self.res_off = 0
        self.threads = []          # (label, emit_fn)
        self.spu = {}              # name -> bytes
        self._uniq = 0
        self.imports = imports
        self.D = Data(DATA_BASE)
        if imports:
            imports.take(self.D)   # first: the tables must be in the low 32 KB
        for k in ("opd",):
            self.D.take(k, 8 * 64, 8)
        for k in ("hex", "written", "saved_r1", "hdr", "end"):
            self.D.take(k, 64)
        self.D.take("line", 2 * 0x400 + 16)
        self.D.take("pristine", 16); self.D.take("scratch", 16)
        self.D.take("res", 0x10000, 16)
        self.D.take("tparam", 16)
        self.D.take("tname", 16, 16, b"spurst\0")
        self.D.take("at", 16, 16, b"@")
        self.t = Asm(TEXT_BASE)

    # ---- data -----------------------------------------------------------
    def alloc(self, name, size, align=16, init=b""):
        return self.D.take(name, size, align, init)

    def __getitem__(self, name):
        return self.D[name]

    def uniq(self, prefix):
        self._uniq += 1
        return "%s_%d" % (prefix, self._uniq)

    # ---- code emission ----------------------------------------------------
    def emit(self, *ws):
        self.t.emit(*ws)

    def load(self, reg, v):
        """reg = v: an int (any 32-bit value, zero-extended; small negatives
        sign-extended), ("u64", value) for a 64-bit constant, ("mem", addr) for
        the guest word at addr, ("mem64", addr) for the doubleword there, ("u8",
        addr) for the byte there."""
        if isinstance(v, tuple):
            kind, a = v
            if kind == "u64":
                self.load(reg, (a >> 32) & 0xFFFFFFFF)
                self.emit(rldicr(reg, reg, 32, 31))                    # sldi reg,reg,32
                self.emit(P.oris(reg, reg, (a >> 16) & 0xFFFF), P.ori(reg, reg, a & 0xFFFF))
            elif kind == "u8":
                self.emit(P.li32(reg, a), P.D(34, reg, reg, 0))                 # lbz
            else:
                self.emit(P.li32(reg, a), P.lwz(reg, reg, 0) if kind == "mem" else P.ld(reg, reg, 0))
        elif -0x8000 <= v < 0:
            self.emit(P.addi(reg, 0, v))
        else:
            v &= 0xFFFFFFFF
            self.emit(P.addis(reg, 0, (v >> 16) & 0xFFFF), P.ori(reg, reg, v & 0xFFFF),
                      rldicl(reg, reg, 0, 32))                            # clrldi: zero-extend

    def call(self, fn, *args, stack=(), rc=None):
        """Call a firmware import with args in r3.. (and the parameter save area
        for args 9+); record its return code under `rc` (default: fn name)."""
        assert len(args) <= 8
        for i, v in enumerate(args):
            self.load(3 + i, v)
        for i, v in enumerate(stack):
            self.load(11, v)
            self.emit(P.std(11, 1, 112 + 8 * i))
        if fn not in self.called:
            self.called.append(fn)
        self.t.bl("imp_" + fn)
        self.emit(P.ld(2, 1, 40))
        if rc is not False:
            self.record_reg(3, rc or fn)

    def syscall(self, num, *args, rc=None):
        for i, v in enumerate(args):
            self.load(3 + i, v)
        self.emit(P.addi(11, 0, num), P.sc())
        if rc:
            self.record_reg(3, rc)

    # ---- records ------------------------------------------------------------
    def record_reg(self, reg, label):
        """Record the low word of a GPR."""
        off = self.res_off
        self.res_off += 16
        self.emit(P.li32(11, self["res"] + off), P.stw(reg, 11, 0))
        self.records.append((label, off, 4, ()))

    def record_mem(self, label, addr, size, ignore=(), deref=False):
        """Snapshot `size` bytes (multiple of 4) at addr into the result area now.
        ignore: (offset, length) byte ranges that legitimately differ between
        implementations (kernel-assigned ids, host pointers) and are not compared."""
        assert size % 4 == 0 and size > 0
        off = self.res_off
        self.res_off = (self.res_off + size + 15) & ~15
        loop = self.uniq("cp")
        if deref:   # addr holds a guest pointer: snapshot what it points at
            self.emit(P.li32(9, addr), P.lwz(9, 9, 0), P.addi(9, 9, -4))
        else:
            self.emit(P.li32(9, addr - 4))
        self.emit(P.li32(10, self["res"] + off - 4), P.li32(12, size // 4), P.mtspr(9, 12))
        self.t.label(loop)
        self.emit(P.D(33, 12, 9, 4),          # lwzu r12,4(r9)
                  P.D(37, 12, 10, 4))         # stwu r12,4(r10)
        self.t.bdnz(loop)
        self.records.append((label, off, size, tuple(ignore)))
        assert self.res_off <= 0x10000

    # ---- control ------------------------------------------------------------
    def wait_word(self, addr, value, tries=5000, usec=1000, mask=None, cond="eq"):
        """Poll the guest word at addr until it is `cond` value (bounded):
        eq, ne, or geu (unsigned >=)."""
        tag = self.uniq("wait")
        self.emit(P.li32(20, tries))
        self.t.label(tag + "_poll")
        self.emit(P.li32(9, addr), P.lwz(9, 9, 0))
        if mask is not None:
            self.emit(P.li32(10, mask), P.X(31, 9, 9, 10, 28))   # and r9,r9,r10
        self.emit(P.li32(10, value), P.X(31, 0, 9, 10, 32 if cond == "geu" else 0))  # cmplw / cmpw
        bo, bi = {"eq": (12, 2), "ne": (4, 2), "geu": (4, 0)}[cond]
        self.t.bc(tag + "_done", bo, bi)
        self.syscall(SYS["usleep"], usec)
        self.emit(P.addi(20, 20, -1), P.D(11, 0, 20, 0))
        self.t.bc(tag + "_poll", 4, 2)
        self.t.label(tag + "_done")

    def store_word(self, addr, value):
        self.load(11, value)
        self.emit(P.li32(12, addr), P.stw(11, 12, 0))

    def record_nonzero(self, label, addr):
        """Record 1 if the guest word at addr is nonzero, else 0 (for counters
        whose exact value depends on timing)."""
        self.emit(P.li32(9, addr), P.lwz(9, 9, 0),
                  P.X(31, 9, 9, 0, 26),                                  # cntlzw r9,r9
                  21 << 26 | 9 << 21 | 9 << 16 | 27 << 11 | 5 << 6 | 31 << 1,  # srwi r9,r9,5
                  P.D(26, 9, 9, 1))                                      # xori r9,r9,1
        self.record_reg(9, label)

    def func(self, name, emit_fn, ret=0):
        """Define an ordinary PPU function `name` (callable through its OPD at
        T.opd_of(name), e.g. as a hook); emit_fn(T) emits its body. Returns
        `ret`."""
        if name not in self.funcs:
            self.funcs.append(name)
        self.threads.append((name, emit_fn, ("func", ret)))

    def opd_of(self, name):
        if name not in self.funcs:
            self.funcs.append(name)
        return self["opd"] + 8 * self.funcs.index(name)

    def settle(self, spurs, nspus):
        """Wait until every SPU of the instance is idling in the SPURS system
        service with no message pending (spuIdling all set, sysSrvMessage
        bits clear): the instance is then at rest, whatever the timing of
        either implementation."""
        m = (1 << nspus) - 1
        self.wait_word(spurs + 0x70, m, mask=(m << 8) | m)
        self.sleep_us(20000)

    def sleep_us(self, usec):
        self.syscall(SYS["usleep"], usec)

    def thread(self, name, emit_fn):
        """Define a PPU thread entry `name`; emit_fn(T) emits its body (it may
        call imports; r1 is its own stack). The thread exits with r3 = 0."""
        if name not in self.funcs:
            self.funcs.append(name)
        self.threads.append((name, emit_fn, "thread"))

    def start_thread(self, name, arg, tid_addr, prio=1000):
        self.emit(P.li32(5, self["tparam"]), P.li32(6, self["opd"] + 8 * self.funcs.index(name)),
                  P.stw(6, 5, 0))
        self.syscall(SYS["thread_create"], tid_addr, self["tparam"], arg, 0, prio, 0x10000, 1,
                     self["tname"])
        self.syscall(SYS["thread_start"], ("mem", tid_addr + 4))

    def join_thread(self, tid_addr, ret_addr):
        self.syscall(SYS["thread_join"], ("mem", tid_addr + 4), ret_addr)

    # ---- SPU binaries ---------------------------------------------------------
    def spu_raw(self, name, code, align=128):
        """Embed raw SPU code (e.g. a policy module) in guest memory."""
        code = code + b"\0" * (-len(code) % 16)
        return self.alloc("spu_" + name, len(code), align, code)

    def spu_elf(self, name, code, base):
        """Embed an SPU ELF built from raw code loaded at LS `base`."""
        key = ("elf", name)
        if key not in self.spu:
            self.spu[key] = wrap_spu(code, tempfile.mkdtemp(prefix="spurst"), name, base=base)
        b = self.spu[key]
        return self.alloc("elf_" + name, len(b), 128, b)

    # ---- SPURS helpers ----------------------------------------------------------
    def spurs_init(self, nspus=2, spu_prio=100, ppu_prio=1000, exit_if_no_work=0, name="spurs"):
        spurs = self.alloc(name, 4096, 128)
        attr = self.alloc(name + "_attr", 512, 8)
        self.call("_cellSpursAttributeInitialize", attr, 2, SDK_VERSION, nspus, spu_prio, ppu_prio,
                  exit_if_no_work, rc="%s: _cellSpursAttributeInitialize" % name)
        self.call("cellSpursInitializeWithAttribute", spurs, attr,
                  rc="%s: cellSpursInitializeWithAttribute" % name)
        return spurs


class SpuTask:
    """Helpers for hand-written SPU code: DMA, atomics and the SPURS task ABI."""

    def __init__(self, base):
        self.a = S.SpuAsm(base)
        self.a.il(127, 0)                      # r127 = 0 (EAH and other zeros)

    def dma(self, cmd, r_ls, r_ea, size, tag=2):
        a = self.a
        a.wrch(S.MFC_LSA, r_ls); a.wrch(S.MFC_EAH, 127); a.wrch(S.MFC_EAL, r_ea)
        a.il(126, size); a.wrch(S.MFC_Size, 126)
        a.il(126, tag); a.wrch(S.MFC_TagID, 126)
        a.il(126, cmd); a.wrch(S.MFC_Cmd, 126)
        a.wait_tag(tag, 126)

    def atomic_add(self, r_ea, r_add, r_ls_line, scratch=(120, 121, 122)):
        """Atomically add the quadword r_add to the 128-byte line at EA r_ea
        (first quadword), retrying GETLLAR/PUTLLC until it sticks."""
        a = self.a
        lab = "aa%d" % len(a.w)
        q, st, c = scratch
        a.label(lab)
        a.wrch(S.MFC_LSA, r_ls_line); a.wrch(S.MFC_EAH, 127); a.wrch(S.MFC_EAL, r_ea)
        a.il(c, S.GETLLAR); a.wrch(S.MFC_Cmd, c); a.rdch(c, S.MFC_RdAtomicStat)
        a.lqd(q, r_ls_line, 0); a.a(q, q, r_add); a.stqd(q, r_ls_line, 0)
        a.wrch(S.MFC_LSA, r_ls_line); a.wrch(S.MFC_EAH, 127); a.wrch(S.MFC_EAL, r_ea)
        a.il(c, S.PUTLLC); a.wrch(S.MFC_Cmd, c); a.rdch(st, S.MFC_RdAtomicStat)
        a.brnz(st, lab)

    def task_syscall(self, num, r_arg=None):
        """Call the taskset policy module's syscall entry (LS word 0x27C4):
        r3 = syscall number, r4 = argument; returns through r0."""
        a = self.a
        if r_arg is not None:
            a.ori(4, r_arg, 0)
        else:
            a.il(4, 0)
        a.lqa(125, 0x27C0); a.rotqbyi(125, 125, 4)
        a.il(3, num); a.bisl(0, 125)

    def bytes(self):
        return self.a.bytes()


def build(test_mod, out_path):
    """Two passes: collect imports/data, then emit with the import tables."""
    def make(imports):
        T = Test(test_mod.__name__, imports)
        T.alloc("hdr", 64); T.alloc("end", 64)
        t = T.t
        t.label("_start")
        T.emit(P.li32(30, T["saved_r1"]), P.std(1, 30, 0), P.stdu(1, 1, -512))
        T.emit(P.li32(3, T["hdr"]), P.addi(4, 0, len(TAG) + 7))
        t.bl("puts")
        T.call("cellSysmoduleLoadModule", 0x0A, rc="cellSysmoduleLoadModule(SPURS)")
        test_mod.body(T)
        # print every record, one line each
        for label, off, size, _ in T.records:
            for o in range(0, size, 0x400):
                # "@" marks the start of a record: runtime logging that lacks a
                # trailing newline can share the line, so the reader takes what
                # follows the marker
                T.emit(P.li32(3, T["at"]), P.addi(4, 0, 1))
                t.bl("puts")
                T.emit(P.li32(3, T["res"] + off + o), P.addi(4, 0, min(0x400, size - o)))
                t.bl("hexdump")
        T.emit(P.li32(3, T["end"]), P.addi(4, 0, len(TAG) + 5))
        t.bl("puts")
        T.emit(P.addi(3, 0, 0), P.addi(11, 0, SYS["process_exit"]), P.sc(), P.b(0))
        emit_routines(t, T.D)
        for name, fn, kind in T.threads:
            t.label(name)
            if kind[0] == "func":
                T.emit(P.mfspr(0, 8), P.std(0, 1, 16), P.stdu(1, 1, -256), P.std(2, 1, 40))
                fn(T)
                T.emit(P.addi(1, 1, 256), P.ld(0, 1, 16), P.mtspr(8, 0), P.addi(3, 0, kind[1]), P.blr())
                continue
            T.emit(P.stdu(1, 1, -256))
            fn(T)
            T.emit(P.addi(3, 0, 0), P.addi(11, 0, SYS["thread_exit"]), P.sc(), P.b(0))
        if imports:
            imports.emit(t, T.D)
        return T

    first = make(None)
    mods = {}
    for fn in first.called:
        mods.setdefault(module_of(fn), []).append(fn)
    T = make(Imports(mods))
    text = T.t.bytes()
    assert TEXT_BASE + len(text) <= DATA_BASE, "text overlaps data"
    data = bytearray(T.D.off)
    for n, o, size, init in T.D.items:
        data[o:o + len(init)] = init

    def put(addr, b):
        data[addr - DATA_BASE:addr - DATA_BASE + len(b)] = b
    toc = T["opd"] + 0x8000
    put(T["opd"], b"".join(struct.pack(">II", T.t.labels[f], toc) for f in T.funcs))
    put(T["hex"], b"0123456789abcdef")
    put(T["hdr"], TAG + b" BEGIN\n")
    put(T["end"], TAG + b" END\n")
    T.imports.fill(put, T.D, T.t)
    elf = write_elf(text, DATA_BASE, bytes(data), T["opd"], 8 * len(T.funcs), T.imports.extra_ph(T.D))
    open(out_path, "wb").write(elf)
    lines = []
    for label, off, size, ignore in T.records:
        for k, o in enumerate(range(0, size, 0x400)):
            n = min(0x400, size - o)
            ign = [(a - o, b) for a, b in ignore if o <= a < o + n]
            lines.append({"label": label if size <= 0x400 else "%s [part %d]" % (label, k),
                          "ignore": ign})
    return lines
