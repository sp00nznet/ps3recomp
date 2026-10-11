#!/usr/bin/env python3
"""lift_firmware_spu.py -- lift the SPU side of a firmware module.

A firmware PRX lifted with lift_firmware_module.py runs its PPU code natively,
but the SPU programs it carries would still run on the SPU interpreter. libsre
is the case that matters: the SPURS kernel is an SPU thread image embedded as
an SPU ELF, and the kernel DMAs further code bodies (the system service, the
taskset policy module) out of libsre's data into local store and branches into
them. This tool lifts both kinds and writes one registration TU:

  * embedded SPU ELFs (found by scanning the relocated module image): their
    functions are registered under their own image id, so an SPU thread whose
    entry point is one of them runs the lift (lv2 group_start looks the entry
    up in the registry);
  * code bodies listed with --body OFF:SIZE[:LSA[:ENTRY]] (offsets into the
    module image; default LS 0xA00, where SPURS loads modules): wrapped into an
    SPU ELF at that LS address, lifted, and registered as a streamed code
    region at the module's EA, so a GET of that body into local store makes the
    lift resident (spu_overlay_register_region).

The module's EAs come from its lift (--base, as given to lift_firmware_module),
so the registration is valid for that build of the module.

Usage:
  lift_firmware_spu.py --module-dir LLE/libsre --base 0x10080000 --name libsre \\
      --first-image-id 100 --body 0x21480:0x2200:0xA00:0xA00:0x2110 \\
      --body 0x23680:0x1E40:0xA00:0xA00:0x1C80 --out DIR
"""
import argparse
import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
EM_SPU = 23


def be16(b, o): return struct.unpack_from(">H", b, o)[0]
def be32(b, o): return struct.unpack_from(">I", b, o)[0]


def spu_elfs(img):
    """(offset, size, entry) of each SPU ELF embedded in the image."""
    out, off = [], img.find(b"\x7FELF")
    while off != -1:
        b = img[off:]
        if len(b) >= 0x34 and b[4] == 1 and b[5] == 2 and be16(b, 18) == EM_SPU:
            phoff, shoff = be32(b, 0x1C), be32(b, 0x20)
            phnum, shnum, shent = be16(b, 0x2C), be16(b, 0x30), be16(b, 0x2E)
            end = shoff + shnum * shent
            for i in range(phnum):
                p = phoff + i * 32
                end = max(end, p + 32, be32(b, p + 4) + be32(b, p + 16))
            out.append((off, end, be32(b, 0x18)))
            off = img.find(b"\x7FELF", off + end)
        else:
            off = img.find(b"\x7FELF", off + 1)
    return out


def code_pointers(elf):
    """Words in the image's loaded segments that point at non-zero code inside
    them: address tables a loaded module reads to call back in (the SPURS
    kernel keeps its entry points at LS 0x1E0). --auto-functions only follows
    branches, so these are seeded as extra entries."""
    segs = []
    phoff, phnum = be32(elf, 0x1C), be16(elf, 0x2C)
    for i in range(phnum):
        p = phoff + i * 32
        if be32(elf, p) == 1:
            segs.append((be32(elf, p + 4), be32(elf, p + 8), be32(elf, p + 16)))
    out = set()
    for off, va, fs in segs:
        for k in range(0, fs - 3, 4):
            w = be32(elf, off + k)
            if w >= 0x200 and w % 4 == 0 and va <= w < va + fs and be32(elf, off + (w - va)):
                out.add(w)
    return sorted(out)


def stack_reset_entries(src, ls_image, ls_base):
    """Lifted functions whose first instruction loads a constant into the stack
    pointer (ila $r1, imm): they abandon every SPU frame above them (the SPURS
    kernel's exit-to-kernel and entry points), so the host frames of the lifted
    callers can be abandoned too. Registered with the runtime, a branch into one
    unwinds to the thread's driver instead of nesting a host call per workload
    switch -- without it a lifted kernel recurses once per module run until the
    host-depth guard halts the SPU."""
    out = []
    for m in re.finditer(r"spu_func_([0-9A-F]{8})\(spu_context\* ctx\) \{", open(src).read()):
        addr = int(m.group(1), 16)
        o = addr - ls_base
        if 0 <= o and o + 4 <= len(ls_image):
            w = be32(ls_image, o)
            if (w >> 25) == 0x21 and (w & 0x7F) == 1:      # ila $r1, imm18
                out.append(addr)
    return out


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit("failed: " + " ".join(cmd[:3]))
    return r.stdout


