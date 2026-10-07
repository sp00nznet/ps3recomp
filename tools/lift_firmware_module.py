#!/usr/bin/env python3
"""
Lift a decrypted PRX into a module lv2's PRX loader can load by file name:
the whole of docs/FIRMWARE_LLE.md steps 2-4 in one command.

  1. tools/lift_prx.py      relocate to --base, write image + metadata
  2. tools/ppu_lifter.py    lift to C, symbols prefixed "<stem>_", output files
                            named "<stem>_recomp*", the module's own imports
                            lifted as ps3_hle_call(nid) (so they reach another
                            loaded module's export, or the HLE library)
  3. tools/gen_prx_module.py  the registration unit (<stem>_module.cpp)

Everything written to --out is derived from the firmware and stays local.
Drop --out's C/C++ files into a build beside the title's lift.

usage: lift_firmware_module.py libsre.prx --base 0x30100000 --out DIR
       [--file libsre.sprx] [--stem libsre]
"""
import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def run(cmd):
    print("+ " + " ".join(cmd), flush=True)
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode:
        sys.stdout.write(r.stdout)
        raise SystemExit("failed: " + cmd[1])
    return r.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("prx", help="decrypted .prx")
    ap.add_argument("--base", required=True, type=lambda x: int(x, 0))
    ap.add_argument("--out", required=True)
    ap.add_argument("--stem", help="default: the input's file stem")
    ap.add_argument("--file", help="file name the guest loads it by (default <stem>.sprx)")
    a = ap.parse_args()

    stem = a.stem or os.path.splitext(os.path.basename(a.prx))[0]
    out = os.path.abspath(a.out)
    meta = os.path.join(out, "meta")
    os.makedirs(meta, exist_ok=True)
    py = sys.executable

    run([py, os.path.join(HERE, "lift_prx.py"), a.prx, "--base", hex(a.base), "--output", meta])
    m = json.load(open(os.path.join(meta, stem + "_exports.json")))
    imports = json.load(open(os.path.join(meta, stem + "_imports.json")))

    sys.path.insert(0, HERE)
    import lift_prx
    va, size = lift_prx.Prx(a.prx).text_extent()
    code_end = a.base + va + size

    cmd = [py, os.path.join(HERE, "ppu_lifter.py"), os.path.join(meta, stem + "_image.bin"),
           "--raw", "--base", hex(a.base), "--toc", m["toc"], "--code-end", hex(code_end),
           "--functions", os.path.join(meta, stem + "_functions.json"),
           "--symbol-prefix", stem + "_",
           "--header-name", stem + "_recomp.h", "--source-name", stem + "_recomp.c",
           "-o", out]
    stubs = [{"library": lib, "nid": nid, "stub": slot}
             for lib, e in imports.items() for nid, slot in e.items()]
    if stubs:
        sp = os.path.join(meta, stem + "_hle_stubs.json")
        json.dump(stubs, open(sp, "w"), indent=1)
        cmd += ["--hle-stubs", sp]
    print(run(cmd).strip().splitlines()[-2])

    print(run([py, os.path.join(HERE, "gen_prx_module.py"), meta, "--stem", stem,
               "--file", a.file or stem + ".sprx", "--prefix", stem + "_",
               "--out", os.path.join(out, stem + "_module.cpp")]).strip())


if __name__ == "__main__":
    main()
