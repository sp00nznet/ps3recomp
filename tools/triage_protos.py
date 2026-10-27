"""triage_protos.py - one-line-per-title debug-vs-retail map of a proto/demo dump.

For a directory full of PS3 prototype archives and disc images, pull ONLY each title's
EBOOT (never the multi-GB assets) and report whether it is a debug/"fake" SELF -- the
unencrypted, usually-unstripped kind that carries symbols + DWARF (see unfself.py) -- or a
retail-encrypted SELF we can do nothing with. For the debug ones, count STT_FUNC symbols and
flag DWARF, so you know which titles are worth mining (nid_harvest / dwarf_abi / objdump_audit).

    python triage_protos.py W:/Roms/PS3_Protos
    python triage_protos.py <dir> --descend   # also crack open nested disc images (SLOW)

Container handling (cheapest path that yields the EBOOT):
  .iso/.gcm (standalone)  -> 7z reads ISO9660, extracts just EBOOT.BIN
  .7z/.zip/.rar           -> if a direct EBOOT.BIN is inside, extract only that;
                             if it wraps a disc image, DEFER (needs --descend) rather than
                             unpack gigabytes
  .pkg                    -> pkg_extract.py --only EBOOT.BIN (debug/retail PKG keys)

In-transit files (size still changing, or locked) are detected and skipped -- a partial
download would mis-triage. No game content is copied anywhere permanent; temp EBOOTs are
deleted after classification.
"""
from __future__ import annotations

import argparse
import glob
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from unfself import FSelf, NotADebugSelf
import elf_symbols

CONTAINER_EXTS = (".7z", ".zip", ".rar", ".iso", ".gcm", ".pkg")
DISC_EXTS = (".iso", ".gcm", ".pkg")           # nested images we defer by default


def find_7z() -> str:
    for p in (r"C:\Program Files\7-Zip\7z.exe", r"C:\Program Files (x86)\7-Zip\7z.exe",
              shutil.which("7z"), shutil.which("7za")):
        if p and os.path.isfile(p):
            return p
    raise SystemExit("7-Zip not found (install it or put 7z on PATH)")


def stable(path: str, settle: float) -> bool:
    """True if the file's size held steady across *settle* seconds and it isn't locked."""
    try:
        s1 = os.path.getsize(path)
    except OSError:
        return False
    time.sleep(settle)
    try:
        if os.path.getsize(path) != s1:
            return False
        with open(path, "rb"):                 # a file mid-write is often lock-denied
            pass
        return True
    except OSError:
        return False


def sz_listing(sevenzip: str, container: str) -> list[str]:
    r = subprocess.run([sevenzip, "l", container], capture_output=True, text=True,
                       errors="replace")
    return r.stdout.splitlines()


def _nested_image(lines: list[str]) -> str | None:
    """First disc-image entry name (Name is the last column of `7z l` rows)."""
    for ln in lines:
        name = ln[53:].strip() if len(ln) > 53 else ""
        if name and os.path.splitext(name)[1].lower() in (".gcm", ".iso", ".pkg"):
            return name
    return None


def extract_eboot(sevenzip, container, workdir, descend=False, pkg_timeout=1800,
                  depth=0) -> tuple[str | None, str]:
    """Return (eboot_path_or_None, note). Cheapest path per container type."""
    ext = os.path.splitext(container)[1].lower()

    if ext == ".pkg":
        out = os.path.join(workdir, "pkg")
        os.makedirs(out, exist_ok=True)
        try:
            subprocess.run([sys.executable, os.path.join(HERE, "pkg_extract.py"),
                            container, out, "--only", "EBOOT.BIN"],
                           capture_output=True, text=True, timeout=pkg_timeout)
        except subprocess.TimeoutExpired:
            return None, f"pkg extract timed out (>{pkg_timeout}s)"
        hits = glob.glob(os.path.join(out, "**", "EBOOT.BIN"), recursive=True)
        return (hits[0] if hits else None), ("" if hits else "no EBOOT in pkg")

    # .iso/.gcm read as ISO9660 by 7z; archives may hold a direct EBOOT.BIN.
    subprocess.run([sevenzip, "e", container, f"-o{workdir}", "-y", "-r", "EBOOT.BIN"],
                   capture_output=True, text=True)
    eb = os.path.join(workdir, "EBOOT.BIN")
    if os.path.isfile(eb):
        return eb, ""

    # Archive with no direct EBOOT: is it wrapping a disc image?
    lines = sz_listing(sevenzip, container)
    img = _nested_image(lines)
    if not img:
        return None, "no EBOOT found"
    if not descend or depth > 1:
        return None, "nested disc image (use --descend)"

    # Extract just the nested image (the GB cost), then recurse into it. A fresh
    # subdir keeps the big image isolated so main()'s cleanup reclaims it per title.
    sub = os.path.join(workdir, f"nested{depth}")
    os.makedirs(sub, exist_ok=True)
    subprocess.run([sevenzip, "e", container, f"-o{sub}", "-y", os.path.basename(img)],
                   capture_output=True, text=True)
    inner = os.path.join(sub, os.path.basename(img))
    if not os.path.isfile(inner):
        return None, "nested image extract failed"
    eb, note = extract_eboot(sevenzip, inner, sub, descend, pkg_timeout, depth + 1)
    return eb, (note or "(via nested disc)")


