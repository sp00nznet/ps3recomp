#!/usr/bin/env python3
"""Title-specific contamination in shared code.

The runtime and the HLE libraries are supposed to be game-agnostic -- that claim
is the whole pitch of the toolkit, and it is what somebody forking it to port
their own title is relying on. In practice a long bring-up leaves behind env-gated
scaffolding named after whichever game it was written for, in the shared files
every other title also runs through, and nothing notices.

This counts it, so draining it is measurable, and ratchets it, so it cannot grow
back while we drain:

    python tools/title_hacks.py             # report, grouped by file
    python tools/title_hacks.py --by-var    # report, grouped by variable
    python tools/title_hacks.py --check     # fail if the count went UP
    python tools/title_hacks.py --update    # re-record after draining some

A hit is a getenv() in shared code whose variable starts with a known title
prefix. Adding a new title's prefix to TITLE_PREFIXES is a deliberate act, which
is the point: it should be awkward to bake a fourth game's name into sys_event.c.

ponytail: a denylist of known titles, not an allowlist of approved prefixes. It
catches everything in the tree today. Switch to an allowlist when an outside fork
starts contributing, because that is when "MYGAME_HACK" stops being hypothetical.
"""
import argparse
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINE = os.path.join(ROOT, "tools", "title_hacks_baseline.json")

# Directories that are supposed to be game-agnostic. tools/ is excluded: the
# Python pipeline is allowed per-title knowledge, it is the RUNTIME that must not
# have it. Per-game test fixtures under */tests/ are excluded for the same reason.
SHARED_DIRS = ["runtime", "libs", "include"]
EXCLUDE_RE = re.compile(r"[/\\]tests[/\\]")
SOURCE_EXT = (".c", ".h", ".cpp", ".hpp", ".m")

# Prefixes that name a GAME rather than a subsystem. Each maps to the port it
# came from, so a reader knows what they are looking at.
TITLE_PREFIXES = {
    "YDKJ":     "You Don't Know Jack (BLUS30569)",
    "FLOW":     "flOw (NPUA80001)",
    "LBP":      "LittleBigPlanet",
    "TM":       "Twisted Metal",
    "RD":       "Rubber Ducky",
    "DUCK":     "Rubber Ducky / DuckTales",
    "SIM":      "The Simpsons Arcade Game (NPUB30563)",
    "SIMPSONS": "The Simpsons Arcade Game (NPUB30563)",
    "TJ":       "Tokyo Jungle (NPUA80523)",
    "VF5":      "Virtua Fighter 5 (BLUS30020)",
    "YZ":       "Yakuza: Dead Souls (fork)",
    "GT5P":     "Gran Turismo 5 Prologue",
    "SCOTT":    "Scott Pilgrim (NPEB00258)",
    "CUBE":     "gcm/cube SDK sample harness",
}

GETENV_RE = re.compile(r'getenv\s*\(\s*"([A-Za-z0-9_]+)"\s*\)')


def is_title_var(var):
    """-> prefix, or None. Longest prefix wins so SIMPSONS beats SIM."""
    for pfx in sorted(TITLE_PREFIXES, key=len, reverse=True):
        if var == pfx or var.startswith(pfx + "_"):
            return pfx
    return None


def scan():
    """-> list of (relpath, lineno, var, prefix)."""
    hits = []
    for d in SHARED_DIRS:
        for dirpath, _, files in os.walk(os.path.join(ROOT, d)):
            for fn in files:
                if not fn.endswith(SOURCE_EXT):
                    continue
                full = os.path.join(dirpath, fn)
                rel = os.path.relpath(full, ROOT).replace("\\", "/")
                if EXCLUDE_RE.search(os.path.join(dirpath, fn)):
                    continue
                with open(full, encoding="utf-8", errors="replace") as f:
                    for n, line in enumerate(f, 1):
                        for var in GETENV_RE.findall(line):
                            pfx = is_title_var(var)
                            if pfx:
                                hits.append((rel, n, var, pfx))
    return hits


def report(hits, by_var):
    if by_var:
        by = {}
        for rel, n, var, pfx in hits:
            by.setdefault(var, []).append(rel)
        for var in sorted(by, key=lambda v: (-len(by[v]), v)):
            print("  %4d  %-24s %s" % (len(by[var]), var,
                                       ", ".join(sorted(set(by[var])))))
    else:
        by = {}
        for rel, n, var, pfx in hits:
            by.setdefault(rel, []).append(var)
        for rel in sorted(by, key=lambda r: (-len(by[r]), r)):
            print("  %4d  %s" % (len(by[rel]), rel))
            seen = sorted(set(by[rel]))
            print("        %s" % ", ".join(seen[:8]) + (" ..." if len(seen) > 8 else ""))

    print()
    per_title = {}
    for rel, n, var, pfx in hits:
        per_title[pfx] = per_title.get(pfx, 0) + 1
    for pfx in sorted(per_title, key=lambda p: -per_title[p]):
        print("  %4d  %-9s %s" % (per_title[pfx], pfx, TITLE_PREFIXES[pfx]))
    print("\n  %4d  TOTAL in %d shared files" % (hits and len(hits) or 0,
                                                 len(set(h[0] for h in hits))))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--by-var", action="store_true")
    ap.add_argument("--check", action="store_true", help="fail if the count went up")
    ap.add_argument("--update", action="store_true", help="re-record the baseline")
    args = ap.parse_args()

    hits = scan()
    total = len(hits)
    per_file = {}
    for rel, n, var, pfx in hits:
        per_file[rel] = per_file.get(rel, 0) + 1

    if args.update:
        with open(BASELINE, "w") as f:
            json.dump({"total": total, "per_file": per_file}, f,
                      indent=2, sort_keys=True)
            f.write("\n")
        print("[title-hacks] recorded baseline: %d hits in %d files"
              % (total, len(per_file)))
        return 0

    if args.check:
        if not os.path.exists(BASELINE):
            print("[title-hacks] no baseline -- run --update once")
            return 2
        with open(BASELINE) as f:
            base = json.load(f)
        worse = [(rel, per_file[rel], base["per_file"].get(rel, 0))
                 for rel in sorted(per_file)
                 if per_file[rel] > base["per_file"].get(rel, 0)]
        if worse:
            print("[title-hacks] REGRESSION -- game-specific hacks added to shared code:")
            for rel, now, was in worse:
                print("    %s: %d -> %d" % (rel, was, now))
            print("\n  Shared code is supposed to be game-agnostic. Put this behind a")
            print("  generic name, or in the port's own repo.")
            return 1
        moved = base["total"] - total
        print("[title-hacks] OK -- %d hits (baseline %d%s)"
              % (total, base["total"],
                 ", %d drained -- run --update" % moved if moved > 0 else ""))
        return 0

    report(hits, args.by_var)
    return 0


if __name__ == "__main__":
    sys.exit(main())
