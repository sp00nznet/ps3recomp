#!/usr/bin/env python3
"""build_spu_workloads.py -- lift a title's SPU images and emit a workload registry.

General, title-agnostic replacement for gen_spu_workloads.py (which hard-coded
flOw's paths/glob/symbol names). For a directory of SPU ELFs it will:

  1. Lift each image with spu_lifter.py under a per-image C *symbol prefix*
     (derived from the image filename) so that dozens of images -- each of which
     defines spu_func_* / spu_recomp_register / a static function table -- link
     together without symbol collisions.
  2. Rewrite the two depth-sensitive relative includes the lifter emits
     ("../../runtime/spu/spu_context.h", ".../spu_helpers.h") to bare includes,
     so the lifted tree can live anywhere and is resolved via a build -I path
     (add <repo>/runtime/spu to the target's include dirs).
  3. Emit a single registration C file that registers every image with the
     runtime workload registry by FNV-1a-64 content fingerprint, under a distinct
     image id, and (optionally) auto-registers via a constructor.

cellSpurs AddWorkload / CreateTask / RunJobChain call spu_workload_dispatch(),
which fingerprints the guest image the game hands to SPURS, finds the lifted
entry registered here, loads it into a fresh local store and runs it. The
fingerprint computed here is byte-identical to runtime/spu/spu_workload.c's
spu_workload_fingerprint() over spu_elf_image_size() bytes, so registrations and
the images the title passes to cellSpurs line up.

Example (LittleBigPlanet, 23 images):
  python tools/build_spu_workloads.py \
      --images lbp_spu --lifted lbp_spu/lifted \
      --out lbp/gen/spu_workloads.c \
      --register-fn lbp_spu_register_all --constructor --title lbp
"""
import argparse, glob, os, re, subprocess, sys

# ---- SPU ELF sizing + fingerprint (must match runtime/spu/spu_workload.c) ----

def be16(b, o): return (b[o] << 8) | b[o + 1]
def be32(b, o): return (b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]

def img_size(b):
    """On-disk extent spanning the section-header table and all PT_LOAD /
    non-NOBITS section content -- matches spu_elf_image_size()."""
    e_phoff = be32(b, 0x1C); e_shoff = be32(b, 0x20)
    e_phentsize = be16(b, 0x2A); e_phnum = be16(b, 0x2C)
    e_shentsize = be16(b, 0x2E); e_shnum = be16(b, 0x30)
    end = e_shoff + e_shnum * e_shentsize
    for k in range(e_phnum):
        po = e_phoff + k * e_phentsize
        if po + 0x14 > len(b): break
        end = max(end, be32(b, po + 0x04) + be32(b, po + 0x10))
    for k in range(e_shnum):
        so = e_shoff + k * e_shentsize
        if so + 0x18 > len(b): break
        if be32(b, so + 0x04) != 8:  # not SHT_NOBITS
            end = max(end, be32(b, so + 0x10) + be32(b, so + 0x14))
    return min(end, len(b))

