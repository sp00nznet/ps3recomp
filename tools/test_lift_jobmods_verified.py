#!/usr/bin/env python3
"""lift_jobmods.py --engine-elf: modules embedded in an engine ELF register as
verified streamed regions whose span is the lifted code only.

A synthetic big-endian ELF64 holds two hand-assembled job modules (and a
byte-identical copy of the first at another EA), each with a data tail after
its code. Checks that the modules are found at their EAs, that code_end stops
before the data tail (and grows to a manual-link return target reached only
through `bi $r0`), that the registration TU registers span == code_end - 0x4000
with exactly the code bytes, and that the default --jobmods mode still emits
signature registration. On POSIX it then links the registration TU and the
lifted C against the SPU runtime: a GET of a job's own data above its code
keeps the module resident, while a GET of another module clears it.
No game data is involved."""
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools/lift_jobmods.py"
BASE = 0x4000

# SPU encodings used below.
def ila(rt, imm): return (0x21 << 25) | ((imm & 0x3FFFF) << 7) | rt
def il(rt, imm):  return (0x081 << 23) | ((imm & 0xFFFF) << 7) | rt
def ai(rt, ra, imm): return (0x1C << 24) | ((imm & 0x3FF) << 14) | (ra << 7) | rt
def a(rt, ra, rb): return (0x0C0 << 21) | (rb << 14) | (ra << 7) | rt
def br(pc, tgt):  return (0x064 << 23) | ((((tgt - pc) >> 2) & 0xFFFF) << 7)
def brsl(pc, tgt, rt=0): return (0x066 << 23) | ((((tgt - pc) >> 2) & 0xFFFF) << 7) | rt
BI_R0, LNOP, STOP = 0x35000000, 0x00200000, 0x00000000


def module(words, tag, entry_off=0x34):
    """`4 ila | entry | size | 0 | 0 | C0DEC0DE | 0 | load base | 0` + words."""
    body = b"".join(struct.pack(">I", w) for w in words)
    size = 0x30 + len(body)
    hdr = b"".join(struct.pack(">I", ila(3, tag + i)) for i in range(4))
    hdr += struct.pack(">IIIIIIII", entry_off, size + 32, 0, 0, 0xC0DEC0DE, 0, BASE, 0)
    return hdr + body


# Module A: entry calls f and stops; data tail decodes as a branch.
A = module([
    BI_R0,                          # 0x30  stub reached only by a function pointer
    il(3, 1),                       # 0x34  entry
    brsl(0x38, 0x40),               # 0x38
    STOP,                           # 0x3C
    ai(3, 3, 1),                    # 0x40  f
    BI_R0,                          # 0x44
    br(0x48, 0x4C), 0x12345678, 0xDEADBEEF, 0, 0x01020304, 0, 0, 0,   # 0x48 data
], tag=0x10)
A_CODE_END = BASE + 0x48

# Module B: entry builds its return address by hand (ila $r0 / a $r0,$r0,$r126),
# so 0x50 is reached only through `bi $r0`. The data tail repeats the pattern;
# being unreachable, it must not extend the code.
B = module([
    BI_R0,                          # 0x30
    ila(0, 0x50),                   # 0x34  entry
    a(0, 0, 126),                   # 0x38
    br(0x3C, 0x44),                 # 0x3C
    LNOP,                           # 0x40
    il(3, 7),                       # 0x44  body
    BI_R0,                          # 0x48
    LNOP,                           # 0x4C
    il(4, 2),                       # 0x50  manual-link return target
    BI_R0,                          # 0x54
    ila(0, 0x70), a(0, 0, 126), 0xCAFEF00D, 0, 0, 0, 0, 0, il(5, 9), BI_R0,   # 0x58 data
], tag=0x20)
B_CODE_END = BASE + 0x58

SEG_VA = 0x00800000
A_EA, B_EA, A2_EA = SEG_VA + 0x100, SEG_VA + 0x400, SEG_VA + 0x700


