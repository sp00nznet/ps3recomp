#!/usr/bin/env python3
"""ps3_recomp_harness.py -- batch-run the ps3recomp analysis pipeline across a
title library, the same way tools/tools/harness/recomp_harness.py does for the
360 side. The point is *generalizing*: running extract -> profile -> functions
(-> lift) over many titles turns one-off observations into a compatibility
matrix and a catalog of recurring failure modes -- which is what actually
hardens the toolkit (and tells us which NID stubs to ship next).

Two input sources, because PS3 content reaches us at two very different stages:

  * --archive-root DIR  A directory of packaged titles (*.rar). No default --
                     PSN packages are NPDRM-encrypted and frequently multi-GB, so
                     we do NOT decrypt them wholesale. Instead we catalog every
                     title cheaply from its filename (which encodes the title-id
                     and region, e.g. "... [NPUZ-00083].rar") and, for a small
                     --probe sample, extract just the PKG header (plaintext) to
                     confirm content-id / item-count / size.

  * --elf-root DIR   One or more directories of *already-decrypted* PS3 binaries
                     (EBOOT.elf / *.elf / *.self). These get the real recomp
                     triage: elf_parser profile, find_functions, and (opt-in)
                     the full ppu_lifter. *.self inputs are decrypted to *.elf
                     first via ps3sce when available.

Tiers (cost grows steeply; triage is cheap and scales):
  1 catalog  : filename -> title-id/region/category; optional PKG-header probe
  2 decrypt  : *.self -> *.elf via ps3sce (only needed for encrypted inputs)
  3 profile  : elf_parser.py --json  -> machine, entry, image base, segments
  4 functions: find_functions.py     -> function count, .opd-seeded starts
  5 lift     : ppu_lifter.py          -> generated C, giant functions (OPT-IN)

Each title is isolated, timed and resumable; one title's failure never aborts
the batch. Per-title results land in <out>/results/<id>.json. `report`
aggregates them into REPORT.md.

Usage:
  python ps3_recomp_harness.py catalog --archive-root <dir> [--probe 10]
  python ps3_recomp_harness.py analyze --elf-root D:\\recomp\\ps3games --max-tier 4
  python ps3_recomp_harness.py analyze --elf-root D:\\recomp\\ps3games\\scout --max-tier 5
  python ps3_recomp_harness.py report
"""

import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from collections import Counter
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent.parent          # the tools/ dir
ELF_PARSER = TOOLS_DIR / "elf_parser.py"
FIND_FUNCS = TOOLS_DIR / "find_functions.py"
PPU_LIFTER = TOOLS_DIR / "ppu_lifter.py"
PPU_LOADER = TOOLS_DIR / "ppu_loader.py"
GHIDRA_ANALYZE = TOOLS_DIR / "ghidra_analyze.py"
IDA_ANALYZE = TOOLS_DIR / "ida_analyze.py"

# Machine-specific locations. Override any of these with the matching
# PS3RECOMP_* environment variable (or the corresponding command-line flag)
# rather than editing this file -- the defaults are deliberately generic so
# the harness carries no one developer's directory layout.
DEFAULTS = {
    "psn_root":   os.environ.get("PS3RECOMP_ARCHIVE_ROOT",
                              os.environ.get("PS3RECOMP_PSN_ROOT", "")),
    "out":        os.environ.get("PS3RECOMP_OUT", str(TOOLS_DIR.parent / "_harness")),
    "sevenzip":   os.environ.get("PS3RECOMP_7ZIP", "7z"),
    "ps3sce":     os.environ.get("PS3RECOMP_PS3SCE", "ps3sce"),
    # IDA's idalib lives in whichever Python IDA was built against (IDA Pro
    # 9.1 -> 3.11); point PS3RECOMP_IDA_PYTHON at that interpreter.
    "ida_python": os.environ.get("PS3RECOMP_IDA_PYTHON", sys.executable),
}

# A PSN archive name carries the title-id and region:  "Name PSN [NPUZ-00401].rar"
TITLEID_RE = re.compile(r"\[([A-Z]{4})[-_ ]?(\d{4,5})\]")
# Coarse meaning of the 4-letter PSN content prefix (first two = region/SCE arm,
# fourth letter = rough content class). Used only for distribution buckets.
REGION_OF = {"NPU": "US (SCEA)", "NPE": "EU (SCEE)", "NPJ": "JP (SCEJ)",
             "NPH": "Asia (SCEH)", "NPK": "Korea", "NPG": "JP", "NPI": "Intl"}
CLASS_OF = {"A": "App/utility", "B": "PSN full game", "C": "PSN game",
            "F": "PSN game", "G": "PSN game/port", "H": "App",
            "I": "Disc-linked", "J": "PS1/minis", "Z": "Demo/minis"}


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def run(cmd, timeout, cwd=None):
    """Run a command, never raising. Returns (rc, stdout, stderr, seconds)."""
    t0 = time.time()
    try:
        p = subprocess.run([str(c) for c in cmd], cwd=cwd, timeout=timeout,
                           capture_output=True, text=True, errors="replace")
        return p.returncode, p.stdout or "", p.stderr or "", time.time() - t0
    except subprocess.TimeoutExpired as e:
        out = e.stdout if isinstance(e.stdout, str) else ""
        return 124, out or "", "TIMEOUT", time.time() - t0
    except Exception as e:                       # harness must survive anything
        return -1, "", f"{type(e).__name__}: {e}", time.time() - t0


