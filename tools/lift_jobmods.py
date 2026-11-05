#!/usr/bin/env python3
"""
Batch-lift the extracted WWS SPU job modules to C.

Each module is DMA'd into the SPU job code buffer and entered at
base + entryOffset. The base is the link address in the module header (+0x28),
LS 0x4000 for almost every module (module_base). Because only one module is
resident in the code buffer at a time (the FMOD-overlay model), modules share
that base; the runtime picks the resident one by content signature.

For each module:
  1. wrap the raw bytes as a minimal SPU ELF (base, entry base+entryOff)
  2. run spu_lifter --auto-functions, which uses find_spu_functions.detect_functions
     to seed from entry + branch/call/jump-table targets -- so rodata past the
     last function is never disassembled (no .word data-as-code).
  3. unique --symbol-prefix per module so all 89 link together.

Output: <outdir>/<modname>/spu_recomp.{c,h}
Also writes <outdir>/jobmods_manifest.json: [{name, prefix, base, entry,
sig(hex16), size, funcs, unsupported}], and a registration TU (--register).

Engine-embedded modules (--engine-elf): some titles link their job modules
into the PPU engine ELF and DMA each one from a fixed effective address. The
modules are then found in the ELF's PT_LOAD segments instead of extracted
.bin files, and each registers as a verified streamed region
(spu_overlay_register_verified_region) keyed by its source EA instead of by
content signature. The span and the verified bytes are the module's lifted
CODE only, [0x4030, code_end): jobs DMA their own data into the module image
above the code, and any GET into a registered span from another source clears
the resident owner, after which every indirect branch of the running job falls
to the interpreter. In a retail title, code-only spans (together with lifting
entries reached only through indirect branches) took the median frame update
from ~79 ms to ~71 ms. code_end is the highest instruction reachable by direct
branches from the module's seeded entries, never past a CODE_END entry.
Like the lifted C, the generated registration TU embeds the title's code
bytes (the expected contents); it is generated output, the tool holds none.
The manifest additionally records ea, eas (every EA holding the module) and
code_end.

Usage:
  python tools/lift_jobmods.py --jobmods <port>/spu/jobmods                                --out     <port>/spu                                --register <port>/spu/jobmods_register.c
  python tools/lift_jobmods.py --engine-elf <port>/EBOOT.elf                                --out     <port>/spu
"""
import argparse, glob, hashlib, json, os, struct, subprocess, sys
sys.path.insert(0, os.path.dirname(__file__))
from wrap_spu_elf import wrap

ROOT   = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
LIFTER = os.path.join(os.path.dirname(__file__), "spu_lifter.py")
BASE   = 0x4000

# Per-module code/data boundary, keyed by the module's content hash -- so this
# table is title-agnostic by construction: any title shipping a byte-identical
# module gets the same fixup, and a module not listed here is simply not fixed
# up. A few images embed a large rodata/const tail
# inside the executable segment; its bytes decode as in-range branches that
# seed spurious "functions", disassembling the data as code (garbage + `.word`
# for undecodable words). The boundary was found by disassembly: after the
# last real unconditional `br`, a zlib-inflate error-string table + float
# const tables run to the image end. Passed to spu_lifter.py --code-end so the
# tail is left un-disassembled. Without it, these two physics jobmods each lift
# ~97 `.word` unsupported (was 0 in the original, pre-git-clean lift).
CODE_END = {
    "jobmod_ef33c6c99dc7_e0A40": 0xAE68,   # real code ends at br 0xA554 @ 0xAE60
    "jobmod_6943e26934ea_e0A20": 0xAE48,   # real code ends at br 0xA534 @ 0xAE40
}

