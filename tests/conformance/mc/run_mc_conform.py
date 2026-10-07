#!/usr/bin/env python3
"""Multicore conformance: RPCS3 (oracle) vs ps3recomp on SPU thread groups,
MFC DMA, atomics and PPU<->SPU mailboxes/signals.

  1. gen_mc_conform.py writes mc_conform.elf (PPU harness + embedded SPU images)
  2. RPCS3 runs it headless (same oracle config as the PPU suite)
  3. ps3recomp lifts the PPU side; the SPU images run on its SPU interpreter
     as resident, concurrent threads with blocking channels
  4. the MCCONF BEGIN..END transcripts must be identical

usage: run_mc_conform.py --work DIR [--suite mc|spurs] [--rpcs3 PATH] [--skip-oracle] [--skip-build]
  --suite spurs runs gen_spurs_conform.py instead (SPURS through firmware imports).
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(HERE, "..", "ppu"))
from run_ppu_conform import DEFAULT_RPCS3, run_oracle, sh  # noqa: E402
sys.path.insert(0, os.path.join(HERE, ".."))
import sanitize  # noqa: E402

# The faithful SPU thread model: interpret images that have no lifted
# registration, on their own host threads, with channel reads that block.
SPU_ENV = {"RD_SPU_INTERP": "1", "RD_SPU_INTERP_ASYNC": "1", "SPU_CH_BLOCK": "1",
           "PS3_VERBOSE": "0"}


SUITES = {"mc": ("gen_mc_conform.py", "mc_conform", b"MCCONF"),
          "spurs": ("gen_spurs_conform.py", "spurs_conform", b"SPURSCONF"),
          "mcx": ("gen_mcx_conform.py", "mcx_conform", b"MCXCONF"),
          "spu": (os.path.join("..", "spu", "gen_spu_conform.py"), "spu_conform", b"SPUCONF")}

# Per-suite oracle settings on top of the PPU suite's. The SPU instruction
# suite compares against RPCS3's reference SPU interpreter with exact
# extended-range single precision, not its default LLVM recompiler.
ORACLE_EXTRA = {"spu": {"SPU Decoder": "Interpreter (static)", "SPU XFloat Accuracy": "Accurate"}}


def cut(path, tag):
    m = re.search(tag + rb" BEGIN.*?" + tag + rb" END", open(path, "rb").read(), re.S)
    if not m:
        return None
    # the harness prints only hex lines; runtime logging on stdout is dropped
    return [l for l in m.group(0).decode("latin-1").splitlines()
            if l.startswith(tag.decode()) or (l and all(c in "0123456789abcdef" for c in l))]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", required=True)
    ap.add_argument("--suite", choices=sorted(SUITES), default="mc")
    ap.add_argument("--rpcs3", default=DEFAULT_RPCS3)
    ap.add_argument("--skip-oracle", action="store_true")
    ap.add_argument("--skip-build", action="store_true")
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 8)
    ap.add_argument("--sanitize", choices=sanitize.KINDS,
                    help="build the runtime with ThreadSanitizer / AddressSanitizer "
                         "(separate build dir); any report fails the run")
    ap.add_argument("--lifted", action="store_true",
                    help="lift the suite's SPU images (tools/build_spu_workloads.py) and run "
                         "them as lifted code, not on the interpreter")
    ap.add_argument("--repeat", type=int, default=1,
                    help="run ours N times; every run must match the oracle")
    a = ap.parse_args()
    work = os.path.abspath(a.work)
    os.makedirs(work, exist_ok=True)
    gen, stem, tag = SUITES[a.suite]
    elf = os.path.join(work, stem + ".elf")
    py = sys.executable

    if sh([py, os.path.join(HERE, gen), "-o", elf]).returncode:
        sys.exit(2)
    if not a.skip_oracle:
        run_oracle(a.rpcs3, elf, work, a.timeout, marker=tag + b" END", extra=ORACLE_EXTRA.get(a.suite))

    rec = os.path.join(work, "recompiled")
    bld = sanitize.build_dir(os.path.join(work, "build_lifted" if a.lifted else "build"), a.sanitize)
    lift_dir = os.path.join(work, "spu_lift")
    if not a.skip_build:
        load = os.path.join(work, "load")
        if sh([py, os.path.join(ROOT, "tools", "ppu_loader.py"), elf, "-o", load],
              stdout=subprocess.DEVNULL).returncode:
            sys.exit(2)
        if sh([py, os.path.join(ROOT, "tools", "ppu_lifter.py"), elf,
               "--functions", os.path.join(load, stem + ".functions.json"),
               "--hle-stubs", os.path.join(load, stem + ".imports.json"),
               "-o", rec], stdout=subprocess.DEVNULL).returncode:
            sys.exit(2)
        if a.lifted:
            if sh([py, os.path.join(ROOT, "tools", "build_spu_workloads.py"),
                   "--images", os.path.join(work, stem + "_spu"), "--lifted", lift_dir,
                   "--out", os.path.join(lift_dir, stem + "_spu_register.c"),
                   "--register-fn", stem + "_spu_register_all", "--constructor", "--relift",
                   "--eboot", elf, "--lift-arg=--merge-chunks", "--lift-arg=--return-entries"],
                  stdout=subprocess.DEVNULL).returncode:
                sys.exit(2)
        if not os.path.exists(os.path.join(bld, "build.ninja")):
            san = sanitize.cmake_args(a.sanitize)
            if sh(["cmake", "-S", os.path.join(ROOT, "templates", "project"), "-B", bld, "-G", "Ninja",
                   "-DCMAKE_BUILD_TYPE=Release", "-DRECOMP_DIR=" + rec,
                   "-DFIRMWARE_SPU_DIR=" + (lift_dir if a.lifted else "")] + san,
                  stdout=subprocess.DEVNULL).returncode:
                sys.exit(2)
        r = sh(["cmake", "--build", bld, "-j", str(a.jobs)], capture_output=True, text=True)
        if r.returncode:
            print("\n".join(l for l in (r.stdout + r.stderr).split("\n") if " error" in l or "Error" in l)[:4000])
            sys.exit(2)

    want = cut(os.path.join(work, "oracle.txt"), tag)
    if want is None:
        print("oracle transcript has no %s block" % tag.decode()); sys.exit(2)
    # Records the architecture decides where the oracle is known to deviate
    # (written by the generator; see gen_mcx_conform.x1_spec).
    spec_path = os.path.join(work, stem + ".spec.json")
    if os.path.exists(spec_path):
        import json
        spec = json.load(open(spec_path))
        for k, v in spec["by_spec"].items():
            want[int(k) + 1] = v                     # +1: the BEGIN line
        print("%d record(s) by spec, not oracle: %s" % (len(spec["by_spec"]), spec["why"]))
    env = sanitize.env(dict(os.environ, **SPU_ENV), a.sanitize)
    failed = 0
    for k in range(a.repeat):
        sfx = "" if a.repeat == 1 else str(k)
        ours = os.path.join(work, "ours%s.txt" % sfx)
        errf = os.path.join(work, "ours%s.stderr.txt" % sfx)
        with open(ours, "wb") as fo, open(errf, "wb") as fe:
            try:
                subprocess.run([os.path.join(bld, "MyGameRecomp"), elf], stdout=fo, stderr=fe,
                               timeout=a.timeout, env=env, cwd=work)
            except subprocess.TimeoutExpired:
                print("run %d: TIMEOUT" % k)
        got = cut(ours, tag)
        if got is None:
            print("run %d: our transcript has no %s block (see %s)" % (k, tag.decode(), errf))
            failed += 1; continue
        bad = [(i, w, g) for i, (w, g) in enumerate(zip(want, got)) if w != g]
        if len(want) != len(got):
            bad.append((min(len(want), len(got)), "<%d lines>" % len(want), "<%d lines>" % len(got)))
        for i, w, g in bad[:20]:
            print("run %d line %d\n  oracle: %s\n  ours:   %s" % (k, i, w, g))
        reports = sanitize.reports(errf) if a.sanitize else []
        for r in reports:
            print("run %d: %s" % (k, r))
        if bad or reports:
            failed += 1
    n = len(want)
    print("IDENTICAL (%d lines)" % n if not failed and a.repeat == 1 else
          "IDENTICAL (%d lines, %d/%d runs)" % (n, a.repeat, a.repeat) if not failed else
          "%d/%d run(s) differ or report" % (failed, a.repeat))
    sys.exit(1 if failed else 0)



if __name__ == "__main__":
    main()