def engine_elf():
    seg = bytearray(0x800)
    for ea, m in ((A_EA, A), (B_EA, B), (A2_EA, A)):
        seg[ea - SEG_VA:ea - SEG_VA + len(m)] = m
    # A decoy: magic at an aligned header but no load base at +0x28.
    seg[0x620:0x624] = b"\xC0\xDE\xC0\xDE"
    phoff, data_off = 0x40, 0x100
    eh = b"\x7fELF" + bytes([2, 2, 1]) + bytes(9)
    eh += struct.pack(">HHIQQQIHHHHHH", 2, 21, 1, 0, phoff, 0, 0, 0x40, 0x38, 1, 0x40, 0, 0)
    ph = struct.pack(">IIQQQQQQ", 1, 5, data_off, SEG_VA, SEG_VA, len(seg), len(seg), 0x10)
    out = bytearray(eh + ph)
    out += bytes(data_off - len(out)) + seg
    return bytes(out)


failures = []
def check(ok, what):
    print(("PASS " if ok else "FAIL ") + what)
    if not ok:
        failures.append(what)


def run(*argv, env=None):
    r = subprocess.run([sys.executable, str(TOOL), *argv], capture_output=True, text=True,
                       env=env)
    if r.returncode:
        print(r.stdout, r.stderr)
        raise SystemExit("lift_jobmods failed")
    return r