def classify(eboot_path: str, workdir: str):
    """Return (klass, nfuncs, has_dwarf). klass: debug/retail 0xNNNN / plain-ELF / not-self."""
    data = open(eboot_path, "rb").read()
    if data[:4] == b"\x7fELF":
        elf_for_syms = eboot_path
        klass = "plain-ELF"
    else:
        try:
            fs = FSelf(data)
        except NotADebugSelf:
            return "not-a-SELF", 0, False
        if not fs.is_debug:
            return f"retail 0x{fs.key_rev:04X}", 0, False
        klass = "debug fSELF"
        try:
            elf_bytes = fs.to_elf()
        except NotADebugSelf as exc:
            return f"debug? {exc}"[:20], 0, False
        elf_for_syms = os.path.join(workdir, "EBOOT.elf")
        open(elf_for_syms, "wb").write(elf_bytes)

    # symbol count + DWARF presence
    nfuncs, has_dwarf = 0, False
    try:
        nfuncs = len(elf_symbols.load_symbols(elf_for_syms))
    except SystemExit:
        pass                                   # stripped: no symtab
    try:
        from elf_parser import ELFFile
        e = ELFFile(elf_for_syms); e.load()
        has_dwarf = any(s.name_str == ".debug_info" for s in e.section_headers)
    except Exception:
        pass
    return klass, nfuncs, has_dwarf


def title_of(path: str, root: str) -> str:
    rel = os.path.relpath(path, root)
    parts = rel.replace("\\", "/").split("/")
    return parts[0] if len(parts) > 1 else os.path.splitext(parts[0])[0]


def main() -> int:
    ap = argparse.ArgumentParser(description="Debug-vs-retail EBOOT triage over a proto dump")
    ap.add_argument("directory", help="Folder of proto archives / disc images")
    ap.add_argument("--descend", action="store_true",
                    help="Also unpack nested disc images inside archives (extracts GBs)")
    ap.add_argument("--settle", type=float, default=4.0,
                    help="Seconds to watch a file's size before trusting it (default 4)")
    ap.add_argument("--pkg-timeout", type=int, default=1800,
                    help="Seconds to allow pkg_extract per .pkg (default 1800)")
    ap.add_argument("--keep-elf", metavar="DIR",
                    help="Save each debug build's unwrapped EBOOT.elf here for mining")
    args = ap.parse_args()

    try:
        sys.stdout.reconfigure(line_buffering=True)   # stream rows when redirected to a file
    except Exception:
        pass
    sevenzip = find_7z()
    # One container per title: prefer the top-level archive/image, skip nested duplicates.
    containers = []
    for dp, _dn, fn in os.walk(args.directory):
        for f in fn:
            if os.path.splitext(f)[1].lower() in CONTAINER_EXTS:
                containers.append(os.path.join(dp, f))
    containers.sort()

    print(f"{'TITLE':44} {'CONTAINER':6} {'CLASS':16} {'FUNCS':>7} DWARF  NOTE")
    print("-" * 100)
    rows = []
    for c in containers:
        title = title_of(c, args.directory)[:43]
        ext = os.path.splitext(c)[1].lower()
        if not stable(c, args.settle):
            print(f"{title:44} {ext:6} {'DOWNLOADING':16} {'-':>7} {'-':5}  size changing / locked")
            continue
        work = tempfile.mkdtemp(prefix="triage_")
        try:
            eb, note = extract_eboot(sevenzip, c, work, args.descend, args.pkg_timeout)
            if not eb:
                print(f"{title:44} {ext:6} {'-':16} {'-':>7} {'-':5}  {note}")
                continue
            klass, n, dw = classify(eb, work)
            rows.append((title, klass, n, dw))
            mark = "Y" if dw else "-"
            print(f"{title:44} {ext:6} {klass:16} {n:>7} {mark:5}  {note}")
            if args.keep_elf and klass in ("debug fSELF", "plain-ELF") and n:
                os.makedirs(args.keep_elf, exist_ok=True)
                srcf = os.path.join(work, "EBOOT.elf") if klass == "debug fSELF" else eb
                if os.path.isfile(srcf):
                    shutil.copy(srcf, os.path.join(args.keep_elf, f"{title}.elf"))
        finally:
            shutil.rmtree(work, ignore_errors=True)

    mine = [r for r in rows if r[2] > 0]
    print("\n" + "-" * 100)
    print(f"{len(rows)} EBOOTs triaged | {len(mine)} carry symbols "
          f"(worth mining) | {sum(1 for r in mine if r[3])} of those have DWARF")
    if mine:
        print("mining targets (most symbols first):")
        for t, k, n, dw in sorted(mine, key=lambda r: -r[2])[:20]:
            print(f"  {n:>7} funcs  {'DWARF' if dw else '     '}  {t}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
