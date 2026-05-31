#!/usr/bin/env python3
"""
Find SPU function boundaries inside an embedded SPU ELF.

Emits a JSON list of {"start": addr, "end": addr} ready for
`spu_lifter.py --functions`.

Seeds the function set from three sources:
  1. ELF entry point (e_entry).
  2. Symbol table -- STT_FUNC entries (with their st_value/st_size if size
     is non-zero, which gives us *exact* boundaries when present).
  3. All brsl/brasl targets the disassembler can resolve in the code.

For each function start without a symbol-provided size, the end is
determined by scanning forward until we hit one of:
   - `stop` / `stopd`,
   - `bi $r0` (the SPU ABI return — link reg is $r0),
   - the next function start in the ordered seed set,
   - the end of the executable section.

The output also reports a "data tail" if the executable PT_LOAD segment
extends past the last detected function -- a hint that .rodata is linked
into .text, and that the `.word` count from `spu_lifter.py` should drop
once function boundaries are honoured.

Usage:
    python find_spu_functions.py <spu.elf> [--out functions.json]
                                           [--code-out code.bin]
                                           [--base ADDR]
"""

import argparse
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from spu_disasm import disassemble_spu, spu_decode  # noqa: E402

# ELF constants
ELF_MAGIC          = b"\x7FELF"
EM_SPU             = 23
SHT_SYMTAB         = 2
SHT_STRTAB         = 3
SHF_EXECINSTR      = 4
STT_FUNC           = 2


def be_u32(buf, off): return struct.unpack_from(">I", buf, off)[0]
def be_u16(buf, off): return struct.unpack_from(">H", buf, off)[0]


def parse_elf(buf):
    if buf[:4] != ELF_MAGIC:
        raise SystemExit("Not an ELF file")
    if buf[5] != 2:
        raise SystemExit("Not big-endian (SPU ELFs are BE)")
    if be_u16(buf, 0x12) != EM_SPU:
        raise SystemExit("Not an SPU ELF (e_machine != 23)")

    # 32-bit ELF header fields
    e_entry     = be_u32(buf, 0x18)
    e_phoff     = be_u32(buf, 0x1C)
    e_shoff     = be_u32(buf, 0x20)
    e_phentsize = be_u16(buf, 0x2A)
    e_phnum     = be_u16(buf, 0x2C)
    e_shentsize = be_u16(buf, 0x2E)
    e_shnum     = be_u16(buf, 0x30)
    e_shstrndx  = be_u16(buf, 0x32)

    # Program headers (32-bit Elf32_Phdr = 32 bytes)
    phs = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type, p_off, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = \
            struct.unpack_from(">IIIIIIII", buf, off)
        phs.append(dict(type=p_type, off=p_off, vaddr=p_vaddr, filesz=p_filesz,
                        memsz=p_memsz, flags=p_flags))

    # Section headers (Elf32_Shdr = 40 bytes)
    shs = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        sh_name, sh_type, sh_flags, sh_addr, sh_off, sh_size, sh_link, sh_info, \
            sh_addralign, sh_entsize = struct.unpack_from(">IIIIIIIIII", buf, off)
        shs.append(dict(name_off=sh_name, type=sh_type, flags=sh_flags,
                        addr=sh_addr, off=sh_off, size=sh_size, link=sh_link,
                        info=sh_info, entsize=sh_entsize))

    # Section name string table (may be missing in stripped images)
    shstrs = b""
    if 0 <= e_shstrndx < len(shs):
        s = shs[e_shstrndx]
        shstrs = buf[s["off"]:s["off"] + s["size"]]

    def sec_name(sh):
        n = sh["name_off"]
        if n >= len(shstrs):
            return ""
        end = shstrs.find(b"\x00", n)
        return shstrs[n:end].decode("ascii", "replace")

    for sh in shs:
        sh["name"] = sec_name(sh)

    return dict(entry=e_entry, phs=phs, shs=shs)


