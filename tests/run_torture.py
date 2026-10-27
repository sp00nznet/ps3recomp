#!/usr/bin/env python3
"""
ps3recomp lifter torture-suite runner.

For each optimization variant (-O0, -O2 -- different codegen exercises
different lifter paths):

  1. compile the guest suite with the PS3 SDK's ppu-lv2-gcc
  2. run the recomp pipeline: ppu_loader -> ppu_lifter -> gen_hle_nids
  3. rebuild the host harness (tests/torture) against the fresh lift
  4. run it, parse the guest's own PASS/FAIL protocol
  5. classify crashes ([CRASH]) and hangs ([TIMEOUT]) as failures too

Exit code: 0 = everything passed, 1 = failures, 2 = infrastructure error.

Usage:
    python tests/run_torture.py                 # both variants
    python tests/run_torture.py --variants O2   # one variant
    python tests/run_torture.py --skip-guest    # reuse existing .elf files
"""

import argparse
import glob
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GUEST = os.path.join(ROOT, "tests", "guest")
PORT = os.path.join(ROOT, "tests", "torture")
PPU_GCC = r"C:\usr\local\cell\host-win32\ppu\bin\ppu-lv2-gcc.exe"

GUEST_SRCS = ["torture.c", "torture_kats.c", "torture_mem.c", "torture_c.c"]


def run(cmd, cwd=None, timeout=None, check=True):
    p = subprocess.run(cmd, cwd=cwd, timeout=timeout,
                       capture_output=True, text=True, errors="replace")
    if check and p.returncode != 0:
        print(f"[runner] command failed ({p.returncode}): {' '.join(cmd)}")
        print(p.stdout[-3000:])
        print(p.stderr[-3000:])
        sys.exit(2)
    return p


def build_guest(opt):
    elf = os.path.join(GUEST, f"torture_{opt}.elf")
    print(f"[runner] compiling guest {opt} ...")
    run([PPU_GCC, f"-{opt}", "-Wall"] + GUEST_SRCS + ["-o", elf], cwd=GUEST)
    return elf


def lift(elf, opt):
    out = os.path.join(GUEST, f"out_{opt}")
    name = os.path.splitext(os.path.basename(elf))[0]
    print(f"[runner] lifting {name} ...")
    run([sys.executable, os.path.join(ROOT, "tools", "ppu_loader.py"),
         elf, "-o", out])
    # clear stale chunks (a smaller lift must not leave old TUs behind)
    for f in glob.glob(os.path.join(PORT, "recompiled", "ppu_recomp_*.cpp")):
        os.remove(f)
    run([sys.executable, os.path.join(ROOT, "tools", "ppu_lifter.py"), elf,
         "--functions", os.path.join(out, f"{name}.functions.json"),
         "--hle-stubs", os.path.join(out, f"{name}.imports.json"),
         "-o", os.path.join(PORT, "recompiled")])
    run([sys.executable, os.path.join(ROOT, "tools", "gen_hle_nids.py"),
         "sysPrxForUser",
         "--out", os.path.join(PORT, "gen", "ppu_hle_nids.cpp")])


def build_port():
    print("[runner] building host harness ...")
    run(["cmd", "/c", os.path.join(PORT, "build.bat")], cwd=PORT)


def run_torture(elf, timeout):
    exe = os.path.join(PORT, "build", "torture.exe")
    env = dict(os.environ, TORTURE_TIMEOUT=str(timeout))
    try:
        p = subprocess.run([exe, elf], capture_output=True, text=True,
                           errors="replace", timeout=timeout + 30, env=env,
                           cwd=os.path.join(PORT, "build"))
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or "") + (e.stderr or "")
        return out, "hard-timeout"
    return p.stdout + p.stderr, p.returncode


def parse(output):
    fails = re.findall(r"^FAIL .*$", output, re.M)
    m = re.search(r"TORTURE COMPLETE pass=(\d+) fail=(\d+)", output)
    sections = re.findall(r"^== (\S+)", output, re.M)
    crashed = "[CRASH]" in output
    timed_out = "[TIMEOUT]" in output
    return {
        "fails": fails,
        "complete": (int(m.group(1)), int(m.group(2))) if m else None,
        "last_section": sections[-1] if sections else None,
        "n_sections": len(sections),
        "crashed": crashed,
        "timed_out": timed_out,
    }


def group_fails(fails):
    by = {}
    for f in fails:
        name = f.split()[1].split("#")[0]
        by.setdefault(name, []).append(f)
    return by


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--variants", default="O2,O0")
    ap.add_argument("--skip-guest", action="store_true",
                    help="reuse existing torture_<opt>.elf")
    ap.add_argument("--timeout", type=int, default=60,
                    help="guest watchdog seconds")
    ap.add_argument("--max-show", type=int, default=6,
                    help="max FAIL lines shown per test name")
    args = ap.parse_args()

    if not os.path.exists(PPU_GCC):
        print(f"[runner] PS3 SDK compiler not found at {PPU_GCC}")
        sys.exit(2)

    # regenerate KATs (deterministic; cheap)
    run([sys.executable, os.path.join(GUEST, "gen_vectors.py"),
         "-o", os.path.join(GUEST, "torture_kats.c")], cwd=GUEST)

    overall_fail = 0
    results = {}
    for opt in args.variants.split(","):
        opt = opt.strip()
        elf = os.path.join(GUEST, f"torture_{opt}.elf")
        if not args.skip_guest or not os.path.exists(elf):
            elf = build_guest(opt)
        lift(elf, opt)
        build_port()
        print(f"[runner] running {os.path.basename(elf)} ...")
        output, rc = run_torture(elf, args.timeout)
        r = parse(output)
        results[opt] = r

        log = os.path.join(PORT, "build", f"torture_{opt}.log")
        with open(log, "w", errors="replace") as f:
            f.write(output)

        print(f"\n===== variant {opt} (rc={rc}, log={os.path.relpath(log, ROOT)}) =====")
        if r["crashed"]:
            print("  ** CRASHED ** (see [CRASH] block in log)")
            overall_fail = 1
        if r["timed_out"] or rc == "hard-timeout":
            print(f"  ** HUNG ** after section '{r['last_section']}' "
                  f"({r['n_sections']} sections reached)")
            overall_fail = 1
        if r["complete"]:
            p, fl = r["complete"]
            print(f"  pass={p} fail={fl}")
            if fl:
                overall_fail = 1
        elif not (r["crashed"] or r["timed_out"]):
            print(f"  ** NO COMPLETION LINE ** last section: {r['last_section']}")
            overall_fail = 1

        grouped = group_fails(r["fails"])
        for name in sorted(grouped, key=lambda k: -len(grouped[k])):
            lines = grouped[name]
            print(f"  {name}: {len(lines)} failing")
            for l in lines[:args.max_show]:
                print(f"    {l}")
            if len(lines) > args.max_show:
                print(f"    ... +{len(lines) - args.max_show} more")

    print("\n===== summary =====")
    for opt, r in results.items():
        state = ("CRASH" if r["crashed"] else
                 "HANG" if r["timed_out"] else
                 f"pass={r['complete'][0]} fail={r['complete'][1]}"
                 if r["complete"] else "incomplete")
        print(f"  {opt}: {state}  (failing test names: "
              f"{len(group_fails(r['fails']))})")
    sys.exit(overall_fail)


if __name__ == "__main__":
    main()
