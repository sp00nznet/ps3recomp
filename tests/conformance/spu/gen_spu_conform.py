#!/usr/bin/env python3
"""Generate the SPU instruction conformance ELF (suite "spu").

Every SPU instruction that computes on registers is run on random operands,
on RPCS3's SPU interpreter and through ps3recomp (its interpreter, or with
--lifted its lifted code), and the results are compared bit for bit.

Each case is one instruction word: the opcode, then RANDOM operand fields --
register numbers (aliasing included), immediates, shift counts, all of it.
The SPU loads a random quadword into every register an operand field names
(lqa: absolute, so no base register is reserved), executes the word, and
stores those registers back. Values mix per-lane specials (0, -0, +-1, 1.0f,
denormals, the extended-range top of single precision, double NaN/inf/denormal
patterns, small shift counts) with random bits.

Not here (they need a controlled base register or change control flow, and
get their own programs): loads/stores, branches, halts, channels, stop.

Cases are packed into SPU images of up to CHUNK cases (code at LS 0, inputs
at IN_LS, outputs at OUT_LS); each image runs as a one-thread group, PUTs its
outputs to main memory, and the PPU prints one 64-byte line per case
(rt, ra, rb, rc fields in that order -- whichever registers they name) between
SPUCONF BEGIN and SPUCONF END. <out>.manifest.json maps lines back to cases.
"""
import argparse
import json
import os
import random
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "mc"))
sys.path.insert(0, os.path.join(HERE, "..", "ppu"))
import ppc_asm as P          # noqa: E402
import spu_asm as S          # noqa: E402
from spu_ops import OPCODES  # noqa: E402
from gen_ppu_conform import Asm, write_elf, emit_routines, TEXT_BASE  # noqa: E402
from gen_mc_conform import wrap_spu, Data  # noqa: E402

SC = dict(process_exit=3, spu_initialize=169, image_import=157, group_create=170,
          group_destroy=171, thread_initialize=172, group_start=173, group_join=178,
          get_exit_status=165)

# Control flow, halts, channels, memory: not register-to-register.
EXCLUDE = {"stop", "stopd", "rdch", "rchcnt", "wrch",
           "lqd", "lqx", "lqa", "lqr", "stqd", "stqx", "stqa", "stqr",
           "br", "bra", "brsl", "brasl", "brz", "brnz", "brhz", "brhnz",
           "bi", "bisl", "bisled", "iret", "biz", "binz", "bihz", "bihnz",
           "heq", "heqi", "hgt", "hgti", "hlgt", "hlgti", "unk",
           # SPU ISA 1.2 double compares / test-special-value: PowerXCell 8i
           # only. The PS3's Cell SPU does not implement them (RPCS3 treats
           # them as fatal), so there is nothing to compare against.
           "dfceq", "dfcmeq", "dfcgt", "dfcmgt", "dftsv"}

CHUNK = 1024
IN_LS, OUT_LS, SAVE_LS = 0x10000, 0x20000, 0x3FF00
CASE_BYTES = 64                 # four quadwords per case
FUNCS = ["_start", "hexdump", "puts", "reset_scratch", "load_state", "save_state", "dump_scratch"]

W32 = [0x00000000, 0x80000000, 0x00000001, 0xFFFFFFFF, 0x7FFFFFFF, 0x3F800000, 0xBF800000,
       0x7F800000, 0xFF800000, 0x7FC00000, 0x7FFFFFFF, 0x00800000, 0x007FFFFF, 0x80000001,
       0x7F7FFFFF, 0x4F000000, 0xCF000000, 0x4F800000, 0x3F000000, 0x00000020, 0x0000001F,
       0x00000010, 0x0000000F, 0x00000080, 0x0000007F, 0x00008000, 0x0000FFFF, 0x00010000]
W64 = [0x0000000000000000, 0x8000000000000000, 0x3FF0000000000000, 0xBFF0000000000000,
       0x7FF0000000000000, 0xFFF0000000000000, 0x7FF8000000000000, 0x7FF4000000000000,
       0x0000000000000001, 0x000FFFFFFFFFFFFF, 0x0010000000000000, 0x7FEFFFFFFFFFFFFF,
       0x47EFFFFFE0000000, 0x3810000000000000, 0x380FFFFFFFFFFFFF, 0x41DFFFFFFFC00000]


def rand_qword(rng):
    """16 bytes: per 64-bit half either a double special, two word specials /
    small counts, or random bits."""
    out = b""
    for _ in range(2):
        k = rng.random()
        if k < 0.2:
            out += struct.pack(">Q", rng.choice(W64))
        else:
            for _ in range(2):
                j = rng.random()
                w = (rng.choice(W32) if j < 0.35 else rng.randrange(0, 140) if j < 0.5
                     else rng.getrandbits(32))
                out += struct.pack(">I", w)
    return out


