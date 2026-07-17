#!/usr/bin/env python3
"""
Find SPU function boundaries inside an embedded SPU ELF.

Emits a JSON list of {"start": addr, "end": addr} ready for
`spu_lifter.py --functions`.

Seeds the function set from four sources:
  1. ELF entry point (e_entry).
  2. Symbol table -- STT_FUNC entries (with their st_value/st_size if size
     is non-zero, which gives us *exact* boundaries when present).
  3. All brsl/brasl targets the disassembler can resolve in the code.
  4. Addresses loaded into $r0 by `il`/`ila` -- the link-register resume
     idiom (see collect_link_register_targets).

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


def collect_link_register_targets(insns):
    """Addresses loaded into $r0 (the SPU ABI link register) by an immediate.

    Not every call site is a `brsl`. Both hand-written SPU code and the SDK's
    shared stubs set up a resume address by hand and then reach the stub with a
    PLAIN branch, because the stub is entered from several places and spills the
    link register to LS rather than keeping it in $r0:

        2ce0:  ila  $r0, 0x2cf0      <- resume address, set by hand
        2ce4:  lqa  $r2, 0x12c0
        2ce8:  cgti $r2, $r2, -1
        2cec:  brnz $r2, 0x3038      <- plain branch, NOT brsl
        2cf0:  lqa  $r0, 0x12a0      <- the stub returns HERE via `bi $r0`
        ...
        3038:  lqa  $r3, 0x13f0
        3040:  stqa $r0, 0x1570      <- stub spills the hand-set link register

    Nothing ever *branches* to 0x2cf0, so the branch-target scan cannot see it,
    and the address is only ever materialised as an immediate. Without seeding
    it the address is a mid-function label: the `bi $r0` return resolves through
    spu_indirect_branch -> spu_lookup(), finds no registered function, and halts
    the SPU (SPU_STATUS_STOPPED_BY_HALT). Observed in LBP's wwsjob SPURS policy
    module, which halted at LS 0x2cf0 on every dispatch.

    Restricted to $r0: setting the *link register* to a code address means
    "resume here", which is a strong signal. Loading a code-looking constant
    into any other register is usually just data (the same PM does
    `ila $r81, 0x1440` for a table base). Over-seeding is cheap anyway -- the
    lifter chains fall-through between adjacent functions -- but a tight filter
    keeps the output honest.
    """
    targets = set()
    for ins in insns:
        if ins.mnemonic not in ("il", "ila"):
            continue
        ops = [t.strip() for t in ins.operands.split(",") if t.strip()]
        if len(ops) != 2 or ops[0] != "$r0":
            continue
        tok = ops[1]
        try:
            # `ila` renders hex ("0x2CF0"), `il` renders signed decimal.
            targets.add(int(tok, 16) if tok.lower().startswith(("0x", "-0x"))
                        else int(tok, 10))
        except ValueError:
            pass
    return targets


_INDIRECT_BR = {"bi", "bisl", "bisled", "biz", "binz", "bihz", "bihnz"}


def collect_computed_branch_targets(insns, window=8):
    """Targets of `bi $rN` where $rN was just loaded with an immediate address.

    The link-register form (collect_link_register_targets) is not the only way
    the SPU materialises a branch target as a constant. The interrupt-window
    idiom uses a *scratch* register, because `bie`/`bid` (branch indirect and
    enable/disable interrupts) are the only way to toggle the interrupt bit
    atomically with a jump:

        2d2c:  ila $r0, 0x2d40     <- where to resume once interrupts are off
        2d30:  ila $r2, 0x2d38     <- where to land with interrupts on
        2d34:  bie $r2             <- jump to 0x2d38, interrupts ENABLED
        2d38:  ai  $r0, $r0, 0     <- the window: any pending interrupt fires here
        2d3c:  bid $r0             <- jump to 0x2d40, interrupts DISABLED
        2d40:  ...

    We decode `bie`/`bid` as plain `bi` (the D/E bits only gate interrupts,
    which we do not model -- the branch itself is identical), so both land in
    spu_indirect_branch and both targets must be registered. 0x2d40 comes from
    the $r0 scan; 0x2d38 only from here. Observed in LBP's wwsjob SPURS policy
    module, which halted at 0x2d38 once the $r0 case was fixed.

    Scans backward a short window for the nearest `il`/`ila` writing the branch
    register -- the same "nearest preceding writer" technique compute_bi_r0_jumps
    uses. Keeping the window local is what makes this precise: an `ila` feeding a
    branch two instructions later is a jump target; a code-shaped constant loaded
    somewhere else entirely is usually just data.
    """
    targets = set()
    for idx, ins in enumerate(insns):
        if ins.mnemonic not in _INDIRECT_BR:
            continue
        ops = [t.strip() for t in ins.operands.split(",") if t.strip()]
        # bi/bisl emit only the target reg; biz/binz/bihz/bihnz emit (cond, target).
        if not ops or not ops[-1].startswith("$r"):
            continue
        reg = ops[-1]
        for j in range(idx - 1, max(-1, idx - 1 - window), -1):
            w = insns[j]
            if w.mnemonic not in ("il", "ila"):
                continue
            wops = [t.strip() for t in w.operands.split(",") if t.strip()]
            if len(wops) != 2 or wops[0] != reg:
                continue
            tok = wops[1]
            try:
                targets.add(int(tok, 16) if tok.lower().startswith(("0x", "-0x"))
                            else int(tok, 10))
            except ValueError:
                pass
            break   # nearest writer decides
    return targets


def collect_jump_table_targets(insns, code, code_start, code_end,
                               window=48, max_entries=32):
    """Targets reached through an in-code JUMP TABLE:

        ila  $r14, 0x1724C          <- TABLE base (an LS address in .text)
        shli $r13, $r15, 2          <- selector * 4
        lqx  $r11, $r13, $r14       <- load the 16-byte line holding the entry
        rotqby $r3, $r11, $r12      <- extract the 32-bit entry
        bi   $r3                    <- dispatch

    The `ila` materialises the table ADDRESS, not a branch target, so the
    il/ila-into-branch-register scans never see the entries — none of the
    handlers get lifted, every dispatch lands in spu_indirect_branch as a
    lookup MISS, and the C call chain silently unwinds to the lifted caller:
    the handler is skipped AND the guest stack frame is never restored.
    (Observed in LBP's FMOD SPU mixer: the 7-entry DSP-node table at LS
    0x1724C leaked 0x60 of guest stack per mix cycle until the task's LS was
    destroyed — and the skipped handlers were the entire DSP graph.)

    Recognizer: anchored on the `il`/`ila` itself, NOT on the branch. Pairing
    the table load with its `bi` by scanning a linear window fails in real
    code: compilers hoist the handler computation far above the dispatch and
    REACH the `bi` by a jump (LBP's shared loading job computes the handler
    ~140 instructions before the function epilogue's `bi $r2` and jumps
    there), so no window connects them. Instead, EVERY `il`/`ila` whose
    immediate is a 4-aligned in-code address is tried as a table base; it
    qualifies if the words there form a plausible table (below). A false
    positive only adds an entry point at a valid instruction boundary — cheap;
    a false negative silently skips an entire handler class — fatal.

    A table entry can be ABSOLUTE (the word is the handler address —
    pm_wwsjob's DSP table) or TABLE-RELATIVE (the code adds the table base
    before branching — LBP's loading job: table @0x198C = {0x8C,0x2F4,...},
    handlers at 0x198C+off). Which one it is can't be decided locally, so BOTH
    in-code-segment interpretations are seeded: a wrong-side seed is just an
    extra entry point at a valid instruction boundary, while a missing
    right-side seed loses the whole handler. Consecutive BE u32 words are read
    while at least one interpretation is a 4-aligned VALID INSTRUCTION START
    (the first word where neither is — adjacent float/data constants —
    terminates the table); bases yielding fewer than 2 entries are ignored.
    """
    del window                       # kept in the signature for compatibility
    valid_starts = {ins.addr for ins in insns}
    targets = set()
    seen_bases = set()
    for ins in insns:
        if ins.mnemonic not in ("il", "ila"):
            continue
        wops = [t.strip() for t in ins.operands.split(",") if t.strip()]
        if len(wops) != 2:
            continue
        try:
            tok = wops[1]
            tbl = int(tok, 16) if tok.lower().startswith(("0x", "-0x")) else int(tok, 10)
        except ValueError:
            continue
        if tbl == 0 or (tbl & 3) or not (code_start <= tbl < code_end):
            continue
        if tbl in seen_bases:
            continue
        seen_bases.add(tbl)
        off = tbl - code_start
        entries = set()
        for k in range(max_entries):
            o = off + 4 * k
            if o + 4 > len(code):
                break
            wrd = int.from_bytes(code[o:o + 4], "big")
            if wrd & 3:
                break
            abs_ok = wrd in valid_starts
            rel = tbl + wrd
            rel_ok = (rel & 3) == 0 and rel in valid_starts
            if not abs_ok and not rel_ok:
                break           # neither reading decodes as code -- table end
            if abs_ok:
                entries.add(wrd)
            if rel_ok:
                entries.add(rel)
        if len(entries) >= 2:
            targets.update(entries)
    return targets


def collect_duff_targets(insns, code_start, code_end, window=12, span=64):
    """Mid-ladder entries of a Duff's device (computed jump into an unrolled
    loop). Sony's SPU job CRT clears BSS with one:

        brsl $r8, .+4               <- PC-getter: r8 = link address
        andi $r6, $r6, 112          <- residual = (count & 112)
        rotmi $r4, $r6, -2          <- ... >> 2  (0,4,...,28)
        ai   $r8, $r8, 36           <- base = link + 36
        a    $r8, $r8, $r4          <- + residual
        bi   $r8                    <- jump INTO the stqd ladder

    The targets are mid-function addresses no table scan can see; the miss
    unwinds the C call chain out of the CRT, skipping everything after the
    clear loop (observed in LBP's loading job as branch-to-0 pc=0x207E4).
    Recognizer: a `bi $rX` whose register was built from a PC-getter
    (`brsl $rX, .+4`) plus an immediate `ai $rX, $rX, K` within `window`
    instructions. Seed link+K plus the following `span` bytes at 4-byte
    stride — the ladder entries are all real instruction starts, and the
    final in-code/valid-instruction filter drops any overshoot."""
    targets = set()
    for idx, ins in enumerate(insns):
        if ins.mnemonic != "bi":
            continue
        ops = [t.strip() for t in ins.operands.split(",") if t.strip()]
        if len(ops) != 1 or not ops[0].startswith("$r") or ops[0] == "$r0":
            continue
        reg = ops[0]
        link = None
        imm = 0
        for j in range(idx - 1, max(-1, idx - 1 - window), -1):
            w = insns[j]
            wops = [t.strip() for t in w.operands.split(",") if t.strip()]
            if w.mnemonic == "ai" and len(wops) == 3 \
                    and wops[0] == reg and wops[1] == reg:
                try:
                    imm += int(wops[2], 0)
                except ValueError:
                    break
                continue
            if w.mnemonic in ("brsl", "brasl") and wops and wops[0] == reg:
                try:
                    tgt = int(wops[-1], 0)
                except ValueError:
                    break
                if tgt == w.addr + 4:       # PC-getter, not a real call
                    link = w.addr + 4
                break
        if link is None or imm <= 0:
            continue
        base = link + imm
        for o in range(0, span + 4, 4):
            t = base + o
            if code_start <= t < code_end:
                targets.add(t)
    return targets


def collect_function_pointer_tables(buf, phs, code_start, code_end,
                                    insns_by_addr, min_run=3, max_run=1024):
    """Function-pointer tables that live in DATA, not .text.

    A program can dispatch through an array of code addresses: it loads an entry
    from a table in the data segment and calls/branches through it. LBP's FMOD
    SPU mixer does exactly this -- it walks a DSP-node handler table and calls
    each node's process function via `bisl $r0,$r3`, r3 loaded from the table.
    Because the pointers live in the R/W data segment and are materialised by a
    memory load (no `ila <target>` in the code), NONE of the code-scan seeders
    (branch / link-register / computed / jump-table) can find them: every
    dispatch through an entry lands in spu_indirect_branch as a lookup MISS ->
    branch-to-0, and the handler (an entire DSP effect) is skipped, so the mixer
    never finishes and never sets its completion event flag (the FMOD boot
    deadlock's audio-path form).

    Observed: a 5-entry table at LS 0x1AB04 in the flags=6 (R/W) segment =
    {0x0B980,0x0C5F0,0x15CF8,0x16D70,0x17100}; the trailing 0x0 sentinel is why
    a dispatch also reached pc=0x00000.

    Recogniser: scan every PT_LOAD segment for runs of >= min_run consecutive
    4-aligned BE32 words that are ALL valid instruction starts inside .text
    (present in insns_by_addr). A run that long of in-range, aligned code
    addresses is a pointer table, not incidental data: the text base is well
    above 0 so small counters/sizes/enums fall below code_start, real SPU
    opcodes encode high bits far above code_end, and float/vector constants
    almost never form a 3+ aligned-in-range sequence. A distinct-value guard
    stops a filled/repeated constant region from seeding.
    """
    targets = set()
    for ph in phs:
        if ph.get("type") != 1:            # PT_LOAD only
            continue
        off, fsz, vaddr = ph["off"], ph["filesz"], ph["vaddr"]
        seg = buf[off:off + fsz]
        skew = (-vaddr) & 3                 # keep words on the LS 4-byte grid
        run = []
        def _flush(r):
            if len(r) >= min_run and len(set(r)) >= 2:
                targets.update(r)
        i = skew
        while i + 4 <= len(seg):
            w = int.from_bytes(seg[i:i + 4], "big")
            if (w & 3) == 0 and code_start <= w < code_end and w in insns_by_addr:
                run.append(w)
                if len(run) >= max_run:
                    _flush(run); run = []
            else:
                _flush(run); run = []
            i += 4
        _flush(run)
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
    # Link-register resume addresses (`il`/`ila $r0, <code>` + a plain branch to
    # a stub that returns via `bi $r0`). Require the target to be 4-byte aligned
    # AND to decode as an instruction, so a scratch use of $r0 that happens to
    # hold a small constant cannot seed a bogus function.
    fptr_targets = collect_function_pointer_tables(
        buf, elf["phs"], code_start, code_end, insns_by_addr)
    for t in (collect_link_register_targets(insns)
              | collect_computed_branch_targets(insns)
              | collect_jump_table_targets(insns, code, code_start, code_end)
              | collect_duff_targets(insns, code_start, code_end)
              | fptr_targets):
        if code_start <= t < code_end and (t & 3) == 0 and t in insns_by_addr:
            seed_starts.add(t)
    if fptr_targets and verbose:
        print(f"  {len(fptr_targets)} data function-pointer-table target(s): "
              f"{', '.join(f'0x{a:X}' for a in sorted(fptr_targets))}")

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
