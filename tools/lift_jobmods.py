#!/usr/bin/env python3
"""
Batch-lift the extracted WWS SPU job modules to C.

Each module is DMA'd into the SPU job code buffer at LS 0x4000 and entered at
0x4000 + entryOffset. Because only one module is resident in the code buffer at
a time (the FMOD-overlay model), every module lifts at the SAME base (0x4000);
the runtime picks the resident one by content signature.

For each module:
  1. wrap the raw bytes as a minimal SPU ELF (base 0x4000, entry 0x4000+entryOff)
  2. run spu_lifter --auto-functions, which uses find_spu_functions.detect_functions
     to seed from entry + branch/call/jump-table targets -- so rodata past the
     last function is never disassembled (no .word data-as-code).
  3. unique --symbol-prefix per module so all 89 link together.

Output: lbp_spu/lifted/<modname>/spu_recomp.{c,h}
Also writes lbp_spu/lifted/jobmods_manifest.json: [{name, prefix, base, entry,
sig(hex16), size, funcs, unsupported}].
"""
import glob, hashlib, json, os, struct, subprocess, sys
sys.path.insert(0, os.path.dirname(__file__))
from wrap_spu_elf import wrap

ROOT   = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
JOBMODS= os.path.join(ROOT, "lbp_spu", "jobmods")
OUTDIR = os.path.join(ROOT, "lbp_spu", "lifted")
LIFTER = os.path.join(os.path.dirname(__file__), "spu_lifter.py")
BASE   = 0x4000

# Per-module code/data boundary. A few images embed a large rodata/const tail
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
                    out.append(base + imm)
                break
    return sorted(set(out))



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
            out += targets
    return sorted(set(out))



def main():
    if EXTRA_FUNCS:
        print(f"  extra function entries: {', '.join(EXTRA_FUNCS)}")
    mods = sorted(glob.glob(os.path.join(JOBMODS, "*.bin")))
    os.makedirs(OUTDIR, exist_ok=True)
    manifest = []
    tmp_elf = os.path.join(OUTDIR, "_tmp.elf")
    for path in mods:
        raw = open(path, "rb").read()
        name = os.path.splitext(os.path.basename(path))[0]         # jobmod_<sha>_e<entry>
        sha  = name.split("_")[1]
        entry_off = struct.unpack_from(">I", raw, 0x10)[0]
        entry = BASE + entry_off
        prefix = f"{name}_"
        outsub = os.path.join(OUTDIR, name)
        # content signature: first 16 bytes (the 4 ila header uniquely identify a module)
        sig = raw[:16]

        elf = wrap(raw, base=BASE, entry=entry)
        with open(tmp_elf, "wb") as f:
            f.write(elf)

        cmd = [sys.executable, LIFTER, "--auto-functions", tmp_elf, "--base", hex(BASE),
               "--symbol-prefix", prefix, "-o", outsub,
               "--source-name", "spu_recomp.c", "--header-name", "spu_recomp.h"]
        if name in CODE_END:
            cmd += ["--code-end", hex(CODE_END[name])]
        # Only offer addresses that fall inside THIS module's image.
        inside = [a for a in EXTRA_FUNCS if BASE <= int(a, 0) < BASE + len(raw)]
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
        inside.append(hex(BASE + 0x30))
        inside += [hex(a) for a in manual_link_targets(raw)]
        inside += [hex(a) for a in switch_table_targets(raw)]
        if inside:
            cmd += ["--extra-funcs", ",".join(inside)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print(f"  FAIL {name}\n{r.stderr}", file=sys.stderr)
            continue
        # parse stats from lifter stdout
        funcs = unsup = 0
        for line in r.stdout.splitlines():
            s = line.strip()
            if s.endswith("function(s) lifted"):
                funcs = int(s.split()[0])
            if "unsupported instruction(s)" in s:
                unsup = int(s.split()[0])
        manifest.append({
            "name": name, "prefix": prefix, "base": BASE, "entry": entry,
            "entry_off": entry_off, "sig": sig.hex(), "size": len(raw),
            "funcs": funcs, "unsupported": unsup,
        })
        flag = "  <-- UNSUPPORTED" if unsup else ""
        print(f"  {name:40} funcs={funcs:3} unsup={unsup:3} entry=0x{entry:X}{flag}")

    if os.path.exists(tmp_elf):
        os.remove(tmp_elf)
    with open(os.path.join(OUTDIR, "jobmods_manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
    emit_register(manifest)
    tot_unsup = sum(m["unsupported"] for m in manifest)
    print(f"\nLifted {len(manifest)} module(s), {tot_unsup} total unsupported instruction(s)")
    print(f"Manifest: {os.path.join(OUTDIR, 'jobmods_manifest.json')}")

# Image ids for the job-code overlays. Well clear of the fingerprint-dispatched
# images (1..25), the FMOD overlays (60..65) and the taskset policy (100).
JOBMOD_IMAGE_BASE = 200
REGISTER_C = os.path.join(ROOT, "lbp", "gen", "jobmods_register.c")

def emit_register(manifest):
    """Generate lbp/gen/jobmods_register.c: register every job module's lifted
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
    L.append("void lbp_jobmods_register_all(void)")
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
    L.append("__attribute__((constructor)) static void lbp_jobmods_register_ctor(void)")
    L.append("{")
    L.append("    lbp_jobmods_register_all();")
    L.append("}")
    L.append("")
    os.makedirs(os.path.dirname(REGISTER_C), exist_ok=True)
    with open(REGISTER_C, "w") as f:
        f.write("\n".join(L))
    print(f"Wrote {REGISTER_C} ({len(manifest)} modules, image ids "
          f"{JOBMOD_IMAGE_BASE}..{JOBMOD_IMAGE_BASE + len(manifest) - 1})")

if __name__ == "__main__":
    main()