env = {k: v for k, v in os.environ.items() if k != "LBP_JOBMOD_EXTRA_FUNCS"}
with tempfile.TemporaryDirectory(prefix="lift-jobmods-verified-") as tmp:
    tmp = Path(tmp)
    (tmp / "engine.elf").write_bytes(engine_elf())
    run("--engine-elf", str(tmp / "engine.elf"), "--out", str(tmp / "out"),
        "--symbol-prefix", "synth", env=env)
    manifest = json.loads((tmp / "out/jobmods_manifest.json").read_text())
    check([m["eas"] for m in manifest] == [[A_EA, A2_EA], [B_EA]],
          "modules found at their EAs, identical copies share one lift, decoy rejected")
    check([m["ea"] for m in manifest] == [A_EA, B_EA], "manifest records the first EA")
    check([m["code_end"] for m in manifest] == [A_CODE_END, B_CODE_END],
          "code_end excludes the data tail and includes the manual-link target")
    check(all(m["size"] == len(x) for m, x in zip(manifest, (A, B))),
          "module is the initialised image (imageSize - 32)")
    for m in manifest:
        src = (tmp / "out" / m["name"] / "spu_recomp.c").read_text()
        starts = {int(x, 16) for x in re.findall(r"^void \w*spu_func_([0-9A-F]{8})\(spu_context\* ctx\)\s*\{",
                                                  src, re.M)}
        check(starts and max(starts) < m["code_end"], f"{m['name']}: nothing lifted past code_end")
        check(m["entry"] in starts and BASE + 0x30 in starts, f"{m['name']}: entry and +0x30 lifted")
    b_src = (tmp / "out" / manifest[1]["name"] / "spu_recomp.c").read_text()
    check(re.search(r"^void \w*spu_func_00004050\(", b_src, re.M) is not None,
          "manual-link return target lifted inside the span")

    reg = (tmp / "out/jobmods_register.c").read_text()
    calls = re.findall(r"spu_overlay_register_verified_region\(0x([0-9A-F]+)u, 0x4000u, 0x([0-9A-F]+)u, "
                       r"(\d+), &seg, 1\)", reg)
    check(calls == [("%08X" % A_EA, "%X" % (A_CODE_END - BASE), "200"),
                    ("%08X" % A2_EA, "%X" % (A_CODE_END - BASE), "200"),
                    ("%08X" % B_EA, "%X" % (B_CODE_END - BASE), "201")],
          "verified region per EA with span == code_end - 0x4000")
    check("spu_overlay_register_sig" not in reg, "engine mode does not register by signature")
    for img, raw, end in ((200, A, A_CODE_END), (201, B, B_CODE_END)):
        arr = re.search(r"static const uint8_t code_%d\[(\d+)\] = \{([^}]*)\}" % img, reg)
        got = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-F]{2})", arr.group(2))) if arr else b""
        check(got == raw[0x30:end - BASE], f"image {img}: segment bytes are exactly the code")
    check(reg.count("{0x4030u, sizeof code_") == 2, "segment starts after the header")

    # Default mode keeps signature registration and the old manifest shape.
    (tmp / "bins").mkdir()
    for raw in (A, B):
        import hashlib
        e = struct.unpack_from(">I", raw, 0x10)[0]
        (tmp / "bins" / f"jobmod_{hashlib.sha1(raw).hexdigest()[:12]}_e{e:04X}.bin").write_bytes(raw)
    run("--jobmods", str(tmp / "bins"), "--out", str(tmp / "sig"), env=env)
    sig_reg = (tmp / "sig/jobmods_register.c").read_text()
    sig_manifest = json.loads((tmp / "sig/jobmods_manifest.json").read_text())
    check(sig_reg.count("spu_overlay_register_sig(sig, ") == 2 and
          "verified_region" not in sig_reg, "--jobmods still registers by content signature")
    check(all(set(m) == {"name", "prefix", "base", "entry", "entry_off", "sig", "size", "funcs",
                         "unsupported"} for m in sig_manifest), "--jobmods manifest unchanged")
    check(sorted(m["name"] for m in sig_manifest) == sorted(m["name"] for m in manifest),
          "both sources name a module identically")
    both = subprocess.run([sys.executable, str(TOOL), "--jobmods", str(tmp / "bins"),
                           "--engine-elf", str(tmp / "engine.elf"), "--out", str(tmp / "x")],
                          capture_output=True, text=True)
    check(both.returncode != 0, "--jobmods and --engine-elf are mutually exclusive")

    # Residency against the real runtime: link the registration TU + lifted C.
    if os.name == "nt":
        print("SKIP runtime residency: fixture requires the POSIX compiler/linker")
    else:
        spu = ROOT / "runtime/spu"
        driver = tmp / "driver.c"
        driver.write_text(r'''
#include "spu_interp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern void spu_overlay_note_get(spu_context*, uint32_t, const uint8_t*, uint32_t);
extern void (*spu_lookup(uint32_t addr, int image_id))(spu_context*);
extern const unsigned char mod_a[], mod_b[];
extern const unsigned mod_a_len, mod_b_len;
uint8_t* vm_base;
uint32_t ppu_vm_size;
void spu_thread_publish_ctx(uint32_t tid, void* c) { (void)tid; (void)c; abort(); }
void ppu_resv_break_store(uint64_t ea) { (void)ea; abort(); }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; abort(); }
unsigned long long ps3_ms_now(void) { abort(); }
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi;
int g_resv_store_active;
uint32_t g_barrier_sync_watch;
uint32_t g_ydkj_real_spurs_ea, g_ydkj_real_taskset_ea, g_ydkj_real_taskid;
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; abort(); }
void ydkj_wake_all_event_flags(void) { abort(); }
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d) {
    (void)c; (void)a; (void)b; (void)d; abort();
}
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
static int failed;
static void check(int ok, const char* what) { printf("%s %s\n", ok ? "PASS" : "FAIL", what); failed |= !ok; }
int main(void)
{
    static spu_context ctx;
    spu_context_init(&ctx, 0);
    spu_lifted_fn fa = (spu_lifted_fn)spu_lookup(0x4034, 200);
    spu_lifted_fn fb = (spu_lifted_fn)spu_lookup(0x4034, 201);
    check(fa && fb && fa != fb, "both modules registered their entries");
    memcpy(ctx.ls + 0x4000, mod_a, mod_a_len);
    spu_overlay_note_get(&ctx, A_EA, ctx.ls + 0x4000, mod_a_len);
    check(ctx.resident_code[0].image_id == 200, "module A resident after its GET");
    memset(ctx.ls + A_CODE_END, 0x5A, 0x20);
    spu_overlay_note_get(&ctx, 0x00900000u, ctx.ls + A_CODE_END, 0x20);
    check(ctx.resident_code[0].image_id == 200 && ctx.resident_code[0].size == A_CODE_END - 0x4000 &&
          spu_lifted_lookup(&ctx, 0x4034) == fa,
          "job data GET above the code keeps A resident (verified on code bytes only)");
    memcpy(ctx.ls + 0x4000, mod_b, mod_b_len);
    spu_overlay_note_get(&ctx, B_EA, ctx.ls + 0x4000, mod_b_len);
    check(spu_lifted_lookup(&ctx, 0x4034) == fb, "GET of module B replaces A");
    spu_overlay_note_get(&ctx, 0x00900000u, ctx.ls + 0x4040, 0x10);
    check(!ctx.resident_code[0].image_id && spu_lifted_lookup(&ctx, 0x4034) != fb,
          "foreign GET into B's code clears residency");
    memcpy(ctx.ls + 0x4000, mod_a, mod_a_len);
    spu_overlay_note_get(&ctx, A2_EA, ctx.ls + 0x4000, mod_a_len);
    check(spu_lifted_lookup(&ctx, 0x4034) == fa, "identical copy at a second EA dispatches A");
    memcpy(ctx.ls + 0x4000, mod_b, mod_b_len);
    spu_overlay_note_get(&ctx, A_EA, ctx.ls + 0x4000, mod_b_len);
    check(spu_lifted_lookup(&ctx, 0x4034) == NULL, "wrong bytes at A's EA stay interpreted");
    return failed;
}
'''.replace("A_EA", hex(A_EA) + "u").replace("B_EA", hex(B_EA) + "u")
   .replace("A2_EA", hex(A2_EA) + "u").replace("A_CODE_END", hex(A_CODE_END)))
        data = tmp / "modules.c"
        data.write_text("".join(
            f"const unsigned char {n}[] = {{{','.join(str(x) for x in raw)}}};\n"
            f"const unsigned {n}_len = {len(raw)};\n" for n, raw in (("mod_a", A), ("mod_b", B))))
        sources = ["spu_channels.c", "spu_drain.c", "spu_lockstep.c", "spu_coherency.c",
                   "spu_workload.c", "spurs_policy.c", "spurs_job.c", "spu_tsp_weak.c",
                   "spu_vm_pagemap.c", "spurs_policy_blob_weak.c", "spu_interp.c"]
        lifted = [str(tmp / "out" / m["name"] / "spu_recomp.c") for m in manifest]
        binary = tmp / "residency"
        subprocess.run([os.environ.get("CC", "cc"), "-std=gnu17", "-O1", "-UNDEBUG", "-w",
                        "-I" + str(ROOT / "include"), "-I" + str(spu),
                        "-I" + str(ROOT / "runtime/platform"),
                        str(driver), str(data), str(tmp / "out/jobmods_register.c"), *lifted,
                        *[str(spu / s) for s in sources],
                        str(ROOT / "runtime/platform/win32_compat.c"), "-pthread", "-lm",
                        "-o", str(binary)], check=True)
        r = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10, cwd=tmp,
                           env={k: v for k, v in os.environ.items()
                                if not k.startswith(("SPU_", "SPURS_", "YDKJ_", "LBP_"))})
        print(r.stdout.strip())
        if r.stderr.strip():
            print(r.stderr.strip()[-2000:])
        check(r.returncode == 0, "runtime residency with code-only spans")

if failures:
    raise SystemExit("FAILED: " + "; ".join(failures))
print("lift_jobmods verified regions: all checks passed")