def operand_regs(width, word):
    """The register fields an instruction of this opcode width has, in the
    order rt, ra, rb, rc (RRR: rt is bits 21-27, rc the low 7)."""
    rt, ra, rb, rc4 = word & 127, (word >> 7) & 127, (word >> 14) & 127, (word >> 21) & 127
    if width == 4:
        return [rc4, ra, rb, rt]           # dest (rt4), ra, rb, rc
    if width == 11:
        return [rt, ra, rb]
    if width in (10, 8):
        return [rt, ra]
    return [rt]                            # RI16 / RI18


def make_cases(per_op, seed, only):
    rng = random.Random(seed)
    cases = []
    for name, width, val in OPCODES:
        if name in EXCLUDE or (only and name not in only):
            continue
        for _ in range(per_op):
            word = (val << (32 - width)) | rng.getrandbits(32 - width)
            regs = operand_regs(width, word)
            inputs = [rand_qword(rng) for _ in range(4)]
            cases.append(dict(op=name, word=word, regs=regs,
                              inputs=[b.hex() for b in inputs]))

    # ---- T-0001 patterned pass: shift-count boundaries vs pointer data ----
    # A WWS scatter loop derives its store offset from shl (6-bit count) but
    # its step from shlqbi (3-bit count); a count > 7 splits the pair and
    # overran the whole local store (the once-per-boot stack smash). Random
    # operands hit these boundaries only by luck, so pin every boundary
    # count, each against pointer-like data patterns, for the register-form
    # shift/rotate ops. Compared bit-for-bit against RPCS3 like every case.
    SHIFT_OPS = ("shl", "shlh", "shlqbi", "shlqbybi", "shlqby",
                 "rot", "roth", "rotm", "rotma", "rotqbi", "rotqbybi", "rotqby")
    SHIFT_BOUNDARIES = (0, 1, 3, 6, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65)
    PTR_LIKE = (0x00033700, 0x0003FFF0, 0x3BFFFFFF, 0x382EC33E,
                0x003FFFFF, 0x4080FFFF, 0x7F800000, 0xFFFFFFFF)
    for name, width, val in OPCODES:
        if name in EXCLUDE or name not in SHIFT_OPS or (only and name not in only):
            continue
        for cnt in SHIFT_BOUNDARIES:
            for pat in PTR_LIKE:
                # rt=2 (dest), ra=3 (data), rb=4 (count) -- distinct, no aliasing
                word = (val << 21) | (4 << 14) | (3 << 7) | 2
                data_q = b"".join(struct.pack(">I", pat | (k << 28)) for k in range(4))
                cnt_q = b"".join(struct.pack(">I", cnt) for _ in range(4))
                cases.append(dict(op=name, word=word, regs=[2, 3, 4],
                                  inputs=["00" * 16, data_q.hex(), cnt_q.hex(),
                                          "00" * 16]))
    return cases


def spu_chunk_prog(cases):
    """Run each case; outputs at OUT_LS + 64*i; PUT them to arg1; exit 0x600D."""
    a = S.SpuAsm(0)
    a.stqa(3, SAVE_LS)
    for i, c in enumerate(cases):
        seen = []
        for k, r in enumerate(c["regs"]):
            if r not in seen:
                seen.append(r)
                a.lqa(r, IN_LS + CASE_BYTES * i + 16 * k)
        a._e(c["word"])
        for k, r in enumerate(c["regs"]):
            a.stqa(r, OUT_LS + CASE_BYTES * i + 16 * k)
    n = CASE_BYTES * len(cases)
    a.lqa(3, SAVE_LS)
    a.rotqbyi(3, 3, 4)                       # arg1 is a u64: low word -> preferred slot
    for off in range(0, n, 0x4000):
        a.ila(13, OUT_LS + off)
        a.li32(15, off); a.a(14, 3, 15)
        a.li32(11, min(0x4000, n - off))
        a.mfc(S.PUT, 13, 14, 11, 2, 12)
        a.wait_tag(2, 12)
    a.li32(30, 0x600D); a.wrch(S.SPU_WrOutMbox, 30); a.stop(0x102)
    code = a.bytes()
    assert len(code) <= IN_LS, "chunk code overflows into the input area"
    data = b"".join(bytes.fromhex(q) for c in cases for q in c["inputs"])
    return code.ljust(IN_LS, b"\0") + data