def slug(s: str) -> str:
    return re.sub(r"[^a-z0-9]+", "", s.lower()) or "untitled"


# --------------------------------------------------------------------- catalog

def parse_titleid(name: str):
    m = TITLEID_RE.search(name)
    if not m:
        return None, None, None
    prefix, num = m.group(1), m.group(2)
    title_id = f"{prefix}{num}"
    region = REGION_OF.get(prefix[:3], "Unknown")
    cls = CLASS_OF.get(prefix[3], "Unknown")
    return title_id, region, cls


def parse_pkg_header(data: bytes) -> dict:
    """Parse the plaintext PS3 .pkg header (no decryption required)."""
    if len(data) < 0x80 or data[:4] != b"\x7fPKG":
        return {"ok": False, "error": "not a PKG"}
    rev, ptype = struct.unpack_from(">HH", data, 4)
    item_count = struct.unpack_from(">I", data, 0x14)[0]
    total_size = struct.unpack_from(">Q", data, 0x18)[0]
    content_id = data[0x30:0x30 + 0x24].split(b"\x00")[0].decode("ascii", "replace")
    return {"ok": True,
            "pkg_revision": f"0x{rev:04X}",
            "finalized": bool(rev & 0x8000),
            "pkg_type": ptype,
            "item_count": item_count,
            "total_size": total_size,
            "content_id": content_id}


def probe_pkg_header(rar: Path, cfg) -> dict:
    """Extract the nested PKG far enough to read its plaintext 0x80-byte header.

    PSN rars nest: outer.rar -> inner.rar -> *.pkg. Extraction is purged right
    after; for huge titles this still writes the full pkg once, so probing is
    deliberately limited to a small sample via --probe.
    """
    tmp = Path(cfg.out) / "_probe" / slug(rar.stem)
    shutil.rmtree(tmp, ignore_errors=True)
    tmp.mkdir(parents=True, exist_ok=True)
    try:
        rc, _, err, _ = run([cfg.sevenzip, "x", rar, f"-o{tmp}", "-y"], timeout=cfg.t_extract)
        if rc != 0:
            return {"ok": False, "error": f"7z outer rc={rc}"}
        inner = next((p for p in tmp.rglob("*.rar")), None)
        if inner:
            run([cfg.sevenzip, "x", inner, f"-o{tmp / 'inner'}", "-y"], timeout=cfg.t_extract)
        pkg = next((p for p in tmp.rglob("*.pkg")), None) or next((p for p in tmp.rglob("*.PKG")), None)
        if not pkg:
            return {"ok": False, "error": "no pkg found"}
        with open(pkg, "rb") as f:
            return parse_pkg_header(f.read(0x100))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def cmd_catalog(cfg):
    root = Path(cfg.psn_root)
    rars = sorted(p for p in root.rglob("*.rar"))
    log(f"{len(rars)} PSN archives under {root}")
    results_dir = Path(cfg.out) / "results"
    results_dir.mkdir(parents=True, exist_ok=True)

    # cheap pass over everything: filename -> title-id/region/class + size
    catalog = []
    for rar in rars:
        title_id, region, cls = parse_titleid(rar.name)
        catalog.append({"name": rar.stem, "title_id": title_id, "region": region,
                        "content_class": cls, "rar_mb": round(rar.stat().st_size / 1048576, 1),
                        "lib": rar.parent.name})
    (Path(cfg.out) / "catalog.json").write_text(json.dumps(catalog, indent=2))

    # optional: enrich the N smallest titles with a real PKG-header probe
    if cfg.probe:
        sample = sorted(rars, key=lambda p: p.stat().st_size)[:cfg.probe]
        log(f"probing PKG headers of {len(sample)} smallest titles")
        for i, rar in enumerate(sample, 1):
            hdr = probe_pkg_header(rar, cfg)
            tid, region, cls = parse_titleid(rar.name)
            rec = {"title": rar.stem, "id": slug(rar.stem), "source": "psn",
                   "title_id": tid, "region": region, "content_class": cls,
                   "rar_mb": round(rar.stat().st_size / 1048576, 1),
                   "ts": time.strftime("%Y-%m-%d %H:%M:%S"),
                   "stages": {"catalog": {"status": "ok" if hdr.get("ok") else "fail", **hdr}}}
            (results_dir / f"cat_{slug(rar.stem)}.json").write_text(json.dumps(rec, indent=2))
            log(f"  [{i}/{len(sample)}] {rar.stem}: {hdr.get('content_id', hdr.get('error'))}")
    cmd_report(cfg)


# --------------------------------------------------------------------- analyze