# Extra function entries, in LS addresses, applied to every module that
# contains them ($LBP_JOBMOD_EXTRA_FUNCS, comma-separated).
#
# All 46 modules stream into the SAME job-code buffer at LS 0x4000 and only one
# is resident at a time, so an indirect-branch target seen at runtime is an LS
# address without a module attached to it -- there is no way to know from the
# BRANCH-TO-0 alone which image was resident. Offering the address to every
# module is the honest response: spu_lifter only splits a function that
# actually spans the address, so modules that do not contain it are unchanged.
EXTRA_FUNCS = [a.strip() for a in
               os.environ.get("LBP_JOBMOD_EXTRA_FUNCS", "").split(",") if a.strip()]


def manual_link_targets(raw, base=BASE):
    """Return targets built into the link register by hand:
        ila  $r0, off
        a    $r0, $r0, $r126        (r126 = module load base, from the entry's
        br   fn                      ila/brsl/sf base probe)
    Every job module's entry does this once (`ila $r0, entry+0xAC ... br body`):
    the body returns with `bi $r0` to base+off, an address no branch names, so
    the lift has no entry there. Without it the return unwinds to the manager's
    drain (drain-mismatch return_pc=0x3258 pc=0x4AEC), the restart re-enters the
    entry function from its start, the prologue runs twice and the job leaves
    on a garbage r0 (`exit: synthesised stop at LS 0xE81C`). Seeding base+off
    is exact: the offset is an immediate."""
    return sorted(set(t for _, t in manual_link_sites(raw, base)))


def manual_link_sites(raw, base=BASE):
    """(LS address of the `ila`, target) for each manual_link_targets entry."""
    out = []
    for off in range(0x30, len(raw) - 20, 4):
        w = struct.unpack_from(">I", raw, off)[0]
        if (w >> 25) & 0x7F != 0x21 or (w & 0x7F) != 0:          # ila $r0, i18
            continue
        imm = (w >> 7) & 0x3FFFF
        for k in range(1, 5):
            w2 = struct.unpack_from(">I", raw, off + 4 * k)[0]
            if (w2 >> 21) & 0x7FF == 0x0C0 and (w2 & 0x7F) == 0 and ((w2 >> 7) & 0x7F) == 0:
                if imm % 4 == 0 and 0x30 <= imm < len(raw):        # a $r0, $r0, $rX
                    out.append((base + off, base + imm))
                break
    return out



def switch_table_targets(raw, base=BASE):
    """Return the targets of position-independent switch tables.  The
    compiler emits, for `switch`:
        ila  $rT, tbl_off ; a $rT, $rT, $r126      ($rT = LS address of table)
        ... lqx/rotqby entry ... a $r2, $entry, $rT ; bi $r2
        tbl: .word target0 - tbl, target1 - tbl, ...  (right after the bi)
    Entries are relative to the table itself and the table follows the `bi`,
    so no absolute address of any case ever appears in the code: the lift
    only ever reached the cases by luck (`bi $r2` computed at run time,
    BRANCH-TO-0 unresolved pc=0x7A08 in jobmod ef33c6c99dc7). A table is
    accepted only when some `ila` in the module names its offset."""
    return sorted(set(t for _, t in switch_table_sites(raw, base)))


def switch_table_sites(raw, base=BASE):
    """(LS address of the `bi`, target) for each switch_table_targets entry."""
    ilas = set()
    for off in range(0x30, len(raw) - 4, 4):
        w = struct.unpack_from(">I", raw, off)[0]
        if (w >> 25) & 0x7F == 0x21:                          # ila $rX, i18
            ilas.add((w >> 7) & 0x3FFFF)
    out = []
    for off in range(0x30, len(raw) - 16, 4):
        w = struct.unpack_from(">I", raw, off)[0]
        if (w >> 21) & 0x7FF != 0x1A8 or (w & 0x7F) != 0:     # bi $rA (rt field 0)
            continue
        tbl = off + 4
        if tbl not in ilas:
            continue
        targets = []
        while tbl + 4 * len(targets) + 4 <= len(raw):
            e = struct.unpack_from(">I", raw, tbl + 4 * len(targets))[0]
            t = (tbl + e) & 0xFFFFFFFF
            if t % 4 or not (0x30 <= t < len(raw)):
                break
            targets.append(base + t)
        if len(targets) >= 2:
            out += [(base + off, t) for t in targets]
    return out



