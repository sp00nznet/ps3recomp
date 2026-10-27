"""
run_compare.py -- one-command driver for the cross-platform comparison harness.

Given a PS3 units.json (side A) and an X360 units.json (side B), it runs the
matcher and writes the markdown report + findings JSON. It's a thin wrapper over
match_functions + compare_report so there is a single obvious entrypoint.

  python run_compare.py ps3.units.json x360.units.json --outdir out/

To PRODUCE the two units.json inputs, use one front-end per binary:

  PS3 (Cell PPU, EBOOT.ELF):
    # option 1 -- ps3recomp's own tools:
    python ../ppu_disasm.py EBOOT.ELF --json > instructions.json
    python ../find_functions.py EBOOT.ELF --json > functions.json
    python ingest_native.py --functions functions.json \
        --instructions instructions.json --binary EBOOT.ELF -o ps3.units.json
    # option 2 -- Ghidra:
    analyzeHeadless proj P -import EBOOT.ELF \
        -processor PowerPC:BE:64:64-32addr \
        -postScript ExportCompareUnits.java ps3 ppc64-cell ps3.units.json

  X360 (Xenon, default.xex):
    python xex_parser.py default.xex          # identify the build / unpack check
    # Ghidra (its XEX loader unpacks the basefile):
    analyzeHeadless proj P -import default.xex \
        -postScript ExportCompareUnits.java x360 ppc64-xenon x360.units.json
    # or IDA:
    ida64 -A -S"ida_export_units.py x360 ppc64-xenon x360.units.json" default.xex
"""

from __future__ import annotations

import argparse
import json
import os

from schema import Module
from match_functions import match
from compare_report import build_findings, render_markdown


def main() -> None:
    ap = argparse.ArgumentParser(description="Run the full cross-platform comparison")
    ap.add_argument("a", help="PS3 units.json (side A)")
    ap.add_argument("b", help="X360 units.json (side B)")
    ap.add_argument("--outdir", default="out", help="output directory")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    os.makedirs(args.outdir, exist_ok=True)
    a = Module.load(args.a)
    b = Module.load(args.b)

    res = match(a, b, verbose=args.verbose)
    findings = build_findings(a, b, res)

    md_path = os.path.join(args.outdir, "report.md")
    fn_path = os.path.join(args.outdir, "findings.json")
    mt_path = os.path.join(args.outdir, "matches.json")
    with open(md_path, "w", encoding="utf-8") as fh:
        fh.write(render_markdown(a, b, res, findings))
    with open(fn_path, "w", encoding="utf-8") as fh:
        json.dump(findings, fh, indent=2)
    with open(mt_path, "w", encoding="utf-8") as fh:
        json.dump(res.to_json(), fh, indent=2)

    s = findings["summary"]
    print("=" * 60)
    print(f"A({a.platform}/{a.source}): {s['a_total']} fns   "
          f"B({b.platform}/{b.source}): {s['b_total']} fns")
    print(f"matched {s['matched']}  "
          f"({s['match_rate_a']*100:.1f}% of A, {s['match_rate_b']*100:.1f}% of B)")
    print(f"diverging pairs: {len(findings['diverging_pairs'])}")
    print(f"reports -> {md_path}, {fn_path}, {mt_path}")


if __name__ == "__main__":
    main()