def stage_decrypt(binp: Path, work: Path, cfg) -> dict:
    """If the input is an encrypted SELF, decrypt to ELF via ps3sce."""
    r = {"status": "ok", "elf": str(binp)}
    head = binp.read_bytes()[:4] if binp.stat().st_size >= 4 else b""
    if head == b"\x7fELF":
        return r                                   # already an ELF
    if head != b"SCE\x00":
        r.update(status="fail", error=f"unknown magic {head!r}")
        return r
    sce = Path(cfg.ps3sce)
    exe = sce if sce.exists() else Path(str(sce) + ".exe")
    if not exe.exists():
        r.update(status="fail", error="ps3sce not found")
        return r
    out_elf = work / (binp.stem + ".dec.elf")
    rc, _, err, dur = run([exe, "-d", binp, out_elf], timeout=cfg.t_decrypt, cwd=str(exe.parent))
    r["decrypt_s"] = round(dur, 1)
    if rc == 0 and out_elf.exists() and out_elf.read_bytes()[:4] == b"\x7fELF":
        r["elf"] = str(out_elf)
    else:
        r.update(status="fail", error=f"ps3sce rc={rc}: {err.strip()[-160:]}")
    return r


def stage_profile(elf: Path, cfg) -> dict:
    r = {"status": "fail"}
    rc, out, err, dur = run([sys.executable, ELF_PARSER, elf, "--json", "--segments"],
                           timeout=cfg.t_profile)
    if rc != 0:
        r["error"] = f"elf_parser rc={rc}: {err.strip()[-160:]}"
        return r
    try:
        d = json.loads(out)
    except json.JSONDecodeError:
        r["error"] = "elf_parser produced no JSON"
        return r
    eh = d.get("elf_header", {})
    phs = d.get("program_headers", [])
    # Only real (non-empty) PT_LOAD segments define the image; PS3 ELFs carry
    # several placeholder PT_LOADs at vaddr 0 / memsz 0 that must not drag the
    # computed base down to 0x0.
    loads = [p for p in phs if p.get("type") == "PT_LOAD" and int(p.get("memsz", "0x0"), 16) > 0]
    bases = [int(p["vaddr"], 16) for p in loads]
    MACHINE = {"0x0014": "PPC", "0x0015": "PPC64", "0x0017": "SPU"}
    ETYPE = {"0x0002": "EXEC", "0x0003": "DYN", "0xffa4": "PRX", "0xffa5": "PRX"}
    r.update(status="ok",
             machine=MACHINE.get(eh.get("machine"), eh.get("machine")),
             elf_type=ETYPE.get(str(eh.get("type")).lower(), eh.get("type")),
             endian=eh.get("endian"), entry=eh.get("entry"),
             image_base=(f"0x{min(bases):08X}" if bases else None),
             n_segments=len(phs), n_load=len(loads),
             mem_kb=round(sum(int(p.get("memsz", "0x0"), 16) for p in loads) / 1024, 1))
    return r


def stage_functions(elf: Path, work: Path, cfg) -> dict:
    r = {"status": "fail"}
    ff = work / "functions.json"
    rc, out, err, dur = run([sys.executable, FIND_FUNCS, elf, "--json", "-o", ff],
                           timeout=cfg.t_functions)
    r["functions_s"] = round(dur, 1)
    blob = out + "\n" + err
    if rc != 0 or not ff.exists():
        r["error"] = f"find_functions rc={rc}: {err.strip()[-160:]}"
        return r
    try:
        funcs = json.loads(ff.read_text())
    except json.JSONDecodeError:
        r["error"] = "functions.json unreadable"
        return r
    r["n_functions"] = len(funcs)
    for key, pat in [("n_instructions", r"disassembled (\d+) instructions"),
                     ("opd_seeded", r"seed pass: \+(\d+) seeded"),
                     ("prologue", r"prologue pass: (\d+) functions"),
                     ("branch_target", r"branch-target pass: (\d+) functions")]:
        m = re.search(pat, blob)
        if m:
            r[key] = int(m.group(1))
    r["opd_verified"] = "every .opd descriptor address is a function start" in blob
    r["status"] = "ok"
    r["functions_path"] = str(ff)
    return r


def stage_lift(elf: Path, ff_path: str, work: Path, cfg) -> dict:
    r = {"status": "fail"}
    out_dir = work / "lifted"
    shutil.rmtree(out_dir, ignore_errors=True)
    out_dir.mkdir(parents=True, exist_ok=True)
    cmd = [sys.executable, PPU_LIFTER, elf, "-o", out_dir, "-j", str(cfg.jobs)]
    if ff_path:
        cmd += ["--functions", ff_path]
    rc, out, err, dur = run(cmd, timeout=cfg.t_lift)
    blob = out + "\n" + err
    r["lift_s"] = round(dur, 1)
    chunks = list(out_dir.glob("*.c")) + list(out_dir.glob("*.cpp"))
    giants = re.findall(r"(0x[0-9A-Fa-f]+).{0,40}?(\d{5,}) bytes", blob)
    if giants:
        r["giant_functions"] = [{"addr": a, "bytes": int(b)} for a, b in giants[:10]]
    m = re.search(r"(\d+) functions lifted", blob)
    if m:
        r["n_lifted"] = int(m.group(1))
    if rc == 0 and chunks:
        r.update(status="ok", n_chunks=len(chunks),
                 c_mb=round(sum(c.stat().st_size for c in chunks) / 1048576, 1))
    else:
        r["error"] = f"ppu_lifter rc={rc}: {err.strip()[-200:]}"
    if not cfg.keep_lifted:
        shutil.rmtree(out_dir, ignore_errors=True)
    return r