def module_base(raw):
    """The LS address the module is linked for. The header carries it at +0x28,
    after the 0xC0DEC0DE magic at +0x20: 0x4000, the job code buffer, for
    almost every module, but Resistance: Fall of Man also ships modules linked
    at 0x7000, and lifting those at 0x4000 put every lifted address 0x3000
    short of where the job manager runs them. Anything implausible (no magic,
    0, unaligned, or an image that would run past local store) falls back to
    BASE."""
    if len(raw) >= 0x2C and raw[0x20:0x24] == b"\xC0\xDE\xC0\xDE":
        b = struct.unpack_from(">I", raw, 0x28)[0]
        if b and b % 0x10 == 0 and b + len(raw) <= 0x40000:
            return b
    return BASE

# SPU branch encodings (SPU ISA): RI16 branches by 9-bit opcode, indirect
# branches by 11-bit opcode.
_RELATIVE_JUMP = {0x064}                            # br
_RELATIVE_CONT = {0x066, 0x040, 0x042, 0x044, 0x046}  # brsl, brz/brnz/brhz/brhnz
_ABSOLUTE_JUMP = {0x060}                            # bra
_ABSOLUTE_CONT = {0x062}                            # brasl
_INDIRECT_END  = {0x1A8, 0x1AA}                     # bi, iret: no fallthrough


def reachable(raw, entries, hi, base=BASE):
    """LS addresses of the instructions reachable from `entries` by direct
    branches and fallthrough, within [base + 0x30, hi). Indirect targets are
    unknown, so code reached only through them is not followed."""
    lo = base + 0x30
    seen, work = set(), [e for e in entries if lo <= e < hi]
    while work:
        pc = work.pop()
        while lo <= pc < hi and pc not in seen and not pc & 3:
            seen.add(pc)
            w = struct.unpack_from(">I", raw, pc - base)[0]
            op9, op11 = w >> 23, w >> 21
            i16 = (w >> 7) & 0xFFFF
            if op9 in _RELATIVE_JUMP or op9 in _RELATIVE_CONT:
                rel = i16 - 0x10000 if i16 & 0x8000 else i16
                work.append((pc + (rel << 2)) & 0x3FFFF)
            elif op9 in _ABSOLUTE_JUMP or op9 in _ABSOLUTE_CONT:
                work.append((i16 << 2) & 0x3FFFF)
            if op9 in _RELATIVE_JUMP or op9 in _ABSOLUTE_JUMP or op11 in _INDIRECT_END:
                break
            pc += 4
    return seen


def module_code_end(raw, name, base=BASE):
    """End of a module's code: one past the highest instruction reachable from
    its seeded entries, never past its CODE_END entry. The entry, +0x30 and
    $LBP_JOBMOD_EXTRA_FUNCS seed it directly; a manual link or switch table
    target seeds it once the `ila`/`bi` that names it is itself reachable, so a
    pattern decoded from the data tail cannot pull the end into the data. The
    lifter is then given only the seeds below this end, so every seeded entry
    it lifts lies inside the registered span."""
    hi = min(base + len(raw), CODE_END.get(name, base + len(raw)))
    seeds = {base + struct.unpack_from(">I", raw, 0x10)[0], base + 0x30}
    seeds |= {int(a, 0) for a in EXTRA_FUNCS}
    derived = manual_link_sites(raw, base) + switch_table_sites(raw, base)
    while True:
        seen = reachable(raw, seeds, hi, base)
        new = {t for site, t in derived if site in seen} - seeds
        if not new:
            break
        seeds |= new
    if not seen:
        return None
    return max(seen) + 4


