#!/usr/bin/env python3
"""Generate the PPU conformance ELF.

The ELF is a bare lv2 program (no imports, raw syscalls only) that, for every
test case:

  1. loads a full register state from its input slot: GPRs, FPRs, VRs, CR,
     XER, LR, CTR, FPSCR, VSCR, VRSAVE (+ resets a scratch buffer for memory
     tests),
  2. executes the instruction(s) under test,
  3. saves the full state back over the same slot and prints it as one hex
     line through sys_tty_write (plus the scratch buffer for memory tests).

The same bytes run on RPCS3 (the oracle: its PPU interpreter) and through
ps3recomp (ppu_loader -> ppu_lifter -> runtime). run_ppu_conform.py diffs the
two transcripts field by field.

Registers r1 (stack) and r31 (state-slot pointer) are never operands of an
instruction under test. Everything else is.

usage: gen_ppu_conform.py -o ppu_conform.elf [--cases cases.json] [--seed N]
       [--scale N] [--only OP[,OP]]
"""
import argparse
import json
import os
import random
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ppc_asm as A  # noqa: E402
import ppu_isa  # noqa: E402

TEXT_BASE = 0x00010000
# OPD descriptors (one per function) so the lifter sees each routine as a function
FUNCS = ["_start", "hexdump", "puts", "reset_scratch", "load_state", "save_state", "dump_scratch"]

# ---- state slot layout (input and output share it) ----
S_GPR, S_FPR, S_VR = 0x000, 0x100, 0x200
S_CR, S_XER, S_LR, S_CTR = 0x400, 0x408, 0x410, 0x418
S_FPSCR, S_VSCR, S_VRSAVE, S_FLAG = 0x420, 0x430, 0x440, 0x444
STATE_SIZE = 0x480

SCRATCH_SIZE = 0x400
GROUP = 200          # cases per guest function (keeps lifted functions compilable)
SPR_XER, SPR_LR, SPR_CTR, SPR_VRSAVE = 1, 8, 9, 256

SYS_PROCESS_EXIT, SYS_TTY_WRITE = 3, 403


class Asm:
    """Text section under construction, with labels and fixups."""

    def __init__(self, base):
        self.base, self.w, self.labels, self.fix = base, [], {}, []

    @property
    def pc(self):
        return self.base + 4 * len(self.w)

    def emit(self, *ws):
        for w in ws:
            if isinstance(w, (list, tuple)):
                self.emit(*w)
            else:
                self.w.append(w & 0xFFFFFFFF)

    def label(self, name):
        self.labels[name] = self.pc

    def bl(self, name):
        self.fix.append((len(self.w), name, "b", 1))
        self.w.append(0)

    def bdnz(self, name):
        self.fix.append((len(self.w), name, "bc", 0))
        self.w.append(0)

    def b(self, name):
        self.fix.append((len(self.w), name, "b", 0))
        self.w.append(0)

    def bc(self, name, bo, bi):
        """Conditional branch to a label: beq = (12, 2), bne = (4, 2) on CR0."""
        self.fix.append((len(self.w), name, (bo, bi), 0))
        self.w.append(0)

    def resolve(self):
        for i, name, kind, lk in self.fix:
            off = self.labels[name] - (self.base + 4 * i)
            if kind == "b":
                self.w[i] = A.b(off, lk)
            elif isinstance(kind, tuple):
                self.w[i] = (16 << 26) | (kind[0] << 21) | (kind[1] << 16) | (off & 0xFFFC)
            else:  # bdnz: BO=16, BI=0
                self.w[i] = (16 << 26) | (16 << 21) | (off & 0xFFFC)

    def bytes(self):
        self.resolve()
        return A.words(self.w)