_NIDDB = None


def _niddb():
    """Lazily build a NID->name resolver from nid_database's builtins."""
    global _NIDDB
    if _NIDDB is None:
        if str(TOOLS_DIR) not in sys.path:
            sys.path.insert(0, str(TOOLS_DIR))
        try:
            from nid_database import NIDDatabase
            db = NIDDatabase()
            db.load_builtins()
        except Exception:                          # resolver is optional
            db = False
        _NIDDB = db
    return _NIDDB or None


def stage_imports(elf: Path, work: Path, cfg) -> dict:
    """Extract the EBOOT's firmware imports (proc_prx_param -> libstub table) via
    ppu_loader, and resolve NIDs to names where known. Non-gating: a binary with
    no import table (or a PRX) simply yields nothing and the pipeline continues.
    """
    r = {"status": "fail", "gating": False}
    out_dir = work / "loader"
    out_dir.mkdir(parents=True, exist_ok=True)
    rc, out, err, _ = run([sys.executable, PPU_LOADER, elf, "-o", out_dir], timeout=cfg.t_profile)
    imp_json = out_dir / (elf.stem + ".imports.json")
    if rc != 0 or not imp_json.exists():
        r["error"] = f"ppu_loader rc={rc}: {err.strip()[-160:]}"
        return r
    try:
        imps = json.loads(imp_json.read_text())
    except json.JSONDecodeError:
        r["error"] = "imports.json unreadable"
        return r
    db = _niddb()
    resolved = []
    for im in imps:
        nidv = im.get("nid")
        try:
            nid = int(nidv, 16) if isinstance(nidv, str) else int(nidv)
        except (TypeError, ValueError):
            nid = None
        name = None
        if db is not None and nid is not None:
            hit = db.lookup_nid(nid)
            if hit:
                name = hit[1]
        resolved.append({"library": im.get("library"), "nid": nidv, "name": name})
    libs = sorted({im["library"] for im in resolved if im.get("library")})
    r.update(status="ok", n_imports=len(resolved), n_libraries=len(libs),
             n_resolved=sum(1 for im in resolved if im["name"]), imports=resolved)
    return r


def _norm_addr(a):
    return int(a, 16) if isinstance(a, str) else int(a)


def _oracle_functions(name, elf, work, cfg):
    """Run one external oracle (Ghidra or IDA) and return (set_of_addrs, seconds,
    error). Import thunks are dropped — they're stub trampolines, not real
    functions our lifter must cover."""
    odir = work / name
    shutil.rmtree(odir, ignore_errors=True)
    if name == "ghidra":
        cmd = [sys.executable, GHIDRA_ANALYZE, elf, "-o", odir, "--max-cpu", str(cfg.ghidra_cpu)]
    else:  # ida — must run under the interpreter idalib is installed in (3.11)
        cmd = [cfg.ida_python, IDA_ANALYZE, elf, "-o", odir]
    rc, out, err, dur = run(cmd, timeout=cfg.t_ghidra)
    fjson = odir / "functions.json"
    if rc != 0 or not fjson.exists():
        return None, round(dur, 1), f"{name} rc={rc}: {(err or out).strip()[-140:]}"
    try:
        data = json.loads(fjson.read_text())
    except (json.JSONDecodeError, OSError):
        return None, round(dur, 1), f"{name}: unreadable functions.json"
    addrs = {_norm_addr(x["addr"]) for x in data if not x.get("thunk")}
    if not cfg.keep_ghidra:
        shutil.rmtree(odir, ignore_errors=True)
    return addrs, round(dur, 1), None