def fix_includes(src):
    s = open(src).read()
    s = s.replace('#include "../../runtime/spu/spu_helpers.h"', '#include "spu_helpers.h"')
    s = s.replace('#include "../../runtime/spu/spu_context.h"', '#include "spu_context.h"')
    open(src, "w").write(s)
    return s.count("(spu_context* ctx) {")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--module-dir", required=True, help="lift_firmware_module.py output for the module")
    ap.add_argument("--base", required=True, type=lambda x: int(x, 0))
    ap.add_argument("--name", required=True)
    ap.add_argument("--first-image-id", type=int, default=100)
    ap.add_argument("--body", action="append", default=[],
                    help="OFF:SIZE[:LSA[:ENTRY[:CODELEN]]] body in the module image (hex). "
                         "CODELEN bounds the code (the rest is the body's data): it is the "
                         "lift's code end and the registered region, so the body's own data "
                         "DMAs do not evict its lift")
    ap.add_argument("--out", required=True)
    ap.add_argument("--trace", action="store_true",
                    help="lift with per-instruction trace calls (diff against SPU_TRACE_INTERP)")
    a = ap.parse_args()

    meta = os.path.join(a.module_dir, "meta")
    img = open(os.path.join(meta, a.name + "_image.bin"), "rb").read()
    os.makedirs(a.out, exist_ok=True)
    py = sys.executable
    regs = []                                   # (prefix, image id, region or None)
    image_id = a.first_image_id

    for off, size, entry in spu_elfs(img):
        name = "%s_spu_%05X" % (a.name, off)
        d = os.path.join(a.out, name); os.makedirs(d, exist_ok=True)
        elf = os.path.join(d, name + ".elf")
        open(elf, "wb").write(img[off:off + size])
        seeds = code_pointers(img[off:off + size])
        cmd = [py, os.path.join(HERE, "spu_lifter.py"), "--auto-functions", elf,
               "--symbol-prefix", name + "_", "--merge-chunks", "-o", d] + (["--trace"] if a.trace else [])
        if seeds:
            cmd += ["--extra-funcs", ",".join("0x%X" % x for x in seeds)]
        run(cmd)
        print("%s: seeded %s" % (name, ", ".join("0x%X" % x for x in seeds)))
        n = fix_includes(os.path.join(d, "spu_recomp.c"))
        e = img[off:off + size]
        p = be32(e, 0x1C)
        resets = stack_reset_entries(os.path.join(d, "spu_recomp.c"),
                                     e[be32(e, p + 4):be32(e, p + 4) + be32(e, p + 16)], be32(e, p + 8))
        print("%s: SPU ELF at +0x%X (EA 0x%08X), entry 0x%X, %d functions, image %d, stack resets %s"
              % (name, off, a.base + off, entry, n, image_id, ", ".join("0x%X" % x for x in resets)))
        regs.append((name, image_id, None, resets)); image_id += 1

    for spec in a.body:
        f = [int(x, 16) for x in spec.split(":")]
        off, size = f[0], f[1]
        lsa = f[2] if len(f) > 2 else 0xA00
        entry = f[3] if len(f) > 3 else lsa
        codelen = f[4] if len(f) > 4 else size
        name = "%s_body_%05X" % (a.name, off)
        d = os.path.join(a.out, name); os.makedirs(d, exist_ok=True)
        raw = os.path.join(d, name + ".bin")
        open(raw, "wb").write(img[off:off + size])
        elf = os.path.join(d, name + ".elf")
        run([py, os.path.join(HERE, "wrap_spu_elf.py"), raw, "--entry", hex(entry),
             "--base", hex(lsa), "--out", elf])
        run([py, os.path.join(HERE, "spu_lifter.py"), "--auto-functions", elf,
             "--base", hex(lsa), "--symbol-prefix", name + "_",
             "--code-end", hex(lsa + codelen), "--merge-chunks", "-o", d] + (["--trace"] if a.trace else []))
        n = fix_includes(os.path.join(d, "spu_recomp.c"))
        resets = stack_reset_entries(os.path.join(d, "spu_recomp.c"), img[off:off + size], lsa)
        print("%s: body at +0x%X (EA 0x%08X) size 0x%X -> LS 0x%X, %d functions, image %d, stack resets %s"
              % (name, off, a.base + off, size, lsa, n, image_id, ", ".join("0x%X" % x for x in resets)))
        regs.append((name, image_id, (a.base + off, codelen), resets)); image_id += 1

    lines = ["/* %s_spu_register.c -- GENERATED by ps3recomp/tools/lift_firmware_spu.py." % a.name,
             " * Registers the lifted SPU side of %s (module base 0x%08X): its SPU ELF" % (a.name, a.base),
             " * images by function, and the code bodies it streams into local store as",
             " * code regions at their EAs. */",
             "#include <stdint.h>", "#include <stdio.h>", "#include <string.h>", "#include <stdlib.h>", "",
             "/* FW_SPU_INTERP=<id>[,<id>...] (or 'all'): leave those images unregistered, so",
             " * they run on the SPU interpreter -- to bisect a lift against the interpreter. */",
             "static int fw_spu_interp(int id)",
             "{",
             "    const char* e = getenv(\"FW_SPU_INTERP\");",
             "    if (!e) return 0;",
             "    if (!strcmp(e, \"all\")) return 1;",
             "    for (const char* p = e; *p; ) {",
             "        if (atoi(p) == id) return 1;",
             "        while (*p && *p != ',') p++;",
             "        if (*p) p++;",
             "    }",
             "    return 0;",
             "}", "",
             "extern void spu_begin_image(int image_id);",
             "extern void spu_overlay_register_region(uint32_t content_ea, uint32_t span, int image_id);",
             "extern void spu_register_stack_reset_entry(uint32_t entry, int image_id);"]
    for name, _, _, _ in regs:
        lines.append("extern void %s_spu_recomp_register(void);" % name)
    lines += ["", "static void %s_spu_register(void)" % a.name, "{"]
    for name, iid, region, resets in regs:
        lines.append("    if (!fw_spu_interp(%d)) {" % iid)
        lines.append("        spu_begin_image(%d); %s_spu_recomp_register();" % (iid, name))
        if region:
            lines.append("        spu_overlay_register_region(0x%08Xu, 0x%Xu, %d);" % (region[0], region[1], iid))
        for r in resets:
            lines.append("        spu_register_stack_reset_entry(0x%Xu, %d);" % (r, iid))
        lines.append("    } else fprintf(stderr, \"[fw-spu] image %d (%s) left to the interpreter\\n\");" % (iid, name))
    lines += ["    spu_begin_image(0);", "}", "",
              "__attribute__((constructor)) static void %s_spu_register_ctor(void)" % a.name,
              "{", "    %s_spu_register();" % a.name, "}", ""]
    reg = os.path.join(a.out, "%s_spu_register.c" % a.name)
    open(reg, "w").write("\n".join(lines))
    print("wrote", reg)


if __name__ == "__main__":
    main()
