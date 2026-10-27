#!/usr/bin/env python3
"""hle_dispatch_classify.py -- dispatch-layer classifier for hle_abi_audit.py.

A raw libs/-vs-RPCS3 signature mismatch (as produced by hle_abi_audit.py) is
only a *live* bug if the mismatched libs/ function is the one actually
invoked at runtime. This tool reads a generated import-bridge table (a game
project's mechanical NID -> function-pointer dispatch table, e.g. one
produced by a `gen_imports.py`-style generator) with three dispatch layers
per PS3 import NID: a `{ NID, fn, lle_opd }` bridge-table row (each with a
`/* module::name */` trailing comment naming the PS3 export), where `fn` is
either nullptr, an override-wrapper symbol, or a generated marshaling-wrapper
symbol. The override/wrapper naming convention is game-project-specific and
configurable via --imp-prefix/--ovr-prefix (default `yz_imp_`/`yz_ovr_`,
this repo's own game project's convention -- see --bridges/--overrides for
where its generated table lives):

  1. fn == nullptr, lle_opd != 0
         -> LLE-DISPATCHED: the call binds directly to a lifted-firmware
            export OPD (Sony's own code, from recomp_prx/). The libs/ HLE
            function of the same name, if any, is NEVER CALLED for this
            import slot -- dead code for this dispatch path.
  2. fn is a `<ovr-prefix><Name>` symbol (defined in --overrides)
         -> OVERRIDE-WRAPPED: a hand-written wrapper shadows the libs/
            function for this import slot. The override body may (a) call
            the libs/ function of the same name internally (in which case
            the libs/ signature still matters -- it's just marshaled
            differently), or (b) be fully self-contained (the libs/
            function, if it exists, is dead for this slot). Both sub-cases
            are reported; see `override_calls_libs_fn`.
  3. fn is `<imp-prefix><Name>` (the generated marshaling wrapper)
         -> HLE-DISPATCHED: the generator's mechanical convention (verify
            against your own --bridges file) is that `<imp-prefix><Name>`
            ALWAYS calls the libs/ C function named `<Name>` directly (e.g.
            with the default prefix, yz_imp_cellFsClose calls cellFsClose()
            from libs/filesystem/cellFs.c). This is the one case where a
            libs/ signature mismatch is a real, live ABI bug.
  4. fn is `<imp-prefix>stub_<module>_<hex>`
         -> HLE-DISPATCHED-STUB: a self-contained CELL_ENOSYS/CELL_OK stub
            (confirmed by reading several bodies -- none call any libs/
            function). Even if libs/ happens to have a same-named function,
            it is NOT what runs for this slot -- folded into SHADOWED.
  (not found in the bridge table at all)
         -> UNKNOWN: this PS3 export is not in the game's import table, i.e.
            not imported by this game build -- the mismatch, live or not,
            can never fire.

Ground-truth sanity check, this repo's own game project only (skipped, with
a note, against any other --bridges file): cellGcmGetTiledPitchSize MUST
classify as SHADOWED (LLE) -- see yakuza/import_bridges_gen.cpp:1864,
`{ 0x010F3D2Cu, nullptr, 0x0210B944u }`.

Stdlib-only, py -3, LF line endings, 4-space indent.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from dataclasses import dataclass, field

import hle_abi_audit as audit


_IDENT = r"[A-Za-z_][A-Za-z0-9_]*"

# One yz_imports[] table row, e.g.:
#   { 0x010F3D2Cu, nullptr, 0x0210B944u }, /* cellGcmSys::cellGcmGetTiledPitchSize -> LLE */
#   { 0x010F308Cu, yz_imp_cellFsClose, 0 }, /* sys_fs::cellFsClose */
_BRIDGE_ROW_RE = re.compile(
    r"\{\s*(?P<nid>0x[0-9A-Fa-f]+)u?\s*,\s*"
    r"(?P<fn>nullptr|" + _IDENT + r")\s*,\s*"
    r"(?P<lle>0x[0-9A-Fa-f]+|0)u?\s*\}\s*,"
    r"\s*/\*\s*(?P<module>" + _IDENT + r")::(?P<name>" + _IDENT + r")\b"
)

def _ovr_def_re(ovr_prefix: str) -> re.Pattern:
    """<ovr-prefix><Name> function bodies in --overrides, so we can test
    whether an override delegates to the libs/ function of the same name."""
    return re.compile(
        r"^\s*(?:extern\s+\"C\"\s+)?(?:static\s+)?\w[\w\s\*]*?\b(?P<sym>"
        + re.escape(ovr_prefix) + _IDENT + r")\s*\(\s*ppu_context\s*\*[^)]*\)\s*\{",
        re.MULTILINE,
    )


@dataclass
class DispatchInfo:
    name: str                  # PS3 export name, e.g. "cellFsClose"
    module: str                # e.g. "sys_fs"
    nid: str
    fn: str                    # "nullptr" | "yz_imp_X" | "yz_ovr_X" | "yz_imp_stub_..."
    lle_opd: str
    kind: str = ""             # "LLE-DISPATCHED" | "OVERRIDE-WRAPPED" | "HLE-DISPATCHED" | "HLE-DISPATCHED-STUB"
    override_calls_libs_fn: object = None  # None (n/a) | True | False
    all_rows: list = field(default_factory=list)  # every raw row seen for this name (dup NIDs)


def _classify_row(fn: str, lle_opd: str, imp_prefix: str, ovr_prefix: str) -> str:
    lle_nonzero = lle_opd not in ("0", "0x0", "0x00000000")
    if fn == "nullptr":
        if lle_nonzero:
            return "LLE-DISPATCHED"
        # fn==nullptr and lle==0 shouldn't occur in practice (unbound slot);
        # keep a distinct label so it's visible rather than silently folded
        # into another bucket.
        return "UNBOUND-NULL"
    if fn.startswith(ovr_prefix):
        return "OVERRIDE-WRAPPED"
    if fn.startswith(imp_prefix + "stub_"):
        return "HLE-DISPATCHED-STUB"
    if fn.startswith(imp_prefix):
        return "HLE-DISPATCHED"
    # Unrecognized naming convention -- don't silently misclassify.
    return "UNRECOGNIZED-FN:" + fn


def parse_bridge_table(bridges_path: str, imp_prefix: str, ovr_prefix: str) -> dict:
    """Returns name -> DispatchInfo. If multiple rows share a name (a PS3
    export imported into more than one library-linkage slot -- confirmed to
    happen for a handful of sysPrxForUser sys_* functions, always with
    matching dispatch across all rows), the rows are merged; a name whose
    rows DISAGREE in dispatch kind is flagged (kind = "AMBIGUOUS:...") so it
    is never silently misreported.
    """
    with open(bridges_path, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()

    by_name: dict = {}
    for m in _BRIDGE_ROW_RE.finditer(text):
        name = m.group("name")
        fn = m.group("fn")
        lle = m.group("lle")
        kind = _classify_row(fn, lle, imp_prefix, ovr_prefix)
        row = (m.group("nid"), fn, lle, kind)
        if name not in by_name:
            by_name[name] = DispatchInfo(
                name=name, module=m.group("module"), nid=m.group("nid"),
                fn=fn, lle_opd=lle, kind=kind, all_rows=[row],
            )
        else:
            info = by_name[name]
            info.all_rows.append(row)
            if info.kind != kind:
                info.kind = f"AMBIGUOUS:{info.kind}+{kind}"
    return by_name


def scan_override_bodies(overrides_path: str, ovr_prefix: str) -> dict:
    """Returns <ovr-prefix><Name> -> True/False: does the override body call
    the libs/ C function `<Name>(...)` (same bare name, i.e. delegates to the
    HLE impl the audit compared) or not (fully self-contained / calls a
    DIFFERENT libs/ function / calls none)?

    Method: slice each <ovr-prefix><Name> function body (brace-matched) and
    search for a call to the identifier `<Name>` immediately followed by
    '(' -- i.e. `Name(...)`, not `<ovr-prefix>Name(...)` or some other
    symbol. This is a textual heuristic, not a semantic one -- labeled as
    such in the report.
    """
    with open(overrides_path, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()
    text_nc = re.sub(r"/\*.*?\*/", lambda mm: re.sub(r"[^\n]", " ", mm.group(0)), text, flags=re.DOTALL)
    text_nc = re.sub(r"//[^\n]*", "", text_nc)

    result = {}
    for m in _ovr_def_re(ovr_prefix).finditer(text_nc):
        sym = m.group("sym")
        bare_name = sym[len(ovr_prefix):]
        # one-line body (has both { and } on this match line's tail) --
        # find_matching_brace handles both one-liners and multi-line.
        open_brace = text_nc.find("{", m.end() - 1)
        if open_brace == -1:
            continue
        close_brace = audit.find_matching_brace(text_nc, open_brace)
        body = text_nc[open_brace:close_brace + 1]
        call_re = re.compile(r"(?<![A-Za-z0-9_])" + re.escape(bare_name) + r"\s*\(")
        result[bare_name] = bool(call_re.search(body))
    return result


# ---------------------------------------------------------------------------
# Bucketing
# ---------------------------------------------------------------------------

BUCKET_LIVE = "LIVE"
BUCKET_SHADOWED = "SHADOWED"
BUCKET_UNKNOWN = "UNKNOWN"


def bucket_for(name: str, dispatch: dict, override_delegates: dict) -> tuple:
    """Returns (bucket, reason_str, dispatch_info_or_None)."""
    info = dispatch.get(name)
    if info is None:
        return BUCKET_UNKNOWN, "not present in yakuza/import_bridges_gen.cpp yz_imports[] -- not imported by this game build", None

    kind = info.kind
    if kind == "LLE-DISPATCHED":
        return BUCKET_SHADOWED, f"LLE-DISPATCHED: NID {info.nid} binds to lifted-firmware OPD {info.lle_opd}; libs/ impl never called", info
    if kind == "HLE-DISPATCHED-STUB":
        return BUCKET_SHADOWED, f"HLE-DISPATCHED-STUB: import slot runs a self-contained CELL_ENOSYS/CELL_OK stub ({info.fn}), not the libs/ function", info
    if kind == "OVERRIDE-WRAPPED":
        delegates = override_delegates.get(name)
        if delegates is True:
            return BUCKET_LIVE, f"OVERRIDE-WRAPPED but override {info.fn} DELEGATES to libs/ {name}(...) internally -- libs/ signature still reachable (mismatch may still be live, just via the override's own marshaling)", info
        elif delegates is False:
            return BUCKET_SHADOWED, f"OVERRIDE-WRAPPED: {info.fn} is self-contained (does not call libs/ {name}); libs/ impl bypassed", info
        else:
            return BUCKET_SHADOWED, f"OVERRIDE-WRAPPED: {info.fn} shadows this slot (override body not found to confirm delegation -- treated conservatively as shadowing)", info
    if kind == "HLE-DISPATCHED":
        return BUCKET_LIVE, f"HLE-DISPATCHED: import slot's {info.fn} calls libs/ {name}(...) directly (gen_imports.py mechanical convention)", info
    if kind.startswith("AMBIGUOUS"):
        return BUCKET_UNKNOWN, f"AMBIGUOUS dispatch across duplicate NIDs for this name ({kind}) -- rows: {info.all_rows}", info
    if kind == "UNBOUND-NULL":
        return BUCKET_UNKNOWN, "fn==nullptr but lle_opd==0 (unbound slot) -- not a normal dispatch state", info
    return BUCKET_UNKNOWN, f"unrecognized dispatch shape: {kind}", info


# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------

def build_classified_report(libs_root: str, oracle_root: str, whitelist_path: str,
                             bridges_path: str, overrides_path: str,
                             imp_prefix: str, ovr_prefix: str) -> dict:
    base = audit.build_report(libs_root, oracle_root, whitelist_path)
    dispatch = parse_bridge_table(bridges_path, imp_prefix, ovr_prefix)
    override_delegates = scan_override_bodies(overrides_path, ovr_prefix)

    # One classification per function NAME (a name can have multiple
    # mismatch findings/"kinds"; the dispatch classification is per-name,
    # not per-finding).
    names = sorted({m.name for m in base["mismatches"]})
    classified = {}
    for name in names:
        bucket, reason, info = bucket_for(name, dispatch, override_delegates)
        classified[name] = (bucket, reason, info)

    live_names = [n for n in names if classified[n][0] == BUCKET_LIVE]
    shadowed_names = [n for n in names if classified[n][0] == BUCKET_SHADOWED]
    unknown_names = [n for n in names if classified[n][0] == BUCKET_UNKNOWN]

    return {
        "base": base,
        "dispatch": dispatch,
        "override_delegates": override_delegates,
        "classified": classified,
        "live_names": live_names,
        "shadowed_names": shadowed_names,
        "unknown_names": unknown_names,
    }


def _imported(name: str, dispatch: dict) -> bool:
    return name in dispatch


def format_classified_report(rep: dict) -> str:
    lines = []
    base = rep["base"]
    classified = rep["classified"]

    lines.append("=" * 78)
    lines.append("DISPATCH-LAYER CLASSIFICATION of hle_abi_audit.py mismatches")
    lines.append("=" * 78)
    lines.append(f"total mismatched functions (raw, all buckets): "
                 f"{len({m.name for m in base['mismatches']})}")
    lines.append(f"  LIVE       (HLE-dispatched, real signature mismatch): {len(rep['live_names'])}")
    lines.append(f"  SHADOWED   (LLE-dispatched or override-bypassed, libs/ dead for this slot): {len(rep['shadowed_names'])}")
    lines.append(f"  UNKNOWN    (not in yz_imports[] / ambiguous dispatch): {len(rep['unknown_names'])}")
    lines.append("")

    # Sanity check the task explicitly asks for.
    sanity_name = "cellGcmGetTiledPitchSize"
    if sanity_name in classified:
        bucket, reason, info = classified[sanity_name]
        lines.append(f"SANITY CHECK: {sanity_name} -> {bucket}  ({reason})")
        lines.append("  PASS (expected SHADOWED/LLE)" if bucket == BUCKET_SHADOWED else "  *** FAIL: expected SHADOWED -- classifier is WRONG ***")
    else:
        lines.append(f"SANITY CHECK: {sanity_name} not found among mismatches (no ABI mismatch flagged for it currently)")
    lines.append("")

    # ---- LIVE bug list ----
    lines.append("=" * 78)
    lines.append("LIVE bugs (HLE-dispatched; libs/ signature mismatch is real)")
    lines.append("=" * 78)
    mism_by_name: dict = {}
    for m in base["mismatches"]:
        mism_by_name.setdefault(m.name, []).append(m)

    for name in sorted(rep["live_names"], key=lambda n: (audit.module_rank(mism_by_name[n][0].module), n)):
        bucket, reason, info = classified[name]
        imported = _imported(name, rep["dispatch"])
        ms = mism_by_name[name]
        lines.append("")
        lines.append(f"-- {name} (module {ms[0].module}) --  imported={imported}")
        lines.append(f"   dispatch: {reason}")
        for m in ms:
            lines.append(f"   [{m.kind}] ours: {m.ours.pretty()}")
            lines.append(f"             ({m.ours.file}:{m.ours.line})")
            lines.append(f"             RPCS3: {m.theirs.pretty()}")
            lines.append(f"             ({m.theirs.file}:{m.theirs.line})")
            lines.append(f"             reason: {m.detail}")
    lines.append("")

    # ---- SHADOWED examples (prove classifier works) ----
    lines.append("=" * 78)
    lines.append("SHADOWED examples (first 15, incl. cellGcm* sanity set)")
    lines.append("=" * 78)
    shown = 0
    gcm_shown = []
    others_shown = []
    for name in rep["shadowed_names"]:
        bucket, reason, info = classified[name]
        if name.startswith("cellGcm") and len(gcm_shown) < 6:
            gcm_shown.append((name, reason))
        elif len(others_shown) < 9:
            others_shown.append((name, reason))
    for name, reason in gcm_shown + others_shown:
        lines.append(f"  {name}: {reason}")
    lines.append("")

    # ---- UNKNOWN ----
    lines.append("=" * 78)
    lines.append(f"UNKNOWN ({len(rep['unknown_names'])}) -- not imported by this game build, or ambiguous")
    lines.append("=" * 78)
    for name in rep["unknown_names"][:20]:
        bucket, reason, info = classified[name]
        lines.append(f"  {name}: {reason}")
    if len(rep["unknown_names"]) > 20:
        lines.append(f"  ... and {len(rep['unknown_names']) - 20} more")

    return "\n".join(lines)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    here = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.dirname(here)
    parser.add_argument("--libs", default=os.path.join(repo_root, "libs"))
    parser.add_argument("--oracle", default=os.path.join(repo_root, "rpcs3", "rpcs3", "Emu", "Cell", "Modules"))
    parser.add_argument("--whitelist", default=os.path.join(here, "hle_abi_audit_whitelist.txt"))
    parser.add_argument("--bridges", default=os.path.join(repo_root, "yakuza", "import_bridges_gen.cpp"))
    parser.add_argument("--overrides", default=os.path.join(repo_root, "yakuza", "import_overrides.cpp"))
    parser.add_argument("--imp-prefix", default="yz_imp_",
                         help="generated marshaling-wrapper symbol prefix in "
                              "--bridges (default: yz_imp_, this repo's own "
                              "game project's convention)")
    parser.add_argument("--ovr-prefix", default="yz_ovr_",
                         help="hand-written override-wrapper symbol prefix in "
                              "--bridges/--overrides (default: yz_ovr_, this "
                              "repo's own game project's convention)")
    args = parser.parse_args(argv)

    for p in (args.libs, args.oracle):
        if not os.path.isdir(p):
            print(f"error: not found: {p}", file=sys.stderr)
            return 2
    for p in (args.bridges, args.overrides):
        if not os.path.isfile(p):
            print(f"error: not found: {p}", file=sys.stderr)
            return 2

    rep = build_classified_report(args.libs, args.oracle, args.whitelist, args.bridges,
                                   args.overrides, args.imp_prefix, args.ovr_prefix)
    print(format_classified_report(rep))
    return 0


if __name__ == "__main__":
    sys.exit(main())