def stage_crosscheck(elf: Path, work: Path, cfg, ff_path) -> dict:
    """Cross-check find_functions against one or two industrial disassemblers
    (Ghidra and/or IDA headless). Recall = how many of the oracle's functions we
    also find; the gap is address-taken / call-graph-recovered functions our
    detector misses. With both oracles, the set they AGREE on is high-confidence
    — functions we miss there are almost certainly real. Opt-in (top tier).
    """
    r = {"status": "fail"}
    if not ff_path or not Path(ff_path).exists():
        r["error"] = "no find_functions output (needs tier >= 4)"
        return r
    try:
        ours = {_norm_addr(f["start"]) for f in json.loads(Path(ff_path).read_text())}
    except (json.JSONDecodeError, OSError):
        r["error"] = "unreadable find_functions json"
        return r

    want = {"ghidra": ["ghidra"], "ida": ["ida"], "both": ["ghidra", "ida"]}[cfg.oracle]
    sets, errs = {}, []
    for name in want:
        addrs, secs, err = _oracle_functions(name, elf, work, cfg)
        r[f"{name}_s"] = secs
        if err:
            errs.append(err)
        else:
            sets[name] = addrs
            agree = ours & addrs
            r[f"recall_{name}_pct"] = round(100 * len(agree) / max(1, len(addrs)), 1)
            r[f"n_{name}"] = len(addrs)
    if not sets:
        r["error"] = "; ".join(errs) or "no oracle produced output"
        return r

    r["n_ours"] = len(ours)
    # union recall + (when both ran) the high-confidence both-agree recall
    union = set().union(*sets.values())
    r.update(status="ok", oracles=list(sets.keys()),
             n_union=len(union), n_missed=len(union - ours), n_extra=len(ours - union),
             recall_pct=round(100 * len(ours & union) / max(1, len(union)), 1))
    if len(sets) == 2:
        both = sets["ghidra"] & sets["ida"]
        r["n_both_agree"] = len(both)
        r["n_missed_high_conf"] = len(both - ours)
        r["recall_high_conf_pct"] = round(100 * len(ours & both) / max(1, len(both)), 1)
        miss = both - ours
    else:
        miss = union - ours
    if miss:
        r["missed_sample"] = [f"0x{a:08X}" for a in sorted(miss)[:12]]
    if errs:
        r["partial_error"] = "; ".join(errs)
    return r


STAGE_TIER = {"decrypt": 2, "profile": 3, "imports": 4, "functions": 4,
              "lift": 5, "crosscheck": 6}


def discover_binaries(roots):
    """Find decrypted/encrypted PS3 executables under the given roots.

    Prefer EBOOT.elf over EBOOT.BIN (same image, already decrypted) and avoid
    cataloging both. Skip obvious SPU/PRX-only artifacts handled elsewhere.
    """
    seen, out = set(), []
    pats = ["EBOOT.elf", "EBOOT.ELF", "*.elf", "EBOOT.BIN", "*.self", "*.SELF"]
    for root in roots:
        root = Path(root)
        for pat in pats:
            for p in root.rglob(pat):
                if not p.is_file():
                    continue
                # De-dup the same image across container dirs (a game's EBOOT
                # often exists in extracted/, USRDIR/, game/ at once): key on the
                # game root + stem + byte size, so identical copies collapse to
                # one while genuinely different binaries stay separate.
                key = (game_label(p).name, p.stem.replace(".dec", ""), p.stat().st_size)
                if key in seen:
                    continue
                # Prefer the decrypted .elf over its encrypted .bin/.self sibling
                # (same image; the .self merely adds the SCE wrapper).
                if p.suffix.lower() in (".bin", ".self") and (
                        p.with_suffix(".elf").exists()
                        or p.with_name(p.stem + ".elf").exists()):
                    continue
                seen.add(key)
                out.append(p)
    return out


GENERIC_DIRS = {"usrdir", "game", "input", "extracted", "ps3_game", "c00", "app_home", "ps3"}


def game_label(binp: Path) -> Path:
    """Walk up past generic container dirs (USRDIR/game/...) to the game root."""
    d = binp.parent
    while d.name.lower() in GENERIC_DIRS and d.parent != d:
        d = d.parent
    return d


def run_binary(binp: Path, cfg) -> dict:
    root = game_label(binp)
    tid = slug(f"{root.name}_{binp.parent.name}_{binp.stem}")
    work = Path(cfg.out) / "work" / tid
    work.mkdir(parents=True, exist_ok=True)
    res = {"title": f"{root.name}/{binp.name}", "id": tid, "source": "elf",
           "input": str(binp), "ts": time.strftime("%Y-%m-%d %H:%M:%S"),
           "stages": {}, "max_tier_reached": 0}

    dec = stage_decrypt(binp, work, cfg)
    res["stages"]["decrypt"] = dec
    if dec["status"] != "ok":
        res["blocked_at"] = "decrypt"
        return res
    res["max_tier_reached"] = 2
    elf = Path(dec["elf"])

    order = [("profile", lambda: stage_profile(elf, cfg)),
             ("imports", lambda: stage_imports(elf, work, cfg)),
             ("functions", lambda: stage_functions(elf, work, cfg))]
    if cfg.max_tier >= 5:
        order.append(("lift", lambda: stage_lift(
            elf, res["stages"].get("functions", {}).get("functions_path"), work, cfg)))
    if cfg.max_tier >= 6:
        order.append(("crosscheck", lambda: stage_crosscheck(
            elf, work, cfg, res["stages"].get("functions", {}).get("functions_path"))))

    for name, fn in order:
        if STAGE_TIER[name] > cfg.max_tier:
            continue
        out = fn()
        res["stages"][name] = out
        if out.get("status") == "ok":
            res["max_tier_reached"] = max(res["max_tier_reached"], STAGE_TIER[name])
        elif out.get("gating") is False:
            continue                               # best-effort stage (imports)
        else:
            res["blocked_at"] = name
            break
    return res