def module_seeds(raw, base=BASE):
    """Extra function entries offered to the lifter for one module."""
    # Only offer addresses that fall inside THIS module's image.
    inside = [a for a in EXTRA_FUNCS if base <= int(a, 0) < base + len(raw)]
    # The module's first function starts right after the 0x30-byte header
    # (`4 ila | entry | size | 0 | 0 | C0DEC0DE | 0 | 0 | n`). Usually it is
    # reached by a branch and found anyway, but in two modules it is a bare
    # `bi $r0` stub reached only through a function pointer the job builds at
    # run time (base + 0x30, relocated via the entry's ila/brsl base probe), so
    # nothing in the code references it and the lift had no entry there: the
    # job's indirect call fell off the lifted set and came back with a
    # corrupted stack (drain-mismatch return_pc=0x50C8 pc=0xA800, jobmod
    # ef33c6c99dc7). Seeding it is exact, not a heuristic: every module's
    # first instruction is at +0x30.
    inside.append(hex(base + 0x30))
    inside += [hex(a) for a in manual_link_targets(raw, base)]
    inside += [hex(a) for a in switch_table_targets(raw, base)]
    return inside


def lift_module(raw, name, outdir, tmp_elf, merge_chunks, code_end=None,
                base=BASE):
    """Lift one module image at `base` into <outdir>/<name>/. Returns its
    manifest entry, or None if the lifter failed. `base` is the LS address the
    module is linked for -- module_base() reads it out of the header, and it is
    not always BASE."""
    entry_off = struct.unpack_from(">I", raw, 0x10)[0]
    entry = base + entry_off
    prefix = f"{name}_"
    outsub = os.path.join(outdir, name)
    # content signature: first 16 bytes (the 4 ila header uniquely identify a module)
    sig = raw[:16]

    elf = wrap(raw, base=base, entry=entry)
    with open(tmp_elf, "wb") as f:
        f.write(elf)

    cmd = [sys.executable, LIFTER, "--auto-functions", tmp_elf, "--base", hex(base),
           "--symbol-prefix", prefix, "-o", outsub,
           "--source-name", "spu_recomp.c", "--header-name", "spu_recomp.h"]
    end = code_end or CODE_END.get(name)
    if end:
        cmd += ["--code-end", hex(end)]
    if merge_chunks:
        cmd += ["--merge-chunks"]
    inside = module_seeds(raw, base)
    if code_end:
        inside = [a for a in inside if int(a, 0) < code_end]
    if inside:
        cmd += ["--extra-funcs", ",".join(inside)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"  FAIL {name}\n{r.stderr}", file=sys.stderr)
        return None
    # parse stats from lifter stdout
    funcs = unsup = 0
    for line in r.stdout.splitlines():
        s = line.strip()
        if s.endswith("function(s) lifted"):
            funcs = int(s.split()[0])
        if "unsupported instruction(s)" in s:
            unsup = int(s.split()[0])
    flag = "  <-- UNSUPPORTED" if unsup else ""
    print(f"  {name:40} funcs={funcs:3} unsup={unsup:3} entry=0x{entry:X}{flag}")
    return {
        "name": name, "prefix": prefix, "base": base, "entry": entry,
        "entry_off": entry_off, "sig": sig.hex(), "size": len(raw),
        "funcs": funcs, "unsupported": unsup,
    }


def elf_load_segments(elf):
    """(vaddr, file bytes) of each PT_LOAD segment of a big-endian ELF64."""
    if elf[:4] != b"\x7fELF" or elf[4] != 2 or elf[5] != 2:
        sys.exit("--engine-elf: expected a big-endian ELF64")
    phoff = struct.unpack_from(">Q", elf, 0x20)[0]
    phentsize, phnum = struct.unpack_from(">HH", elf, 0x36)
    out = []
    for i in range(phnum):
        p_type, _, off, vaddr, _, filesz = struct.unpack_from(">IIQQQQ", elf, phoff + i * phentsize)
        if p_type == 1 and filesz:
            out.append((vaddr, elf[off:off + filesz]))
    return out


