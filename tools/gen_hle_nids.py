#!/usr/bin/env python3
"""
Generate the HLE NID -> handler registration unit from the /* NID: 0x.. */
annotations in the HLE library sources.

For each annotated function it emits a `ps3_hle_register(nid, "name", name)`
call inside `ppu_hle_register_all()` (the strong override of the weak default
in ppu_hle.cpp). The matching library headers are #included so the symbols are
declared.

Usage:
    python gen_hle_nids.py --out gen/ppu_hle_nids.cpp cellGcmSys [cellSysutil ...]
    (lib names resolve to libs/**/<name>.c and the sibling <name>.h)
"""
import argparse
import glob
import os
import sys
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nid_database import compute_nid

# Non-static, top-level definitions of exported-looking functions. The NID is
# COMPUTED from the name (the firmware-correct algorithm in nid_database.py),
# not read from a /* NID */ comment -- computation matches the real imports far
# better and needs no manual annotation.
DEF_RE = re.compile(
    r"^(?!static)[A-Za-z_][\w \t\*]*?\b(_?(?:cell|sys|sce|_sys)\w+)\s*\([^;]*?\)\s*\{",
    re.M)


def scan(c_path):
    txt = open(c_path, encoding="latin1").read()
    out = []
    for m in DEF_RE.finditer(txt):
        name = m.group(1)
        out.append((compute_nid(name), name))
    return out


def discover_all():
    """Every PRX-style HLE module compiled into the runtime lib.

    The registration table must cover EVERY library the runtime implements:
    an unregistered import silently fakes CELL_OK (see the note below), and a
    hand-passed module list is the single point where a whole library gets
    dropped by accident -- exactly what happened when gen/ppu_hle_nids.cpp was
    regenerated with only cellFs/cellGcmSys/cellSysmodule and LBP then called
    cellSysutilCheckCallback unregistered, faked it, and diverged at boot. So
    `--all` is the canonical invocation; discover the same dirs CMakeLists.txt
    globs into ps3recomp_runtime, and take the cell*/sce*/sys* file names."""
    dirs = ["system", "filesystem", "input", "audio", "video", "network",
            "spurs", "sync", "codec", "font", "misc", "hardware"]
    names = set()
    for d in dirs:
        for p in glob.glob(os.path.join(ROOT, "libs", d, "*.c")):
            n = os.path.splitext(os.path.basename(p))[0]
            if re.match(r"^(cell|sce|sys)", n) and not re.match(r"^(test_|validate_)", n):
                names.add(n)
    return sorted(names)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("libs", nargs="*",
                    help="Module (file) names to register; omit with --all.")
    ap.add_argument("--all", action="store_true",
                    help="Register EVERY PRX module compiled into the runtime "
                         "(the canonical invocation -- can't silently drop a "
                         "library the way a hand-typed list can).")
    ap.add_argument("--out", default="ppu_hle_nids.cpp")
    args = ap.parse_args()

    if args.all:
        args.libs = discover_all() + list(args.libs)
    if not args.libs:
        ap.error("no modules given; pass module names or --all")

    regs, seen, nlibs, missing = [], set(), 0, []
    for lib in args.libs:
        cs = glob.glob(os.path.join(ROOT, "libs", "**", lib + ".c"), recursive=True)
        if not cs:
            missing.append(lib); continue
        nlibs += 1
        for nid, name in scan(cs[0]):
            if nid in seen:
                continue
            seen.add(nid)
            regs.append((nid, name))

    # A name that resolves to no .c file used to print a warning and carry on.
    # That is the worst possible outcome: every NID in that module stays
    # unregistered, so the dispatcher's unresolved path answers CELL_OK with
    # untouched out-params and the title runs on fabricated success. It is also
    # silent -- the warning scrolls past in the build noise. The trap is that
    # these are PRX names, while the files are named after the LIBRARY:
    # `sys_io` is cellMouse.c + cellKb.c, `sys_fs` is cellFs.c, and so on. LBP
    # shipped six such names (sys_io, sys_fs, sys_net, sceNp2,
    # cellSysutilAvconfExt, cellDiscGame) and silently faked all of them --
    # cellMouseInit/cellMouseGetInfo/cellKbInit were fully implemented and never
    # registered. Fail loudly instead, and suggest the file that probably meant.
    if missing:
        have = sorted(os.path.splitext(os.path.basename(p))[0]
                      for p in glob.glob(os.path.join(ROOT, "libs", "**", "*.c"),
                                         recursive=True))
        print("error: these module names match no libs/**/<name>.c, so every NID "
              "they export would go unregistered and silently fake CELL_OK:",
              file=sys.stderr)
        for lib in missing:
            near = [h for h in have if lib.lower() in h.lower()
                    or h.lower() in lib.lower()]
            hint = f"  did you mean: {', '.join(near)}" if near else \
                   "  no similar file -- implement it or drop it from the list"
            print(f"  {lib}.c NOT FOUND\n{hint}", file=sys.stderr)
        sys.exit(1)

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w") as f:
        f.write("/* Auto-generated by gen_hle_nids.py -- do not edit. */\n")
        f.write('extern "C" void ps3_hle_register(unsigned int nid, const char* name, void* handler);\n')
        # Forward-declare every handler ourselves (extern "C", all libs/*.c are C)
        # instead of #including module headers: some functions are defined in the
        # .c but not declared in the .h, and we only need the address for the
        # registration table. This keeps the unit self-contained.
        f.write("extern \"C\" {\n")
        for _nid, name in regs:
            f.write(f"    void {name}(void);\n")
        f.write("}\n")
        f.write('extern "C" void ppu_hle_register_all(void) {\n')
        for nid, name in regs:
            f.write(f'    ps3_hle_register(0x{nid:08X}u, "{name}", (void*){name});\n')
        f.write("}\n")
    print(f"wrote {args.out}: {len(regs)} NID handlers from {nlibs} module(s)")


if __name__ == "__main__":
    main()