def cmd_analyze(cfg):
    roots = [r for r in (cfg.elf_root or []) if Path(r).exists()]
    if not roots:
        log("no --elf-root given or none exist; nothing to analyze")
        return
    bins = discover_binaries(roots)
    log(f"{len(bins)} binaries discovered under {len(roots)} root(s) | max-tier {cfg.max_tier}")
    results_dir = Path(cfg.out) / "results"
    results_dir.mkdir(parents=True, exist_ok=True)
    if cfg.limit:
        bins = bins[:cfg.limit]

    done = skipped = 0
    for i, binp in enumerate(bins, 1):
        tid = slug(f"{game_label(binp).name}_{binp.parent.name}_{binp.stem}")
        rj = results_dir / f"elf_{tid}.json"
        if rj.exists() and not cfg.force:
            try:
                prev = json.loads(rj.read_text())
                if prev.get("max_tier_reached", 0) >= cfg.max_tier or prev.get("blocked_at"):
                    skipped += 1
                    continue
            except json.JSONDecodeError:
                pass
        log(f"[{i}/{len(bins)}] {binp.parent.name}/{binp.name}")
        res = run_binary(binp, cfg)
        rj.write_text(json.dumps(res, indent=2))
        b = res.get("blocked_at", "-")
        log(f"    -> tier {res['max_tier_reached']}" + (f" (blocked at {b})" if b != "-" else " OK"))
        done += 1
    log(f"done: {done} processed, {skipped} skipped (resumed)")
    cmd_report(cfg)


# ---------------------------------------------------------------------- report

