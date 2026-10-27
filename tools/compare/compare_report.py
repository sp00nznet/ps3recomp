"""
Turn a MatchResult into a human-readable report plus machine-readable findings.

The report is written to highlight the things we actually care about for
improving ps3recomp:

  * Coverage      -- how much of each build we paired up.
  * Diverging pairs -- functions we are confident are the SAME source function
                     but whose code differs a lot between builds. These are the
                     mining targets: study how the (proven) 360/rexglue side
                     lifted it vs how ps3recomp did.
  * Mnemonic deltas -- opcodes that appear a lot on one side but not the other.
                     Surfaces ISA features (e.g. VMX128 on Xenon) and codegen
                     idioms that one lifter handles and the other may not.
  * Unmatched      -- functions present in one build but not the other (engine
                     subset differences, or matcher gaps to investigate).
"""

from __future__ import annotations

import argparse
import json
from collections import Counter

from schema import Module, hex_addr
from match_functions import match, MatchResult, DIVERGENCE_SIM


def _corpus_hist(mod: Module) -> Counter:
    c: Counter = Counter()
    for u in mod.units:
        c.update(u.mnemonic_hist)
    return c


def _normalized_deltas(a: Module, b: Module, top: int = 40):
    """
    Compare per-mnemonic frequency between the two corpora as a fraction of
    total instructions, so binary-size differences don't dominate. Returns
    (only_in_a, only_in_b, skewed) lists of (mnem, a_frac, b_frac).
    """
    ha, hb = _corpus_hist(a), _corpus_hist(b)
    ta, tb = max(1, sum(ha.values())), max(1, sum(hb.values()))
    keys = set(ha) | set(hb)
    rows = []
    for k in keys:
        fa, fb = ha.get(k, 0) / ta, hb.get(k, 0) / tb
        rows.append((k, ha.get(k, 0), fa, hb.get(k, 0), fb))
    only_a = sorted([r for r in rows if r[3] == 0 and r[1] > 0], key=lambda r: -r[1])[:top]
    only_b = sorted([r for r in rows if r[1] == 0 and r[3] > 0], key=lambda r: -r[3])[:top]
    skew = sorted(
        [r for r in rows if r[1] > 0 and r[3] > 0],
        key=lambda r: -abs(r[2] - r[4]),
    )[:top]
    return only_a, only_b, skew


def build_findings(a: Module, b: Module, res: MatchResult) -> dict:
    a_u, b_u = a.by_addr(), b.by_addr()
    diverging = [
        {
            "a_addr": hex_addr(m.a_addr),
            "b_addr": hex_addr(m.b_addr),
            "sim": round(m.sim, 3),
            "method": m.method,
            "a_name": a_u[m.a_addr].name,
            "b_name": b_u[m.b_addr].name,
            "a_insn": a_u[m.a_addr].insn_count,
            "b_insn": b_u[m.b_addr].insn_count,
            "evidence": m.evidence,
        }
        for m in res.matches
        if m.sim < DIVERGENCE_SIM
    ]
    diverging.sort(key=lambda d: d["sim"])
    only_a, only_b, skew = _normalized_deltas(a, b)
    return {
        "summary": res.to_json()["summary"],
        "diverging_pairs": diverging,
        "mnemonics_only_in_a": [
            {"mnem": k, "count": ca} for k, ca, _, _, _ in only_a
        ],
        "mnemonics_only_in_b": [
            {"mnem": k, "count": cb} for k, _, _, cb, _ in only_b
        ],
        "mnemonics_skewed": [
            {"mnem": k, "a_frac": round(fa, 4), "b_frac": round(fb, 4)}
            for k, _, fa, _, fb in skew
        ],
    }