def find_engine_modules(elf):
    """Job module images embedded in an engine ELF: a 16-byte aligned header
    with C0DEC0DE at +0x20 and the load base (0x4000) at +0x28, entry offset
    at +0x10 and image size at +0x14. Like extract_jobmods.py, the module is
    the initialised image, imageSize - 32 (the rest is bss), so a module gets
    the same name, and CODE_END fixup, from either source.
    Returns [(ea, raw)] sorted by EA."""
    out = []
    for vaddr, blob in elf_load_segments(elf):
        at = blob.find(b"\xc0\xde\xc0\xde")
        while at != -1:
            h = at - 0x20
            if h >= 0 and h % 16 == 0 and struct.unpack_from(">I", blob, h + 0x28)[0] == BASE:
                entry_off, image = struct.unpack_from(">II", blob, h + 0x10)
                size = image - 32
                if 0x30 <= entry_off < size and image <= 0x40000 - BASE and h + size <= len(blob):
                    out.append((vaddr + h, bytes(blob[h:h + size])))
            at = blob.find(b"\xc0\xde\xc0\xde", at + 4)
    return sorted(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--jobmods",
                     help="Directory of extracted jobmod_*.bin (extract_jobmods.py --out).")
    src.add_argument("--engine-elf", metavar="ELF",
                     help="Engine ELF the modules are embedded in at fixed EAs; register "
                          "them as verified streamed regions instead of by signature.")
    ap.add_argument("--out", required=True,
                    help="Directory to lift into: <out>/<modname>/spu_recomp.{c,h}.")
    ap.add_argument("--register", default=None, metavar="FILE",
                    help="Write the registration TU here (default: <out>/jobmods_register.c).")
    ap.add_argument("--symbol-prefix", default="port", metavar="NAME",
                    help="C symbol prefix for the registration functions, so two "
                         "ports linked in one tree cannot collide (default: port).")
    ap.add_argument("--merge-chunks", action="store_true",
                    help="Pass --merge-chunks to spu_lifter.py: each run of chunks "
                         "between call targets becomes one C function with gotos "
                         "instead of chunks linked by trampolines.")
    args = ap.parse_args()
    OUTDIR  = args.out
    register_c = args.register or os.path.join(OUTDIR, "jobmods_register.c")

    if EXTRA_FUNCS:
        print(f"  extra function entries: {', '.join(EXTRA_FUNCS)}")
    if args.engine_elf:
        return lift_engine_elf(args, register_c)
    JOBMODS = args.jobmods
    mods = sorted(glob.glob(os.path.join(JOBMODS, "*.bin")))
    if not mods:
        sys.exit(f"no jobmod_*.bin in {JOBMODS} -- run extract_jobmods.py first")
    os.makedirs(OUTDIR, exist_ok=True)
    manifest = []
    tmp_elf = os.path.join(OUTDIR, "_tmp.elf")
    for path in mods:
        raw = open(path, "rb").read()
        name = os.path.splitext(os.path.basename(path))[0]         # jobmod_<sha>_e<entry>
        m = lift_module(raw, name, OUTDIR, tmp_elf, args.merge_chunks,
                        base=module_base(raw))
        if m:
            manifest.append(m)

    if os.path.exists(tmp_elf):
        os.remove(tmp_elf)
    with open(os.path.join(OUTDIR, "jobmods_manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
    emit_register(manifest, register_c, args.symbol_prefix)
    tot_unsup = sum(m["unsupported"] for m in manifest)
    print(f"\nLifted {len(manifest)} module(s), {tot_unsup} total unsupported instruction(s)")
    print(f"Manifest: {os.path.join(OUTDIR, 'jobmods_manifest.json')}")


def lift_engine_elf(args, register_c):
    """--engine-elf: lift each module found in the ELF once (byte-identical
    copies at several EAs share one lift) with its code bounded at code_end,
    and register every EA as a verified streamed region."""
    found = find_engine_modules(open(args.engine_elf, "rb").read())
    if not found:
        sys.exit(f"no job modules found in {args.engine_elf}")
    by_name = {}
    for ea, raw in found:
        entry_off = struct.unpack_from(">I", raw, 0x10)[0]
        name = f"jobmod_{hashlib.sha1(raw).hexdigest()[:12]}_e{entry_off:04X}"
        by_name.setdefault(name, (raw, []))[1].append(ea)
    os.makedirs(args.out, exist_ok=True)
    manifest, code = [], {}
    tmp_elf = os.path.join(args.out, "_tmp.elf")
    for name, (raw, eas) in sorted(by_name.items(), key=lambda kv: kv[1][1][0]):
        code_end = module_code_end(raw, name)
        if code_end is None:
            print(f"  SKIP {name}: no reachable code", file=sys.stderr)
            continue
        m = lift_module(raw, name, args.out, tmp_elf, args.merge_chunks, code_end)
        if not m:
            continue
        m.update(ea=eas[0], eas=eas, code_end=code_end)
        code[name] = raw[0x30:code_end - BASE]
        manifest.append(m)
    if os.path.exists(tmp_elf):
        os.remove(tmp_elf)
    with open(os.path.join(args.out, "jobmods_manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
    emit_register_verified(manifest, code, register_c, args.symbol_prefix)
    tot_unsup = sum(m["unsupported"] for m in manifest)
    print(f"\nLifted {len(manifest)} module(s), {tot_unsup} total unsupported instruction(s)")
    print(f"Manifest: {os.path.join(args.out, 'jobmods_manifest.json')}")

# Image ids for the job-code overlays. Well clear of the fingerprint-dispatched
# images (1..25), the FMOD overlays (60..65) and the taskset policy (100).
JOBMOD_IMAGE_BASE = 200

def emit_register(manifest, REGISTER_C, sym_prefix):
    """Generate the registration TU: register every job module's lifted
    functions under a distinct image id, plus its 16-byte content signature so
    spu_overlay_note_get flips resident_ovl when the module DMAs into the job
    code buffer (LS 0x4000). Dispatched exactly like the FMOD codec overlays."""
    L = []
    L.append("/* jobmods_register.c -- GENERATED by tools/lift_jobmods.py.")
    L.append(" * WWS SPU job-code modules extracted from patch.sdat/data.sdat. Every module")
    L.append(" * streams into the same job code buffer (LS 0x4000) and is entered at")
    L.append(" * 0x4000+entryOffset; only one is resident at a time, so each registers its")
    L.append(" * lifted functions under a distinct image id and a 16-byte content signature.")
    L.append(" * spu_overlay_note_get() sets spu_context.resident_ovl on the DMA that copies")
    L.append(" * the module in; dispatch then resolves 0x4000+ pcs against that overlay. */")
    L.append("#include <stdint.h>")
    L.append("")
    L.append("extern void spu_begin_image(int image_id);")
    L.append("extern void spu_overlay_register_sig(const uint8_t sig[16], int image_id);")
    L.append("")
    for m in manifest:
        L.append(f"extern void {m['prefix']}spu_recomp_register(void);")
    L.append("")
    L.append(f"void {sym_prefix}_jobmods_register_all(void)")
    L.append("{")
    for i, m in enumerate(manifest):
        img = JOBMOD_IMAGE_BASE + i
        sig = bytes.fromhex(m["sig"])
        arr = ",".join(f"0x{b:02X}" for b in sig)
        L.append(f"    {{ static const uint8_t sig[16] = {{{arr}}};")
        L.append(f"      spu_begin_image({img}); {m['prefix']}spu_recomp_register();")
        L.append(f"      spu_overlay_register_sig(sig, {img}); }}   /* {m['name']} entry 0x{m['entry']:X} */")
    L.append("    spu_begin_image(0);")
    L.append("}")
    L.append("")
    L.append(f"__attribute__((constructor)) static void {sym_prefix}_jobmods_register_ctor(void)")
    L.append("{")
    L.append(f"    {sym_prefix}_jobmods_register_all();")
    L.append("}")
    L.append("")
    os.makedirs(os.path.dirname(REGISTER_C), exist_ok=True)
    with open(REGISTER_C, "w") as f:
        f.write("\n".join(L))
    print(f"Wrote {REGISTER_C} ({len(manifest)} modules, image ids "
          f"{JOBMOD_IMAGE_BASE}..{JOBMOD_IMAGE_BASE + len(manifest) - 1})")

def emit_register_verified(manifest, code, REGISTER_C, sym_prefix):
    """Generate the registration TU for --engine-elf: register every job
    module's lifted functions under a distinct image id and each EA it is
    streamed from as a verified region (spu_overlay_register_verified_region).
    A GET from that EA into LS 0x4000 makes the module resident; the first
    dispatch into the span compares the code bytes once per load.

    The span is the lifted code alone, [0x4000, code_end), and the one verified
    segment is [0x4030, code_end). Jobs DMA their own data into the module
    image above the code; any GET into a span from another source clears the
    resident owner, so a whole-image span sent every later indirect branch of
    the running job to the interpreter. A GET of another module into the
    buffer still overlaps the code span and clears it. The 0x30-byte header
    holds no code and is not compared."""
    L = []
    L.append("/* jobmods_register.c -- GENERATED by tools/lift_jobmods.py --engine-elf.")
    L.append(" * WWS SPU job-code modules embedded in the engine ELF. Every module streams")
    L.append(" * from its EA into the same job code buffer (LS 0x4000); each registers its")
    L.append(" * lifted functions under a distinct image id and its EA as a verified")
    L.append(" * streamed region whose span and expected bytes cover the lifted code only,")
    L.append(" * so the job's own data DMAs above the code keep it resident. Contains the")
    L.append(" * title's code bytes, like the lifted C it registers. */")
    L.append("#include <stdint.h>")
    L.append("#include <stdio.h>")
    L.append('#include "spu_overlay_residency.h"')
    L.append("")
    L.append("extern void spu_begin_image(int image_id);")
    L.append("")
    for m in manifest:
        L.append(f"extern void {m['prefix']}spu_recomp_register(void);")
    L.append("")
    for i, m in enumerate(manifest):
        b = code[m["name"]]
        L.append(f"static const uint8_t code_{JOBMOD_IMAGE_BASE + i}[{len(b)}] = {{")
        for k in range(0, len(b), 16):
            L.append("    " + ",".join(f"0x{x:02X}" for x in b[k:k + 16]) + ",")
        L.append("};")
    L.append("")
    L.append(f"void {sym_prefix}_jobmods_register_all(void)")
    L.append("{")
    for i, m in enumerate(manifest):
        img = JOBMOD_IMAGE_BASE + i
        span = m["code_end"] - BASE
        L.append(f"    {{ static const spu_overlay_code_segment seg = "
                 f"{{0x{BASE + 0x30:X}u, sizeof code_{img}, code_{img}}};")
        L.append(f"      spu_begin_image({img}); {m['prefix']}spu_recomp_register();   "
                 f"/* {m['name']} entry 0x{m['entry']:X} */")
        for ea in m["eas"]:
            L.append(f"      if (!spu_overlay_register_verified_region(0x{ea:08X}u, 0x{BASE:X}u, "
                     f"0x{span:X}u, {img}, &seg, 1))")
            L.append(f'          fprintf(stderr, "[jobmods] image {img}: verified region '
                     f'0x{ea:08X} not registered, interpreted\\n");')
        L.append("    }")
    L.append("    spu_begin_image(0);")
    L.append("}")
    L.append("")
    L.append(f"__attribute__((constructor)) static void {sym_prefix}_jobmods_register_ctor(void)")
    L.append("{")
    L.append(f"    {sym_prefix}_jobmods_register_all();")
    L.append("}")
    L.append("")
    os.makedirs(os.path.dirname(REGISTER_C) or ".", exist_ok=True)
    with open(REGISTER_C, "w") as f:
        f.write("\n".join(L))
    print(f"Wrote {REGISTER_C} ({len(manifest)} modules as verified regions, image ids "
          f"{JOBMOD_IMAGE_BASE}..{JOBMOD_IMAGE_BASE + len(manifest) - 1})")

if __name__ == "__main__":
    main()
