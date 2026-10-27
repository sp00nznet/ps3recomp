"""
Cross-platform function matcher.

Given two Modules (conventionally `a` = PS3 / ps3recomp, `b` = X360 / rexglue),
pair up functions that correspond to the same source-level function.

Why this is tractable here: both binaries are 64-bit big-endian PowerPC built
from a largely shared C++ codebase (same engine on both consoles). String
literals and large constants survive compilation almost identically, so they
make excellent platform-independent anchors. From a handful of anchors we
propagate along the call graph, then score every candidate pair structurally.

Pipeline:
  1. Name anchors      -- identical (normalized) symbol names, when present.
  2. String anchors    -- a string referenced by exactly one unit on EACH side
                          is a near-certain pair. Rarer strings score higher.
  3. Const anchors     -- same idea for distinctive immediates/data constants.
  4. Call-graph propagation -- from an anchored pair, if both sides make a
                          single unmatched call, pair the callees (gated by
                          structural similarity). Iterate to a fixpoint.
  5. Structural scoring -- mnemonic-histogram cosine + size/insn ratios +
                          leaf/call agreement, used to confirm and to surface
                          "matched but diverging" pairs (the interesting ones:
                          same function, different codegen => mining target).

Pure stdlib. Import as a library (`match`) or run as a CLI on two units.json.
"""

from __future__ import annotations

import argparse
import json
import math
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from typing import Iterable

from schema import Module, Unit, hex_addr


# Strings shorter than this carry little signal (e.g. "%d", "/", "on").
MIN_ANCHOR_STRING_LEN = 6
# A const is only an anchor if it looks distinctive (not a tiny int / common mask).
CONST_ANCHOR_MIN = 0x1000
# Below this structural score we won't auto-propagate a callee pairing.
PROPAGATE_MIN_SIM = 0.55
# Pairs matched but below this similarity are flagged as "diverging".
DIVERGENCE_SIM = 0.80


@dataclass
class Match:
    a_addr: int
    b_addr: int
    confidence: float          # 0..1
    method: str                # how it was first anchored
    sim: float                 # structural similarity of the final pair
    evidence: list[str] = field(default_factory=list)

    def to_json(self) -> dict:
        return {
            "a_addr": hex_addr(self.a_addr),
            "b_addr": hex_addr(self.b_addr),
            "confidence": round(self.confidence, 4),
            "method": self.method,
            "sim": round(self.sim, 4),
            "evidence": self.evidence,
        }


@dataclass
class MatchResult:
    matches: list[Match]
    a_unmatched: list[int]
    b_unmatched: list[int]
    a_total: int
    b_total: int

    def to_json(self) -> dict:
        return {
            "summary": {
                "a_total": self.a_total,
                "b_total": self.b_total,
                "matched": len(self.matches),
                "a_unmatched": len(self.a_unmatched),
                "b_unmatched": len(self.b_unmatched),
                "match_rate_a": round(len(self.matches) / self.a_total, 4) if self.a_total else 0.0,
                "match_rate_b": round(len(self.matches) / self.b_total, 4) if self.b_total else 0.0,
            },
            "matches": [m.to_json() for m in self.matches],
            "a_unmatched": [hex_addr(a) for a in self.a_unmatched],
            "b_unmatched": [hex_addr(a) for a in self.b_unmatched],
        }


# --------------------------------------------------------------------------
# structural similarity
# --------------------------------------------------------------------------

def _cosine(h1: dict[str, int], h2: dict[str, int]) -> float:
    if not h1 or not h2:
        return 0.0
    keys = set(h1) | set(h2)
    dot = sum(h1.get(k, 0) * h2.get(k, 0) for k in keys)
    n1 = math.sqrt(sum(v * v for v in h1.values()))
    n2 = math.sqrt(sum(v * v for v in h2.values()))
    if n1 == 0 or n2 == 0:
        return 0.0
    return dot / (n1 * n2)


def _ratio(x: int, y: int) -> float:
    """Symmetric size ratio in [0,1]; 1.0 == identical."""
    if x == 0 and y == 0:
        return 1.0
    if x == 0 or y == 0:
        return 0.0
    return min(x, y) / max(x, y)


def structural_sim(a: Unit, b: Unit) -> float:
    """
    Weighted blend of cheap structural signals. Tuned so that "same function,
    same compiler family" lands high (>0.85) while unrelated functions stay low.
    """
    mnem = _cosine(a.mnemonic_hist, b.mnemonic_hist)          # dominant signal
    insn = _ratio(a.insn_count, b.insn_count)
    size = _ratio(a.size, b.size)
    leaf = 1.0 if a.is_leaf == b.is_leaf else 0.0
    ncall = _ratio(len(a.calls), len(b.calls))
    return 0.55 * mnem + 0.20 * insn + 0.10 * size + 0.05 * leaf + 0.10 * ncall


# --------------------------------------------------------------------------
# anchoring helpers
# --------------------------------------------------------------------------

def _norm_name(name: str | None) -> str | None:
    """Strip leading underscores / platform decoration for name comparison."""
    if not name:
        return None
    n = name.strip()
    # ignore tool-generated placeholders
    low = n.lower()
    for junk in ("fun_", "sub_", "lab_", "func_", "loc_", "unk_", "j_"):
        if low.startswith(junk):
            return None
    return n.lstrip("_.")


def _unique_index(units: list[Unit], key_fn) -> dict:
    """
    Build value -> single-unit-addr for values that are referenced by exactly
    one unit. Values shared by multiple units are dropped (ambiguous).
    """
    buckets: dict = defaultdict(set)
    for u in units:
        for k in key_fn(u):
            buckets[k].add(u.addr)
    return {k: next(iter(v)) for k, v in buckets.items() if len(v) == 1}


