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

Example:
  python tools/build_spu_workloads.py \
      --images <port>/spu/images --lifted <port>/spu \
      --out <port>/spu/spu_workloads.c \
      --register-fn port_spu_register_all --constructor --title <port>
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

def text_fp(b):
    """Fingerprint of the executable PT_LOAD's file bytes -- what lands in local
    store. A raw SPU whose program the PPU streams in by proxy DMA never hands
    the runtime the ELF, only these bytes (runtime/spu/spu_raw.c)."""
    phoff, phes, phn = be32(b, 0x1C), be16(b, 0x2A), be16(b, 0x2C)
    for i in range(phn):
        o = phoff + i * phes
        if be32(b, o) == 1 and be32(b, o + 0x18) & 1:
            off, fsz = be32(b, o + 4), be32(b, o + 0x10)
            return fnv1a64(b[off:off + fsz])
    return None


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
    inputs = ap.add_mutually_exclusive_group(required=True)
    inputs.add_argument("--input", action="append", help="Discover every embedded SPU ELF in supplied files (repeatable)")
    inputs.add_argument("--images", help="dir containing SPU ELFs (*.elf)")
    ap.add_argument("--lifted", required=True, help="output dir for per-image lifted C (one subdir per image)")
    ap.add_argument("--out", required=True, help="output registration C file")
    ap.add_argument("--register-fn", default="spu_register_all",
                    help="name of the emitted registration function")
    ap.add_argument("--title", default="spu", help="label used in comments")
    ap.add_argument("--lifter", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "spu_lifter.py"))
    ap.add_argument("--extra-funcs", action="append", default=[],
                    metavar="IMAGE=ADDR[,ADDR...]",
                    help="Extra function entry addresses for one image, passed "
                         "through to spu_lifter --extra-funcs. Repeatable. An SPU "
                         "image entered by an INTERRUPT VECTOR needs this: the "
                         "handler is not reachable from the entry point by any "
                         "branch, so --auto-functions cannot see it, and dispatch "
                         "to it lands on address 0 at runtime. "
                         "e.g. --extra-funcs pm_wwsjob=0xA2C")
    ap.add_argument("--constructor", action="store_true",
                    help="also emit an __attribute__((constructor)) that calls the register fn at startup")
    ap.add_argument("--relift", action="store_true", help="re-lift even if a prior lift exists")
    ap.add_argument("--skip-lift", action="store_true",
                    help="do not run the lifter; assume --lifted already populated (still fixes includes + emits registry)")
    ap.add_argument("--allow-partial", action="store_true",
                    help="With --input, emit supported registrations despite incomplete coverage")
    ap.add_argument("--merge-chunks", action="store_true",
                    help="Pass --merge-chunks to spu_lifter.py: each run of chunks from one "
                         "call target to the next becomes one C function, so branches "
                         "inside it are gotos instead of trampoline round trips")
    ap.add_argument("--handled", metavar="JSON",
                    help="With --input: {sha256: reason} for discovered programs another path "
                         "already provides (an existing registration, an interpreter route). They "
                         "are not lifted here, count as covered, and the reason is recorded")
    ap.add_argument("--image-ids", metavar="JSON",
                    help="With --input: {sha256: image_id} pinning every registered program's "
                         "image id. A registered program missing from the map is an error, so "
                         "ids are reviewed and stable instead of content-derived")
    args = ap.parse_args()
    if args.input:
        from spu_program_pipeline import build_programs
        sys.exit(build_programs(args, write_registry))

    elfs = sorted(glob.glob(os.path.join(args.images, "*.elf")))
    if not elfs:
        sys.exit(f"[build_spu_workloads] no *.elf in {args.images}")

    extra_funcs = {}
    for spec in args.extra_funcs:
        if "=" not in spec:
            sys.exit(f"[build_spu_workloads] --extra-funcs wants IMAGE=ADDR, got {spec!r}")
        k, v = spec.split("=", 1)
        extra_funcs[k.strip()] = v.strip()

    imgs = []  # (img, prefix, fingerprint, e_entry)
    text_fps = {}  # img -> executable-segment fingerprint
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
            if img in extra_funcs:
                cmd += ["--extra-funcs", extra_funcs[img]]
            if args.merge_chunks:
                cmd += ["--merge-chunks"]
                print(f"[build_spu_workloads] {img}: extra entries {extra_funcs[img]}")
            print(f"[build_spu_workloads] LIFT {img}  (prefix {prefix})")
            if subprocess.run(cmd).returncode != 0:
                sys.exit(f"[build_spu_workloads] lift FAILED for {img}")

        # Make the lifted headers/sources location-independent.
        debase_includes(os.path.join(outdir, "spu_recomp.h"))
        debase_includes(srcc)

        sz = img_size(b)
        e_entry = be32(b, 0x18)
        imgs.append((img, prefix, fnv1a64(b[:sz]), e_entry))
        text_fps[img] = text_fp(b)

    if not imgs:
        sys.exit("[build_spu_workloads] no SPU images lifted")

    # Verify each registered entry symbol actually exists in the lifted header.
    for img, prefix, fp, ent in imgs:
        hdr = os.path.join(args.lifted, img, "spu_recomp.h")
        sym = f"{prefix}spu_func_{ent:08X}"
        if os.path.exists(hdr) and f"{sym}(" not in open(hdr, encoding="utf-8", errors="ignore").read():
            print(f"[build_spu_workloads] WARNING: entry symbol {sym} not found for {img} "
                  f"(e_entry=0x{ent:X}); registration may not link")

    write_registry(args, imgs, text_fps)


def write_registry(args, imgs, text_fps, image_ids=None):
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
    for index, (img, prefix, fp, ent) in enumerate(imgs, 1):
        i = image_ids[img] if image_ids else index
        L.append(f"    spu_begin_image({i}); {prefix}spu_recomp_register();")
        L.append(f"    spu_workload_register_img(0x{fp:016X}ULL, {prefix}spu_func_{ent:08X}, {i}, \"{img}\");")
        if text_fps.get(img):  # same entry, keyed by the text segment (raw SPU proxy-DMA load)
            L.append(f"    spu_workload_register_img(0x{text_fps[img]:016X}ULL, {prefix}spu_func_{ent:08X}, {i}, \"{img}.text\");")
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