def build(out_path, per_op, seed, only):
    cases = make_cases(per_op, seed, only)
    chunks = [cases[i:i + CHUNK] for i in range(0, len(cases), CHUNK)]
    tmp = tempfile.mkdtemp(prefix="spuconf")
    imgs = [wrap_spu(spu_chunk_prog(c), tmp, "spu%d" % k) for k, c in enumerate(chunks)]
    spu_dir = os.path.splitext(out_path)[0] + "_spu"          # for --lifted
    os.makedirs(spu_dir, exist_ok=True)
    for k, b in enumerate(imgs):
        open(os.path.join(spu_dir, "spuconf_%d.elf" % k), "wb").write(b)

    D = Data()
    D.take("opd", 8 * len(FUNCS))
    for k in ("hex", "written", "saved_r1", "hdr", "end"):
        D.take(k, 64)
    D.take("line", 2 * 0x400 + 16)
    D.take("pristine", 16); D.take("scratch", 16)
    for k, b in enumerate(imgs):
        D.take("img%d" % k, len(b), 128, b)
        D.take("imgs%d" % k, 32)
    D.take("gname", 16, 16, b"spuconf\0")
    D.take("gattr", 16); D.take("tattr", 16); D.take("targ", 32)
    D.take("ids", 64); D.take("join", 16); D.take("exst", 16)
    D.take("out", CASE_BYTES * CHUNK, 128)
    HDR, END = b"SPUCONF BEGIN\n", b"SPUCONF END\n"

    def emit(d):
        t = Asm(TEXT_BASE)

        def sc(num, *args):
            for i, v in enumerate(args):
                t.emit(P.li32(3 + i, v))
            t.emit(P.addi(11, 0, num), P.sc())

        def lwz_arg(reg, addr):
            t.emit(P.li32(reg, addr), P.lwz(reg, reg, 0))

        def hexdump(addr, n):
            t.emit(P.li32(3, addr), P.addi(4, 0, n)); t.bl("hexdump")

        t.label("_start")
        t.emit(P.li32(30, d["saved_r1"]), P.std(1, 30, 0))
        t.emit(P.li32(3, d["hdr"]), P.addi(4, 0, len(HDR))); t.bl("puts")
        sc(SC["spu_initialize"], 6, 0)
        for k, b in enumerate(imgs):
            sc(SC["image_import"], d["imgs%d" % k], d["img%d" % k], len(b), 0)
        for k, c in enumerate(chunks):
            sc(SC["group_create"], d["ids"], 1, 100, d["gattr"])
            t.emit(P.li32(5, d["targ"]), P.li32(6, d["out"]), P.stw(6, 5, 4),
                   P.li32(6, 0), P.stw(6, 5, 12))
            lwz_arg(4, d["ids"])
            t.emit(P.li32(3, d["ids"] + 4), P.addi(5, 0, 0), P.li32(6, d["imgs%d" % k]),
                   P.li32(7, d["tattr"]), P.li32(8, d["targ"]),
                   P.addi(11, 0, SC["thread_initialize"]), P.sc())
            lwz_arg(3, d["ids"]); t.emit(P.addi(11, 0, SC["group_start"]), P.sc())
            lwz_arg(3, d["ids"])
            t.emit(P.li32(4, d["join"]), P.li32(5, d["join"] + 4),
                   P.addi(11, 0, SC["group_join"]), P.sc())
            lwz_arg(3, d["ids"] + 4)
            t.emit(P.li32(4, d["exst"]), P.addi(11, 0, SC["get_exit_status"]), P.sc())
            hexdump(d["join"], 8); hexdump(d["exst"], 4)
            # one line per case: r20 = record, r21 = cases left
            t.emit(P.li32(20, d["out"]), P.li32(21, len(c)))
            t.label("dump%d" % k)
            t.emit(P.addi(3, 20, 0), P.addi(4, 0, CASE_BYTES)); t.bl("hexdump")
            t.emit(P.addi(20, 20, CASE_BYTES), P.addi(21, 21, -1), P.D(11, 0, 21, 0))  # cmpwi r21,0
            t.bc("dump%d" % k, 4, 2)                                               # bne
            lwz_arg(3, d["ids"]); t.emit(P.addi(11, 0, SC["group_destroy"]), P.sc())
        t.emit(P.li32(3, d["end"]), P.addi(4, 0, len(END))); t.bl("puts")
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
    man = dict(chunks=[len(c) for c in chunks], cases=cases)
    json.dump(man, open(os.path.splitext(out_path)[0] + ".manifest.json", "w"))
    print("wrote %s (%d cases, %d ops, %d SPU images)" %
          (out_path, len(cases), len({c["op"] for c in cases}), len(imgs)))


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--per-op", type=int, default=24)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--only", default="", help="comma-separated mnemonics")
    a = ap.parse_args()
    build(a.out, a.per_op, a.seed, set(x for x in a.only.split(",") if x))