def emit_routines(t, d):
    """load_state / save_state / hexdump / reset_scratch. r31 = state slot."""
    # hexdump: r3 = src, r4 = len -> one line on tty 0. Clobbers r3..r11, CTR, CR0.
    t.label("hexdump")
    t.emit(A.li32(5, d["line"]), A.li32(6, d["hex"]), A.mtspr(SPR_CTR, 4))
    t.label("hexdump_loop")
    t.emit(A.D(34, 7, 3, 0),                                   # lbz r7,0(r3)
           (21 << 26) | (7 << 21) | (8 << 16) | (28 << 11) | (28 << 6) | (31 << 1),  # rlwinm r8,r7,28,28,31
           A.X(31, 8, 6, 8, 87),                               # lbzx r8,r6,r8
           A.D(38, 8, 5, 0),                                   # stb r8,0(r5)
           (28 << 26) | (7 << 21) | (9 << 16) | 15,            # andi. r9,r7,15
           A.X(31, 9, 6, 9, 87),                               # lbzx r9,r6,r9
           A.D(38, 9, 5, 1),                                   # stb r9,1(r5)
           A.addi(3, 3, 1), A.addi(5, 5, 2))
    t.bdnz("hexdump_loop")
    t.emit(A.addi(7, 0, 10), A.D(38, 7, 5, 0),                 # '\n'
           A.addi(5, 5, 1),
           A.li32(4, d["line"]),
           (31 << 26) | (5 << 21) | (4 << 16) | (5 << 11) | (40 << 1),  # subf r5,r4,r5 (= length)
           A.addi(3, 0, 0), A.li32(6, d["written"]),
           A.addi(11, 0, SYS_TTY_WRITE), A.sc(), A.blr())

    # puts: r3 = string address, r4 = length
    t.label("puts")
    t.emit(A.ori(5, 4, 0), A.ori(4, 3, 0), A.addi(3, 0, 0), A.li32(6, d["written"]),
           A.addi(11, 0, SYS_TTY_WRITE), A.sc(), A.blr())

    # reset_scratch: copy pristine -> work (doublewords). Clobbers r3..r6, CTR.
    t.label("reset_scratch")
    t.emit(A.li32(3, d["pristine"] - 8), A.li32(4, d["scratch"] - 8),
           A.addi(5, 0, SCRATCH_SIZE // 8), A.mtspr(SPR_CTR, 5))
    t.label("reset_loop")
    t.emit((58 << 26) | (6 << 21) | (3 << 16) | 8 | 1,         # ldu r6,8(r3)
           (62 << 26) | (6 << 21) | (4 << 16) | 8 | 1)         # stdu r6,8(r4)
    t.bdnz("reset_loop")
    t.emit(A.blr())

    # load_state: everything but r0, r1, r31 and LR (the caller loads those after return).
    t.label("load_state")
    t.emit(A.lfd(0, 31, S_FPSCR), A.mtfsf(0))
    t.emit(A.addi(30, 0, S_VSCR), A.lvx(0, 31, 30), A.mtvscr(0))
    for v in range(32):
        t.emit(A.addi(30, 0, S_VR + 16 * v), A.lvx(v, 31, 30))
    for f in range(32):
        t.emit(A.lfd(f, 31, S_FPR + 8 * f))
    t.emit(A.lwz(0, 31, S_CR), A.mtcrf(0))
    t.emit(A.ld(0, 31, S_XER), A.mtspr(SPR_XER, 0))
    t.emit(A.ld(0, 31, S_CTR), A.mtspr(SPR_CTR, 0))
    t.emit(A.lwz(0, 31, S_VRSAVE), A.mtspr(SPR_VRSAVE, 0))
    for r in range(2, 31):
        t.emit(A.ld(r, 31, S_GPR + 8 * r))
    t.emit(A.blr())

    # save_state: the caller already stored r0 and LR. Restores r1, then prints the slot.
    t.label("save_state")
    for r in range(1, 32):
        t.emit(A.std(r, 31, S_GPR + 8 * r))
    t.emit(A.mfcr(0), A.stw(0, 31, S_CR))
    t.emit(A.mfspr(0, SPR_XER), A.std(0, 31, S_XER))
    t.emit(A.mfspr(0, SPR_CTR), A.std(0, 31, S_CTR))
    t.emit(A.mfspr(0, SPR_VRSAVE), A.stw(0, 31, S_VRSAVE))
    for f in range(32):
        t.emit(A.stfd(f, 31, S_FPR + 8 * f))
    t.emit(A.mffs(0), A.stfd(0, 31, S_FPSCR))
    for v in range(32):
        t.emit(A.addi(30, 0, S_VR + 16 * v), A.stvx(v, 31, 30))
    t.emit(A.mfvscr(0), A.addi(30, 0, S_VSCR), A.stvx(0, 31, 30))
    t.emit(A.li32(30, d["saved_r1"]), A.ld(1, 30, 0))
    t.emit(A.mfspr(29, SPR_LR), A.ori(3, 31, 0), A.addi(4, 0, STATE_SIZE))
    t.bl("hexdump")
    t.emit(A.mtspr(SPR_LR, 29), A.blr())

    t.label("dump_scratch")
    t.emit(A.mfspr(29, SPR_LR), A.li32(3, d["scratch"]), A.addi(4, 0, SCRATCH_SIZE))
    t.bl("hexdump")
    t.emit(A.mtspr(SPR_LR, 29), A.blr())


def build(cases, out_path):
    """cases: list of dicts {name, body(t, slot_addr) -> None, state bytes, mem}"""
    n = len(cases)
    # data layout is fixed relative to DATA_BASE; text size is known only after
    # emission, so emit text twice: once to size it, once for real.
    def layout(data_base):
        d = {}
        off = 0
        def take(name, size, align=16):
            nonlocal off
            off = (off + align - 1) & ~(align - 1)
            d[name] = data_base + off
            off += size
        take("opd", 8 * (len(FUNCS) + (n + GROUP - 1) // GROUP + sum(len(c.get("funcs", [])) for c in cases)))
        take("hex", 16)
        take("written", 8)
        take("saved_r1", 8)
        take("group_lr", 8)
        take("hdr", 64)
        take("end", 64)
        take("line", 2 * max(STATE_SIZE, SCRATCH_SIZE) + 16)
        take("pristine", SCRATCH_SIZE, 128)
        take("scratch", SCRATCH_SIZE, 128)
        take("slots", n * STATE_SIZE, 16)
        d["size"] = off
        return d

    extra_funcs = []

    def emit_text(d):
        t = Asm(TEXT_BASE)
        t.label("_start")
        t.emit(A.li32(30, d["saved_r1"]), A.std(1, 30, 0))
        t.emit(A.li32(3, d["hdr"]), A.addi(4, 0, len(HDR)))
        t.bl("puts")
        ngroups = (len(cases) + GROUP - 1) // GROUP
        del extra_funcs[:]
        for gi in range(ngroups):
            t.bl("group_%d" % gi)
        t.emit(A.li32(3, d["end"]), A.addi(4, 0, len(END)))
        t.bl("puts")
        t.emit(A.addi(3, 0, 0), A.addi(11, 0, SYS_PROCESS_EXIT), A.sc(), A.b(0))
        for gi in range(ngroups):
            t.label("group_%d" % gi)
            t.emit(A.mfspr(0, SPR_LR), A.li32(30, d["group_lr"]), A.std(0, 30, 0))
            for i in range(gi * GROUP, min(len(cases), (gi + 1) * GROUP)):
                c = cases[i]
                slot = d["slots"] + i * STATE_SIZE
                c["slot"] = slot
                if c.get("mem"):
                    t.bl("reset_scratch")
                t.emit(A.li32(31, slot))
                t.bl("load_state")
                t.emit(A.ld(0, 31, S_LR), A.mtspr(SPR_LR, 0), A.ld(0, 31, S_GPR))
                c["pc"] = t.pc
                for off in c.get("funcs", []):
                    extra_funcs.append(t.pc + off)
                c["body"](t, c)
                t.emit(A.std(0, 31, S_GPR), A.mfspr(0, SPR_LR), A.std(0, 31, S_LR))
                t.bl("save_state")
                if c.get("mem"):
                    t.bl("dump_scratch")
            t.emit(A.li32(30, d["group_lr"]), A.ld(0, 30, 0), A.mtspr(SPR_LR, 0), A.blr())
        emit_routines(t, d)
        return t

    HDR = b"PPUCONF BEGIN %d\n" % n
    END = b"PPUCONF END\n"
    d = layout(0x10000000)
    t = emit_text(d)
    text = t.bytes()
    data_base = (TEXT_BASE + len(text) + 0xFFFF) & ~0xFFFF
    d = layout(data_base)
    t = emit_text(d)                      # cases see the final slot/pc values
    text = t.bytes()
    assert TEXT_BASE + len(text) <= data_base

    data = bytearray(d["size"])
    def put(addr, b):
        o = addr - data_base
        data[o:o + len(b)] = b
    toc = d["opd"] + 0x8000
    fnames = FUNCS + ["group_%d" % gi for gi in range((n + GROUP - 1) // GROUP)]
    entries = [t.labels[f] for f in fnames] + extra_funcs
    put(d["opd"], b"".join(struct.pack(">II", e, toc) for e in entries))
    put(d["hex"], b"0123456789abcdef")
    put(d["hdr"], HDR)
    put(d["end"], END)
    pristine = ppu_isa.pristine_scratch()
    put(d["pristine"], pristine)
    put(d["scratch"], pristine)
    for c in cases:
        st = c["state"](c, d) if callable(c["state"]) else c["state"]
        c["_st"] = st
        assert len(st) == STATE_SIZE
        put(c["slot"], st)

    elf = write_elf(text, data_base, bytes(data), d["opd"], 8 * len(entries))
    open(out_path, "wb").write(elf)
    manifest = {"text_base": TEXT_BASE, "data_base": data_base, "scratch": d["scratch"],
                "state_size": STATE_SIZE, "scratch_size": SCRATCH_SIZE,
                "cases": [{"name": c["name"], "op": c["op"], "pc": c["pc"], "slot": c["slot"],
                           "mem": bool(c.get("mem")), "words": c.get("words", []), "mask": c.get("mask", {}), "vscr_dest": c.get("vscr_dest"), "resv": bool(c.get("resv")),
                           "input": c["_st"].hex()}
                          for c in cases]}
    return manifest


def write_elf(text, data_base, data, entry_opd, opd_size, extra_ph=()):
    """ELF64 BE PPC64 ET_EXEC: two PT_LOADs, plus section headers for .text
    and .opd (ppu_loader.py finds functions through the section holding e_entry).
    extra_ph: (p_type, vaddr, size) program headers for blocks inside the data
    segment (sys_process_param 0x60000001, sys_proc_prx_param 0x60000002)."""
    ehdr, phsz, phnum, shsz = 64, 56, 2 + len(extra_ph), 64
    text_off = 0x1000
    data_off = (text_off + len(text) + 0xFFF) & ~0xFFF
    shstr = b"\0.text\0.opd\0.data\0.shstrtab\0"
    shstr_off = data_off + len(data)
    sh_off = (shstr_off + len(shstr) + 7) & ~7
    out = bytearray(sh_off + 5 * shsz)
    out[0:16] = b"\x7fELF\x02\x02\x01\x66" + b"\x00" * 8   # ELFCLASS64, BE, OSABI 0x66 (CELL LV2)
    struct.pack_into(">HHI", out, 16, 2, 21, 1)
    struct.pack_into(">QQQ", out, 24, entry_opd, ehdr, sh_off)
    struct.pack_into(">IHHHHHH", out, 48, 0, ehdr, phsz, phnum, shsz, 5, 4)
    def ph(i, off, vaddr, filesz, memsz, flags):
        struct.pack_into(">IIQQQQQQ", out, ehdr + i * phsz, 1, flags, off, vaddr, vaddr,
                         filesz, memsz, 0x10000)
    ph(0, text_off, TEXT_BASE, len(text), len(text), 5)
    ph(1, data_off, data_base, len(data), len(data), 6)
    for i, (typ, vaddr, size) in enumerate(extra_ph):
        struct.pack_into(">IIQQQQQQ", out, ehdr + (2 + i) * phsz, typ, 4, data_off + vaddr - data_base,
                         vaddr, vaddr, size, size, 4)
    out[text_off:text_off + len(text)] = text
    out[data_off:data_off + len(data)] = data
    out[shstr_off:shstr_off + len(shstr)] = shstr
    def sh(i, name, typ, flags, addr, off, size, align):
        struct.pack_into(">IIQQQQIIQQ", out, sh_off + i * shsz, name, typ, flags, addr, off, size,
                         0, 0, align, 0)
    opd_off = data_off + (entry_opd - data_base)
    sh(1, shstr.index(b".text"), 1, 6, TEXT_BASE, text_off, len(text), 4)
    sh(2, shstr.index(b".opd"), 1, 3, entry_opd, opd_off, opd_size, 8)
    sh(3, shstr.index(b".data"), 1, 3, data_base, data_off, len(data), 16)
    sh(4, shstr.index(b".shstrtab"), 3, 0, 0, shstr_off, len(shstr), 1)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--manifest")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--scale", type=int, default=1, help="cases per op multiplier")
    ap.add_argument("--only", default="", help="comma-separated op names")
    args = ap.parse_args()
    rng = random.Random(args.seed)
    only = {s.strip().upper() for s in args.only.split(",") if s.strip()}
    cases = ppu_isa.make_cases(rng, args.scale, only)
    man = build(cases, args.out)
    mp = args.manifest or os.path.splitext(args.out)[0] + ".manifest.json"
    json.dump(man, open(mp, "w"))
    ops = sorted({c["op"] for c in cases})
    print("wrote %s: %d cases over %d ops; manifest %s" % (args.out, len(cases), len(ops), mp))


if __name__ == "__main__":
    main()
