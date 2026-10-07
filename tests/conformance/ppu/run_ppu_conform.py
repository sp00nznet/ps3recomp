#!/usr/bin/env python3
"""PPU conformance: RPCS3 (oracle) vs ps3recomp, end to end.

  1. gen_ppu_conform.py writes ppu_conform.elf (+ manifest)
  2. RPCS3 runs it headless with oracle_config.yml (PPU interpreter, accurate
     flags); its TTY.log is the oracle transcript
  3. ppu_loader.py + ppu_lifter.py lift the same ELF; templates/project builds it
  4. the lifted binary runs; its stdout is our transcript
  5. compare.py diffs them field by field

usage: run_ppu_conform.py --work DIR [--rpcs3 PATH] [--only OPS] [--scale N]
       [--seed N] [--skip-oracle] [--skip-build] [--ignore FIELDS]
Needs a local RPCS3 build (default: <repo>/../rpcs3/build/bin/rpcs3.app/...).
"""
import argparse
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
DEFAULT_RPCS3 = os.path.join(ROOT, "..", "rpcs3", "build", "bin", "rpcs3.app", "Contents", "MacOS", "rpcs3")
RPCS3_CACHE = os.path.expanduser("~/Library/Caches/rpcs3")

# Settings that make RPCS3's PPU interpreter the most accurate reference.
ORACLE_SETTINGS = {
    "PPU Decoder": "Interpreter (static)",
    "PPU Set Saturation Bit": "true",
    "PPU Accurate Non-Java Mode": "true",
    "PPU Accurate Vector NaN Values": "true",
    "PPU Set FPCC Bits": "true",
    "Use Accurate DFMA": "true",
    "PPU Vector NaN Handling": "true",
}


def sh(cmd, **kw):
    print("+", " ".join(cmd) if isinstance(cmd, list) else cmd, flush=True)
    return subprocess.run(cmd, **kw)


def oracle_config(work, extra=None):
    """The user's RPCS3 config with the oracle settings forced (written to work/)."""
    settings = dict(ORACLE_SETTINGS, **(extra or {}))
    base = os.path.expanduser("~/Library/Application Support/rpcs3/config.yml")
    out = os.path.join(work, "oracle_config.yml")
    lines = open(base).read().split("\n") if os.path.exists(base) else ["Core:"]
    seen = set()
    for i, l in enumerate(lines):
        k = l.strip().split(":")[0]
        if k in settings:
            indent = l[:len(l) - len(l.lstrip())]
            lines[i] = "%s%s: %s" % (indent, k, settings[k])
            seen.add(k)
    missing = [k for k in settings if k not in seen]
    if missing:
        i = next((n for n, l in enumerate(lines) if l.startswith("Core:")), None)
        if i is None:
            lines.append("Core:"); i = len(lines) - 1
        for k in missing:
            lines.insert(i + 1, "  %s: %s" % (k, settings[k]))
    open(out, "w").write("\n".join(lines))
    return out


def run_oracle(rpcs3, elf, work, timeout, marker=b"PPUCONF END", extra=None):
    tty = os.path.join(RPCS3_CACHE, "TTY.log")
    cfg = oracle_config(work, extra)
    # The previous run's TTY.log ends with the marker too: remove it, or the
    # wait below can match it before RPCS3 has started a new one.
    try:
        os.remove(tty)
    except FileNotFoundError:
        pass
    p = subprocess.Popen([rpcs3, "--headless", "--config", cfg, elf],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    t0 = time.time()
    # RPCS3 aborts on process exit in headless mode; wait for the END marker instead.
    while time.time() - t0 < timeout:
        time.sleep(2)
        if p.poll() is not None:
            break
        try:
            with open(tty, "rb") as f:
                f.seek(max(0, os.path.getsize(tty) - 64))
                if marker in f.read():
                    break
        except OSError:
            pass
    if p.poll() is None:
        p.kill()
    p.wait()
    out = os.path.join(work, "oracle.txt")
    shutil.copy(tty, out)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", required=True)
    ap.add_argument("--rpcs3", default=DEFAULT_RPCS3)
    ap.add_argument("--only", default="")
    ap.add_argument("--scale", type=int, default=1)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--skip-oracle", action="store_true")
    ap.add_argument("--skip-build", action="store_true")
    ap.add_argument("--ignore", default="r1,fpscr")
    ap.add_argument("--show", type=int, default=3)
    ap.add_argument("--timeout", type=int, default=1200)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 8)
    a = ap.parse_args()
    work = os.path.abspath(a.work)
    os.makedirs(work, exist_ok=True)
    elf = os.path.join(work, "ppu_conform.elf")
    man = os.path.join(work, "ppu_conform.manifest.json")
    py = sys.executable

    gen = [py, os.path.join(HERE, "gen_ppu_conform.py"), "-o", elf, "--manifest", man,
           "--seed", str(a.seed), "--scale", str(a.scale)]
    if a.only:
        gen += ["--only", a.only]
    if sh(gen).returncode:
        sys.exit(2)

    if not a.skip_oracle:
        run_oracle(a.rpcs3, elf, work, a.timeout)

    rec = os.path.join(work, "recompiled")
    bld = os.path.join(work, "build")
    if not a.skip_build:
        load = os.path.join(work, "load")
        if sh([py, os.path.join(ROOT, "tools", "ppu_loader.py"), elf, "-o", load],
              stdout=subprocess.DEVNULL).returncode:
            sys.exit(2)
        if sh([py, os.path.join(ROOT, "tools", "ppu_lifter.py"), elf,
               "--functions", os.path.join(load, "ppu_conform.functions.json"),
               "--hle-stubs", os.path.join(load, "ppu_conform.imports.json"),
               "-o", rec, "--max-chunk-lines", "25000"], stdout=subprocess.DEVNULL).returncode:
            sys.exit(2)
        if not os.path.exists(os.path.join(bld, "build.ninja")):
            if sh(["cmake", "-S", os.path.join(ROOT, "templates", "project"), "-B", bld, "-G", "Ninja",
                   "-DCMAKE_BUILD_TYPE=Release", "-DRECOMP_DIR=" + rec],
                  stdout=subprocess.DEVNULL).returncode:
                sys.exit(2)
        r = sh(["cmake", "--build", bld, "-j", str(a.jobs)], capture_output=True, text=True)
        if r.returncode:
            print("\n".join(l for l in (r.stdout + r.stderr).split("\n") if " error" in l or "Error" in l)[:4000])
            sys.exit(2)

    ours = os.path.join(work, "ours.txt")
    with open(ours, "wb") as fo, open(os.path.join(work, "ours.stderr.txt"), "wb") as fe:
        try:
            r = subprocess.run([os.path.join(bld, "MyGameRecomp"), elf], stdout=fo, stderr=fe,
                               timeout=a.timeout, env=dict(os.environ, PS3_VERBOSE="0"), cwd=work)
            print("lifted run exit:", r.returncode)
        except subprocess.TimeoutExpired:
            print("lifted run: TIMEOUT")

    r = sh([py, os.path.join(HERE, "compare.py"), man, os.path.join(work, "oracle.txt"), ours,
            "--ignore", a.ignore, "--show", str(a.show), "--json", os.path.join(work, "result.json")])
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