def _string_keys(u: Unit) -> Iterable[str]:
    for s in u.string_refs:
        if len(s) >= MIN_ANCHOR_STRING_LEN:
            yield s


def _const_keys(u: Unit) -> Iterable[int]:
    for c in u.const_refs:
        if abs(c) >= CONST_ANCHOR_MIN:
            yield c


# --------------------------------------------------------------------------
# matcher
# --------------------------------------------------------------------------

def match(a: Module, b: Module, verbose: bool = False) -> MatchResult:
    a_units = a.by_addr()
    b_units = b.by_addr()

    a2b: dict[int, int] = {}          # ps3 addr -> x360 addr
    b2a: dict[int, int] = {}
    info: dict[tuple[int, int], Match] = {}

    def claim(aa: int, bb: int, method: str, conf: float, ev: str) -> bool:
        if aa in a2b or bb in b2a:
            return False
        a2b[aa] = bb
        b2a[bb] = aa
        info[(aa, bb)] = Match(
            a_addr=aa, b_addr=bb, confidence=conf, method=method,
            sim=structural_sim(a_units[aa], b_units[bb]),
            evidence=[ev],
        )
        return True

    # ---- 1. name anchors ------------------------------------------------
    a_names = {n: u.addr for u in a.units if (n := _norm_name(u.name))}
    b_names = {n: u.addr for u in b.units if (n := _norm_name(u.name))}
    # only names unique on both sides
    for n, aa in a_names.items():
        bb = b_names.get(n)
        if bb is not None:
            claim(aa, bb, "name", 0.98, f"name={n!r}")

    # ---- 2. string anchors ---------------------------------------------
    a_str = _unique_index(a.units, _string_keys)
    b_str = _unique_index(b.units, _string_keys)
    # rank by string length (longer == rarer == more confident) for stable claiming
    shared_strs = sorted(set(a_str) & set(b_str), key=len, reverse=True)
    for s in shared_strs:
        claim(a_str[s], b_str[s], "string", 0.95, f"unique-string={s[:48]!r}")

    # ---- 3. const anchors ----------------------------------------------
    a_c = _unique_index(a.units, _const_keys)
    b_c = _unique_index(b.units, _const_keys)
    for c in set(a_c) & set(b_c):
        if claim(a_c[c], b_c[c], "const", 0.85, f"unique-const={hex_addr(c)}"):
            pass

    if verbose:
        print(f"[seed] name+string+const anchors: {len(a2b)}")

    # ---- 4. call-graph propagation -------------------------------------
    changed = True
    rounds = 0
    while changed and rounds < 50:
        changed = False
        rounds += 1
        for aa, bb in list(a2b.items()):
            ua, ub = a_units[aa], b_units[bb]
            # unmatched callees on each side
            a_callees = [c for c in ua.calls if c in a_units and c not in a2b]
            b_callees = [c for c in ub.calls if c in b_units and c not in b2a]
            # Unambiguous 1:1 case -> pair if structurally plausible.
            if len(a_callees) == 1 and len(b_callees) == 1:
                ca, cb = a_callees[0], b_callees[0]
                sim = structural_sim(a_units[ca], b_units[cb])
                if sim >= PROPAGATE_MIN_SIM:
                    if claim(ca, cb, "callgraph", 0.70 + 0.25 * sim,
                             f"sole callee of matched {hex_addr(aa)} (sim={sim:.2f})"):
                        changed = True
                continue
            # Multi-callee: greedily pair by best structural sim above threshold.
            if a_callees and b_callees:
                pairs = []
                for ca in a_callees:
                    for cb in b_callees:
                        s = structural_sim(a_units[ca], b_units[cb])
                        if s >= max(PROPAGATE_MIN_SIM, 0.7):
                            pairs.append((s, ca, cb))
                pairs.sort(reverse=True)
                for s, ca, cb in pairs:
                    if claim(ca, cb, "callgraph", 0.65 + 0.25 * s,
                             f"callee of matched {hex_addr(aa)} (sim={s:.2f})"):
                        changed = True

    if verbose:
        print(f"[propagate] after {rounds} rounds: {len(a2b)} matches")

    # ---- finalize: refresh sim, sort ------------------------------------
    matches = sorted(info.values(), key=lambda m: (-m.confidence, m.a_addr))
    a_unmatched = sorted(set(a_units) - set(a2b))
    b_unmatched = sorted(set(b_units) - set(b2a))
    return MatchResult(
        matches=matches,
        a_unmatched=a_unmatched,
        b_unmatched=b_unmatched,
        a_total=len(a_units),
        b_total=len(b_units),
    )


def main() -> None:
    ap = argparse.ArgumentParser(description="Cross-platform PPC function matcher")
    ap.add_argument("a", help="units.json for side A (e.g. PS3 / ps3recomp)")
    ap.add_argument("b", help="units.json for side B (e.g. X360 / rexglue)")
    ap.add_argument("-o", "--out", help="write matches.json here")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    ma = Module.load(args.a)
    mb = Module.load(args.b)
    res = match(ma, mb, verbose=args.verbose)
    out = res.to_json()
    if args.out:
        with open(args.out, "w", encoding="utf-8") as fh:
            json.dump(out, fh, indent=2)
        print(f"wrote {args.out}")
    s = out["summary"]
    print(f"A({ma.platform}) {s['a_total']} fns  B({mb.platform}) {s['b_total']} fns  "
          f"matched {s['matched']}  "
          f"({s['match_rate_a']*100:.1f}% of A, {s['match_rate_b']*100:.1f}% of B)")


if __name__ == "__main__":
    main()