def read_symbols(buf, shs):
    """Return [{addr, size, name}, ...] for every STT_FUNC symbol."""
    funcs = []
    for sh in shs:
        if sh["type"] != SHT_SYMTAB:
            continue
        strtab = shs[sh["link"]] if 0 <= sh["link"] < len(shs) else None
        strbuf = buf[strtab["off"]:strtab["off"] + strtab["size"]] if strtab else b""
        for i in range(sh["size"] // sh["entsize"]):
            off = sh["off"] + i * sh["entsize"]
            st_name, st_value, st_size, st_info, st_other, st_shndx = \
                struct.unpack_from(">IIIBBH", buf, off)
            if (st_info & 0xF) != STT_FUNC:
                continue
            name = ""
            if st_name < len(strbuf):
                e = strbuf.find(b"\x00", st_name)
                name = strbuf[st_name:e].decode("ascii", "replace")
            funcs.append(dict(addr=st_value, size=st_size, name=name))
    return funcs


def pick_text(phs):
    """Return (file_off, vaddr, size) of the executable PT_LOAD segment."""
    for ph in phs:
        if ph["type"] == 1 and (ph["flags"] & 1):     # PT_LOAD + PF_X
            return ph["off"], ph["vaddr"], ph["filesz"]
    raise SystemExit("No executable PT_LOAD segment found")


_BRANCH_MNEMONICS = {
    "brsl", "brasl",                          # direct calls (definitely functions)
    "br", "bra",                              # unconditional branches
    "brz", "brnz", "brhz", "brhnz",           # conditional branches
}

def collect_branch_targets(insns):
    """All branch targets the disassembler resolved within `insns`.

    Includes calls (brsl/brasl) AND non-call branches (br/bra/brz/brnz/...).
    Conditional / unconditional cross-function branches are common — without
    seeding them, the lifter sees calls to functions the detector never
    promoted, producing linker-unresolved spu_func_X symbols.
    """
    targets = set()
    for ins in insns:
        if ins.mnemonic not in _BRANCH_MNEMONICS:
            continue
        ops = [t.strip() for t in ins.operands.split(",") if t.strip()]
        for t in ops:
            if t.startswith("0x"):
                try:
                    targets.add(int(t, 16))
                except ValueError:
                    pass
    return targets


# Kept for backwards compatibility / call-only counting.
def collect_brsl_targets(insns):
    return {t for ins in insns if ins.mnemonic in ("brsl", "brasl")
            for t in [int(tok.strip(), 16) for tok in ins.operands.split(",")
                      if tok.strip().startswith("0x")]}


# Mnemonics that end a function's fall-through region.
_TERMINATORS_NO_FALLTHROUGH = {
    "stop", "stopd",        # absolute end
    "br", "bra", "iret",    # unconditional jumps -- not necessarily function end,
                            # but no fallthrough past them
}


def is_return(ins):
    """SPU ABI: `bi $r0` is the standard return."""
    if ins.mnemonic != "bi":
        return False
    ops = [t.strip() for t in ins.operands.split(",") if t.strip()]
    return ops == ["$r0"]


def find_end(start, insns_by_addr, sorted_starts, code_end):
    """Walk forward from `start` until a terminator or the next function."""
    # Index of next function start
    i = 0
    while i < len(sorted_starts) and sorted_starts[i] <= start:
        i += 1
    next_start = sorted_starts[i] if i < len(sorted_starts) else code_end

    pc = start
    while pc < next_start and pc < code_end:
        ins = insns_by_addr.get(pc)
        if ins is None:
            break
        if is_return(ins):
            return pc + 4
        if ins.mnemonic in _TERMINATORS_NO_FALLTHROUGH:
            # Unconditional control flow -- if the target is not within this
            # range, the function ends here.
            return pc + 4
        pc += 4

    # Hit next function or end of code; that's our boundary.
    return min(next_start, code_end)


def detect_functions(buf, base_override=None, verbose=True):
    elf = parse_elf(buf)
    text_off, text_va, text_size = pick_text(elf["phs"])
    code = buf[text_off:text_off + text_size]
    base = base_override if base_override is not None else text_va
    insns = disassemble_spu(code, base_addr=base)
    insns_by_addr = {ins.addr: ins for ins in insns}
    code_start = base
    code_end   = base + len(code)

    # ---- seeds ----
    syms = read_symbols(buf, elf["shs"])
    seed_starts = set()
    sized_funcs = []   # (start, end) from symbols with non-zero size

    # ELF entry point — 0 is a valid LS address, so don't treat falsy as unset.
    if code_start <= elf["entry"] < code_end:
        seed_starts.add(elf["entry"])
    for s in syms:
        if code_start <= s["addr"] < code_end:
            seed_starts.add(s["addr"])
            if s["size"] > 0:
                sized_funcs.append((s["addr"], s["addr"] + s["size"]))
    for t in collect_branch_targets(insns):
        if code_start <= t < code_end:
            seed_starts.add(t)

    # Always cover the entry of the text segment itself (some images have no
    # entry point but a function at va 0).
    if not seed_starts:
        seed_starts.add(code_start)

    sorted_starts = sorted(seed_starts)
    sized_by_start = {s: e for (s, e) in sized_funcs}

    # ---- end discovery ----
    funcs = []
    for start in sorted_starts:
        if start in sized_by_start:
            end = sized_by_start[start]
        else:
            end = find_end(start, insns_by_addr, sorted_starts, code_end)
        if end > start:
            funcs.append((start, end))

    # Coalesce / dedupe adjacent ranges produced by overlapping seeds:
    # If two seeds resolved to overlapping ranges keep the earlier one.
    funcs.sort()
    cleaned = []
    for s, e in funcs:
        if cleaned and s < cleaned[-1][1]:
            # Overlap: take the earlier start, extend end if needed.
            ps, pe = cleaned[-1]
            cleaned[-1] = (ps, max(pe, e))
        else:
            cleaned.append((s, e))

    if verbose:
        print(f"Text segment: va=0x{code_start:X} .. 0x{code_end:X}"
              f" ({len(code):,} bytes, {len(code)//4:,} instructions)")
        print(f"Seeds: entry={'set' if elf['entry'] else 'none'}, "
              f"{len(syms)} STT_FUNC symbols, "
              f"{len(collect_brsl_targets(insns))} brsl/brasl + "
              f"{len(collect_branch_targets(insns)) - len(collect_brsl_targets(insns))} "
              f"other branch targets, "
              f"{len(sorted_starts)} unique starts")
        print(f"Detected {len(cleaned)} function(s)")
        if cleaned:
            cov = sum(e - s for s, e in cleaned)
            print(f"Function coverage: {cov:,} / {len(code):,} bytes "
                  f"({100.0 * cov / len(code):.1f}%) -- "
                  f"the rest is data/padding embedded in .text")

    return cleaned, (text_off, base, len(code))


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("input", help="SPU ELF (e.g. extracted by extract_spu_images.py)")
    p.add_argument("--out", default=None, help="JSON output (default: stdout)")
    p.add_argument("--code-out", default=None,
                   help="Also write the raw .text bytes here (for "
                        "`spu_lifter.py --base <va>`)")
    p.add_argument("--base", type=lambda x: int(x, 0), default=None,
                   help="Override the .text base address")
    args = p.parse_args()

    with open(args.input, "rb") as f:
        buf = f.read()

    funcs, (text_off, base, size) = detect_functions(buf, args.base)

    out_obj = [{"start": s, "end": e} for s, e in funcs]
    if args.out:
        with open(args.out, "w") as f:
            json.dump(out_obj, f, indent=2)
        print(f"Wrote {args.out} ({len(out_obj)} function(s))")
    else:
        print(json.dumps(out_obj, indent=2))

    if args.code_out:
        with open(args.code_out, "wb") as f:
            f.write(buf[text_off:text_off + size])
        print(f"Wrote {args.code_out} ({size} bytes, base 0x{base:X})")


if __name__ == "__main__":
    sys.exit(main() or 0)