def cmd_report(cfg):
    out = []
    A = out.append
    A(f"# ps3recomp Harness Report\n\n_generated {time.strftime('%Y-%m-%d %H:%M')}_\n")

    # ---- PSN catalog (filename pass) ----
    cat_path = Path(cfg.out) / "catalog.json"
    if cat_path.exists():
        cat = json.loads(cat_path.read_text())
        A(f"## PSN library catalog\n\n_{len(cat)} archives scanned by filename (no extraction)._\n")
        regions = Counter(c["region"] for c in cat)
        classes = Counter(c["content_class"] for c in cat)
        libs = Counter(c["lib"] for c in cat)
        total_gb = round(sum(c["rar_mb"] for c in cat) / 1024, 1)
        named = sum(1 for c in cat if c["title_id"])
        A(f"- **Total archive size:** {total_gb} GB across {len(cat)} titles "
          f"({named} with a parseable title-id)")
        A(f"- **Sub-libraries:** " + ", ".join(f"{k} ({v})" for k, v in libs.most_common()))
        A("\n**Region distribution:**\n")
        A("| Region | # titles |\n|---|---:|")
        for k, v in regions.most_common():
            A(f"| {k} | {v} |")
        A("\n**Content-class distribution (PSN prefix 4th letter):**\n")
        A("| Class | # titles |\n|---|---:|")
        for k, v in classes.most_common():
            A(f"| {k} | {v} |")
        A("")

    rows = []
    for rj in sorted((Path(cfg.out) / "results").glob("*.json")):
        try:
            rows.append(json.loads(rj.read_text()))
        except json.JSONDecodeError:
            continue
    elf_rows = [r for r in rows if r.get("source") == "elf"]
    cat_rows = [r for r in rows if r.get("source") == "psn"]

    # ---- PKG-header probes ----
    if cat_rows:
        A(f"## PKG-header probes ({len(cat_rows)})\n")
        A("| Title | content-id | items | size (MB) |\n|---|---|---:|---:|")
        for r in cat_rows:
            c = r["stages"].get("catalog", {})
            A(f"| {r['title'][:40]} | {c.get('content_id', c.get('error', '?'))} "
              f"| {c.get('item_count', '?')} | {round(c.get('total_size', 0)/1048576, 1)} |")
        A("")

    # ---- decrypted-ELF recomp triage ----
    if elf_rows:
        n = len(elf_rows)
        A(f"## Decrypted-ELF recomp triage\n\n_{n} binaries._\n")

        def ok(stage):
            return sum(1 for r in elf_rows if r["stages"].get(stage, {}).get("status") == "ok")
        A("### Pipeline funnel\n")
        A("| Stage | Reached OK | % |\n|---|---:|---:|")
        for s in ["decrypt", "profile", "imports", "functions", "lift", "crosscheck"]:
            attempted = sum(1 for r in elf_rows if s in r["stages"])
            if attempted:
                A(f"| {s} | {ok(s)}/{attempted} | {round(100*ok(s)/n)}% |")
        A("")

        base_dist = Counter()
        machines = Counter()
        A("### Per-binary profile\n")
        A("| Binary | machine | image base | segs | functions | .opd seeded | imports (libs) | lift |\n"
          "|---|---|---|---:|---:|---:|---:|---|")
        for r in elf_rows:
            st = r["stages"]
            prof = st.get("profile", {})
            fn = st.get("functions", {})
            imp = st.get("imports", {})
            lift = st.get("lift", {})
            if prof.get("image_base"):
                base_dist[prof["image_base"]] += 1
            if prof.get("machine"):
                machines[prof["machine"]] += 1
            lift_cell = (f"{lift.get('n_chunks')} chunks/{lift.get('c_mb')}MB"
                         if lift.get("status") == "ok"
                         else (lift.get("error", "-")[:18] if lift else "-"))
            imp_cell = (f"{imp.get('n_imports')} ({imp.get('n_libraries')})"
                        if imp.get("status") == "ok" else "-")
            A(f"| {r['title'][:34]} | {prof.get('machine', '-')} | {prof.get('image_base', '-')} "
              f"| {prof.get('n_segments', '-')} "
              f"| {fn.get('n_functions', '-')} | {fn.get('opd_seeded', '-')} | {imp_cell} | {lift_cell} |")
        A("")

        # ---- cross-title import ranking (stub-prioritization) ----
        lib_titles = Counter()        # how many titles import each library
        fn_titles = Counter()         # how many titles import each function
        fn_label = {}                 # key -> human label
        for r in elf_rows:
            imp = r["stages"].get("imports", {})
            if imp.get("status") != "ok":
                continue
            seen_lib, seen_fn = set(), set()
            for im in imp.get("imports", []):
                lib = im.get("library") or "?"
                key = (lib, im.get("name") or im.get("nid"))
                seen_lib.add(lib)
                seen_fn.add(key)
                fn_label[key] = (f"{lib}::{im['name']}" if im.get("name")
                                 else f"{lib}::{im.get('nid')}")
            for l in seen_lib:
                lib_titles[l] += 1
            for k in seen_fn:
                fn_titles[k] += 1
        if lib_titles:
            A("### Top imported libraries (stub-prioritization)\n")
            A("_How many of the analyzed titles import each firmware library — the "
              "most common ones are the highest-value modules to support._\n")
            A("| Library | # titles |\n|---|---:|")
            for lib, c in lib_titles.most_common(30):
                A(f"| `{lib}` | {c} |")
            A("")
            A("### Top imported functions (stub-prioritization)\n")
            A("_Most-imported NIDs across titles (resolved to names where known). "
              "These are the highest-value stubs to implement next._\n")
            A("| Function (library::name/NID) | # titles |\n|---|---:|")
            for k, c in fn_titles.most_common(40):
                A(f"| `{fn_label[k]}` | {c} |")
            A("")
        if base_dist:
            A("### Image-base distribution\n")
            for b, c in base_dist.most_common():
                flag = "  <- non-standard" if b not in ("0x00010000",) else ""
                A(f"- `{b}`: {c}{flag}")
            A("")
        if machines:
            A("### Machine types\n")
            for m, c in machines.most_common():
                A(f"- {m}: {c}")
            A("")

        # ---- function-detection cross-check vs Ghidra ----
        cc_rows = [(r["title"], r["stages"]["crosscheck"]) for r in elf_rows
                   if r["stages"].get("crosscheck", {}).get("status") == "ok"]
        if cc_rows:
            tot_union = sum(c["n_union"] for _, c in cc_rows)
            tot_hit = sum(c["n_union"] - c["n_missed"] for _, c in cc_rows)
            has_both = any("recall_high_conf_pct" in c for _, c in cc_rows)
            A("### Function-detection cross-check (find_functions vs Ghidra/IDA)\n")
            A("_Recall = how many of the oracle's functions our detector also finds "
              "(a gap means address-taken / call-graph-recovered functions the lifter "
              "would miss — recoverable via `find_functions --seed-json`). `extra` = "
              "starts we find the oracle doesn't (real `.opd` functions it folded, or "
              "our false positives). With both oracles, **high-conf** recall is measured "
              "against the set Ghidra and IDA agree on._\n")
            A(f"- **Corpus recall (vs oracle union): {round(100*tot_hit/max(1,tot_union),1)}%** "
              f"({tot_hit}/{tot_union})\n")
            hdr = "| Binary | oracles | find_functions | union | missed | extra | recall |"
            sep = "|---|---|---:|---:|---:|---:|---:|"
            if has_both:
                hdr += " high-conf recall |"
                sep += "---:|"
            A(hdr + "\n" + sep)
            for t, c in cc_rows:
                row = (f"| {t[:28]} | {'+'.join(c.get('oracles', []))} | {c['n_ours']} "
                       f"| {c['n_union']} | {c['n_missed']} | {c['n_extra']} "
                       f"| {c.get('recall_pct', '-')}% |")
                if has_both:
                    row += (f" {c.get('recall_high_conf_pct', '-')}% "
                            f"({c.get('n_missed_high_conf', '-')} missed) |"
                            if "recall_high_conf_pct" in c else " - |")
                A(row)
            A("")
            worst = min(cc_rows, key=lambda tc: tc[1].get("recall_pct", 100))
            if worst[1].get("missed_sample"):
                lbl = ("both oracles agree, we didn't find"
                       if "recall_high_conf_pct" in worst[1] else "oracle found, we didn't")
                A(f"Sample functions missed on **{worst[0]}** ({lbl}): "
                  + ", ".join(f"`{a}`" for a in worst[1]["missed_sample"]) + "\n")

        # giant functions + lift failures
        giants = [(r["title"], st["lift"]["giant_functions"])
                  for r in elf_rows if (st := r["stages"]).get("lift", {}).get("giant_functions")]
        if giants:
            A("### Giant-function warnings (boundary mis-detection)\n")
            for t, gs in giants[:20]:
                A(f"- {t}: " + ", ".join(f"{g['addr']} ({g['bytes']//1024} KB)" for g in gs))
            A("")
        fails = [(r["title"], r["blocked_at"], r["stages"][r["blocked_at"]].get("error", "?"))
                 for r in elf_rows if r.get("blocked_at")]
        if fails:
            A(f"### Blocked binaries ({len(fails)})\n")
            for t, stage, err in fails[:30]:
                A(f"- **{t}** at *{stage}*: {err}")
            A("")

    report = Path(cfg.out) / "REPORT.md"
    report.parent.mkdir(parents=True, exist_ok=True)
    report.write_text("\n".join(out), encoding="utf-8")
    log(f"report -> {report}  ({len(elf_rows)} elf, {len(cat_rows)} probed)")