def render_markdown(a: Module, b: Module, res: MatchResult, findings: dict) -> str:
    s = findings["summary"]
    L = []
    L.append(f"# Cross-platform comparison: {a.binary} (A) vs {b.binary} (B)\n")
    L.append(f"- **A** = `{a.platform}` / `{a.arch}` / source=`{a.source}` — {s['a_total']} functions")
    L.append(f"- **B** = `{b.platform}` / `{b.arch}` / source=`{b.source}` — {s['b_total']} functions")
    L.append("")
    L.append("## Coverage\n")
    L.append(f"- Matched pairs: **{s['matched']}**")
    L.append(f"- Coverage of A: **{s['match_rate_a']*100:.1f}%** "
             f"({s['a_unmatched']} unmatched)")
    L.append(f"- Coverage of B: **{s['match_rate_b']*100:.1f}%** "
             f"({s['b_unmatched']} unmatched)")
    by_method = Counter(m.method for m in res.matches)
    L.append(f"- Anchor breakdown: " + ", ".join(f"{k}={v}" for k, v in by_method.most_common()))
    L.append("")

    L.append("## Diverging matched pairs (same function, different codegen)\n")
    L.append("_Highest-value targets: confidently paired but structurally far apart._\n")
    dv = findings["diverging_pairs"]
    if not dv:
        L.append("_None below the divergence threshold._")
    else:
        L.append("| sim | A addr | B addr | A insn | B insn | A name | B name | via |")
        L.append("|----:|--------|--------|-------:|-------:|--------|--------|-----|")
        for d in dv[:60]:
            L.append(f"| {d['sim']:.2f} | {d['a_addr']} | {d['b_addr']} | "
                     f"{d['a_insn']} | {d['b_insn']} | "
                     f"{(d['a_name'] or '')[:24]} | {(d['b_name'] or '')[:24]} | {d['method']} |")
        if len(dv) > 60:
            L.append(f"\n_+{len(dv)-60} more in findings.json_")
    L.append("")

    L.append("## Mnemonics only on the B side (e.g. Xenon VMX128 / 360 idioms)\n")
    mb = findings["mnemonics_only_in_b"]
    L.append(", ".join(f"`{m['mnem']}`({m['count']})" for m in mb) or "_none_")
    L.append("\n## Mnemonics only on the A side\n")
    ma = findings["mnemonics_only_in_a"]
    L.append(", ".join(f"`{m['mnem']}`({m['count']})" for m in ma) or "_none_")
    L.append("\n## Most frequency-skewed shared mnemonics\n")
    L.append("| mnem | A freq | B freq |")
    L.append("|------|-------:|-------:|")
    for m in findings["mnemonics_skewed"][:25]:
        L.append(f"| `{m['mnem']}` | {m['a_frac']*100:.2f}% | {m['b_frac']*100:.2f}% |")
    L.append("")
    return "\n".join(L)


def main() -> None:
    ap = argparse.ArgumentParser(description="Generate cross-platform comparison report")
    ap.add_argument("a", help="units.json side A (PS3)")
    ap.add_argument("b", help="units.json side B (X360)")
    ap.add_argument("--md", default="report.md", help="markdown report output")
    ap.add_argument("--findings", default="findings.json", help="findings JSON output")
    ap.add_argument("--matches", help="optional: also dump raw matches.json")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    a = Module.load(args.a)
    b = Module.load(args.b)
    res = match(a, b, verbose=args.verbose)
    findings = build_findings(a, b, res)

    with open(args.findings, "w", encoding="utf-8") as fh:
        json.dump(findings, fh, indent=2)
    with open(args.md, "w", encoding="utf-8") as fh:
        fh.write(render_markdown(a, b, res, findings))
    if args.matches:
        with open(args.matches, "w", encoding="utf-8") as fh:
            json.dump(res.to_json(), fh, indent=2)

    s = findings["summary"]
    print(f"matched {s['matched']}  "
          f"({s['match_rate_a']*100:.1f}% A / {s['match_rate_b']*100:.1f}% B)  "
          f"diverging={len(findings['diverging_pairs'])}")
    print(f"wrote {args.md}, {args.findings}")


if __name__ == "__main__":
    main()