def fnv1a64(b):
    h = 1469598103934665603
    for x in b:
        h = ((h ^ x) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h

def is_spu_elf(b):
    return (len(b) >= 0x34 and b[0:4] == b"\x7fELF"
            and b[4] == 1 and b[5] == 2  # ELFCLASS32, ELFDATA2MSB
            and be16(b, 0x12) == 0x17)   # EM_SPU

def sanitize(name):
    s = re.sub(r"[^0-9A-Za-z_]", "_", name)
    if not s or s[0].isdigit():
        s = "img_" + s
    return s

# ---- include rewrite: make the lifted tree location-independent --------------

_INC_RE = re.compile(r'#include\s+"(?:\.\./)+runtime/spu/([A-Za-z0-9_]+\.h)"')

def debase_includes(path):
    try:
        txt = open(path, encoding="utf-8", errors="ignore").read()
    except OSError:
        return
    new = _INC_RE.sub(r'#include "\1"', txt)
    if new != txt:
        open(path, "w", encoding="utf-8", newline="\n").write(new)

# ---- main --------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--images", required=True, help="dir containing SPU ELFs (*.elf)")
    ap.add_argument("--lifted", required=True, help="output dir for per-image lifted C (one subdir per image)")
    ap.add_argument("--out", required=True, help="output registration C file")
    ap.add_argument("--register-fn", default="spu_register_all",
                    help="name of the emitted registration function")
    ap.add_argument("--title", default="spu", help="label used in comments")
    ap.add_argument("--lifter", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "spu_lifter.py"))
    ap.add_argument("--constructor", action="store_true",
                    help="also emit an __attribute__((constructor)) that calls the register fn at startup")
    ap.add_argument("--relift", action="store_true", help="re-lift even if a prior lift exists")
    ap.add_argument("--skip-lift", action="store_true",
                    help="do not run the lifter; assume --lifted already populated (still fixes includes + emits registry)")
    args = ap.parse_args()

    elfs = sorted(glob.glob(os.path.join(args.images, "*.elf")))
    if not elfs:
        sys.exit(f"[build_spu_workloads] no *.elf in {args.images}")

    imgs = []  # (img, prefix, fingerprint, e_entry)
    for e in elfs:
        b = open(e, "rb").read()
        if not is_spu_elf(b):
            print(f"[build_spu_workloads] skip non-SPU ELF: {e}")
            continue
        img = os.path.splitext(os.path.basename(e))[0]
        prefix = sanitize(img) + "_"
        outdir = os.path.join(args.lifted, img)
        srcc = os.path.join(outdir, "spu_recomp.c")

        if not args.skip_lift and (args.relift or not os.path.exists(srcc)):
            os.makedirs(outdir, exist_ok=True)
            cmd = [sys.executable, args.lifter, e, "--auto-functions", e,
                   "--symbol-prefix", prefix, "-o", outdir]
            print(f"[build_spu_workloads] LIFT {img}  (prefix {prefix})")
            if subprocess.run(cmd).returncode != 0:
                sys.exit(f"[build_spu_workloads] lift FAILED for {img}")

        # Make the lifted headers/sources location-independent.
        debase_includes(os.path.join(outdir, "spu_recomp.h"))
        debase_includes(srcc)

        sz = img_size(b)
        e_entry = be32(b, 0x18)
        imgs.append((img, prefix, fnv1a64(b[:sz]), e_entry))

    if not imgs:
        sys.exit("[build_spu_workloads] no SPU images lifted")

    # Verify each registered entry symbol actually exists in the lifted header.
    for img, prefix, fp, ent in imgs:
        hdr = os.path.join(args.lifted, img, "spu_recomp.h")
        sym = f"{prefix}spu_func_{ent:08X}"
        if os.path.exists(hdr) and f"{sym}(" not in open(hdr, encoding="utf-8", errors="ignore").read():
            print(f"[build_spu_workloads] WARNING: entry symbol {sym} not found for {img} "
                  f"(e_entry=0x{ent:X}); registration may not link")

    L = []
    L.append(f"/* {os.path.basename(args.out)} -- GENERATED by tools/build_spu_workloads.py.")
    L.append(f" * Registers the {len(imgs)} lifted SPU images of \"{args.title}\" with the workload")
    L.append(" * dispatch registry (by FNV-1a-64 image fingerprint) and populates each image's")
    L.append(" * indirect-branch table under a distinct image id. cellSpurs AddWorkload/")
    L.append(" * CreateTask/RunJobChain call spu_workload_dispatch(), which fingerprints the")
    L.append(" * guest image, finds the lifted entry here, and runs it on a fresh local store.")
    L.append(" * Build note: add <repo>/runtime/spu to this target's include dirs (the lifted")
    L.append(' * sources use bare #include "spu_context.h" etc.). */')
    L.append('#include "spu_workload.h"')
    L.append("")
    L.append("extern void spu_begin_image(int image_id);")
    L.append("")
    L.append("/* per-image extern decls (prefixed entry fn + recomp_register) */")
    for img, prefix, fp, ent in imgs:
        L.append(f"extern void {prefix}spu_func_{ent:08X}(spu_context*);")
        L.append(f"extern void {prefix}spu_recomp_register(void);")
    L.append("")
    L.append(f"void {args.register_fn}(void)")
    L.append("{")
    for i, (img, prefix, fp, ent) in enumerate(imgs, 1):
        L.append(f"    spu_begin_image({i}); {prefix}spu_recomp_register();")
        L.append(f"    spu_workload_register_img(0x{fp:016X}ULL, {prefix}spu_func_{ent:08X}, {i}, \"{img}\");")
    L.append("    spu_begin_image(0);")
    L.append("}")
    if args.constructor:
        L.append("")
        L.append("/* Auto-register at startup (before the game adds any SPURS workload/task). */")
        L.append(f"__attribute__((constructor)) static void {args.register_fn}_ctor(void)")
        L.append("{")
        L.append(f"    {args.register_fn}();")
        L.append("}")
    L.append("")

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    open(args.out, "w", encoding="utf-8", newline="\n").write("\n".join(L))
    print(f"[build_spu_workloads] wrote {args.out} with {len(imgs)} images "
          f"(register fn: {args.register_fn}{', +constructor' if args.constructor else ''})")

if __name__ == "__main__":
    main()