# ------------------------------------------------------------------------ main

def build_cfg(args):
    class C:
        pass
    c = C()
    for k, v in vars(args).items():
        setattr(c, k, v)
    for k, v in DEFAULTS.items():
        if getattr(c, k, None) is None:
            setattr(c, k, v)
    c.t_extract = 1200
    c.t_decrypt = 300
    c.t_profile = 120
    c.t_functions = 600
    c.t_lift = getattr(args, "t_lift", None) or 3600
    c.t_ghidra = getattr(args, "t_ghidra", None) or 1800
    if getattr(c, "ghidra_cpu", None) in (None, 0):
        c.ghidra_cpu = 4
    if not hasattr(c, "keep_ghidra"):
        c.keep_ghidra = False
    return c


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    pc = sub.add_parser("catalog", help="catalog a directory of packaged titles by filename (+optional probe)")
    pc.add_argument("--archive-root", dest="psn_root",
                    help="directory of packaged titles to catalog")
    # Kept so existing scripts keep working; --archive-root is the documented name.
    pc.add_argument("--psn-root", dest="psn_root", help=argparse.SUPPRESS)
    pc.add_argument("--probe", type=int, default=0, help="extract+read PKG header for N smallest titles")
    pc.add_argument("--out")
    pc.add_argument("--sevenzip")

    pa = sub.add_parser("analyze", help="profile/lift decrypted ELFs under --elf-root")
    pa.add_argument("--elf-root", dest="elf_root", action="append",
                    help="directory of decrypted PS3 binaries (repeatable)")
    pa.add_argument("--max-tier", dest="max_tier", type=int, default=4, choices=[2, 3, 4, 5, 6],
                    help="2 decrypt, 3 +profile, 4 +functions (default), 5 +lift, "
                         "6 +crosscheck (Ghidra recall, opt-in)")
    pa.add_argument("--limit", type=int)
    pa.add_argument("--force", action="store_true")
    pa.add_argument("--jobs", type=int, default=0, help="ppu_lifter --jobs (0 = its default)")
    pa.add_argument("--keep-lifted", dest="keep_lifted", action="store_true")
    pa.add_argument("--keep-ghidra", dest="keep_ghidra", action="store_true",
                    help="keep the Ghidra export dirs (tier 6)")
    pa.add_argument("--ghidra-cpu", dest="ghidra_cpu", type=int, default=4,
                    help="analyzeHeadless -max-cpu (tier 6)")
    pa.add_argument("--oracle", choices=["ghidra", "ida", "both"], default="ghidra",
                    help="tier-6 cross-check oracle(s) (default: ghidra). 'both' "
                         "reports a high-confidence recall vs the set Ghidra and IDA agree on")
    pa.add_argument("--ida-python", dest="ida_python",
                    help="interpreter idalib is installed in (default: Python 3.11)")
    pa.add_argument("--t-lift", dest="t_lift", type=int)
    pa.add_argument("--t-ghidra", dest="t_ghidra", type=int)
    pa.add_argument("--out")
    pa.add_argument("--ps3sce")

    rp = sub.add_parser("report", help="aggregate results into REPORT.md")
    rp.add_argument("--out")

    args = ap.parse_args()
    # default-fill fields a given subparser may not define
    for f, d in [("psn_root", None), ("probe", 0), ("elf_root", None), ("max_tier", 4),
                 ("limit", None), ("force", False), ("jobs", 0), ("keep_lifted", False),
                 ("t_lift", None), ("sevenzip", None), ("ps3sce", None),
                 ("keep_ghidra", False), ("ghidra_cpu", 4), ("t_ghidra", None),
                 ("oracle", "ghidra"), ("ida_python", None)]:
        if not hasattr(args, f):
            setattr(args, f, d)
    if getattr(args, "jobs", 0) in (0, None):
        args.jobs = ""  # let ppu_lifter pick CPU count; "" -> omitted? we pass int, so default below
        args.jobs = __import__("os").cpu_count() or 4
    cfg = build_cfg(args)
    {"catalog": cmd_catalog, "analyze": cmd_analyze, "report": cmd_report}[args.cmd](cfg)


if __name__ == "__main__":
    main()
