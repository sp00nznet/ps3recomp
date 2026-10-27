"""spu_ident.py - identify the SPU images embedded in a PS3 executable.

Most SPU work in a shipped game is not custom code -- it is prebuilt middleware linked
into the PPU image: Sony Edge (geometry, zlib), Bink video, Havok, Wwise audio, codec
libraries. Debug builds prove it: their .debug_scespuversion and `_binary_<name>_elf_start`
symbols name every embedded SPU ELF (SPU_PM, binkspurs, edgezlib_inflate_task, the Ak*FX
Wwise jobs, ...). Knowing a blob *is* Edge zlib means you HLE it instead of lifting it.

Two modes:
  * name them (debug build): list each embedded SPU image with its name + category, and
    optionally record a fingerprint DB so the same blobs can be found elsewhere.
  * identify them (stripped/retail build): match each embedded SPU image against the
    fingerprint DB built from debug builds.

    python spu_ident.py EBOOT.elf                       # list this build's SPU jobs
    python spu_ident.py *.elf --update-db spu_sigs.json # harvest fingerprints
    python spu_ident.py RETAIL.elf --db spu_sigs.json   # identify against the DB

An SPU image is located by its `_binary_..._start/_end` bracket (debug builds) or by
scanning for the SPU ELF header (machine 23). Its identity comes from the `_binary_`
name, the in-image `.note.spu_name`, or -- when both are stripped -- a .text hash match.
"""
from __future__ import annotations

import argparse
import glob
import hashlib
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from elf_parser import ELFFile
from ppu_loader import read_vaddr

SPU_MACHINE = 23

# name-substring -> (category, "hle" | "lift") advice
MIDDLEWARE = [
    ("edge", "Sony Edge", "hle"),
    ("bink", "Bink video", "hle"),
    ("hka", "Havok animation", "hle"),
    ("havok", "Havok", "hle"),
    ("granny", "Granny animation", "hle"),
    ("mp3", "MP3 codec", "hle"),
    ("vorbis", "Vorbis codec", "hle"),
    ("adpcm", "ADPCM codec", "hle"),
    ("at3", "ATRAC3 codec", "hle"),
    ("scream", "SCREAM audio", "hle"),
    ("ak", "Wwise audio", "hle"),          # AkRoomVerbFXJob etc.
    ("fmod", "FMOD audio", "hle"),
    ("spurs", "SPURS kernel/util", "hle"),
    ("inflate", "zlib inflate", "hle"),
    ("zlib", "zlib", "hle"),
]


def classify(name: str) -> tuple[str, str]:
    low = name.lower()
    for key, cat, advice in MIDDLEWARE:
        if key in low:
            return cat, advice
    return "custom", "lift"


def _all_symbols(elf) -> dict[str, int]:
    """name -> value for every symbol (any type), from .symtab if present."""
    si = next((i for i, s in enumerate(elf.section_headers) if s.sh_type == 2), None)
    if si is None:
        return {}
    sd = elf.get_section_data(si)
    strd = elf.get_section_data(elf.section_headers[si].sh_link)
    ent = elf.section_headers[si].sh_entsize or 24
    out = {}
    for i in range(len(sd) // ent):
        name_o, _info, _o, _sh, val, _sz = struct.unpack_from(">IBBHQQ", sd, i * ent)
        z = strd.find(b"\0", name_o)
        out[strd[name_o:z].decode("latin1")] = val
    return out


def _clean_name(binary_symbol: str) -> str:
    """`_binary_task_binkspurs_elf` -> `binkspurs`. The linker-generated symbol is the
    reliable identifier; just trim the boilerplate the objcopy embed adds."""
    n = binary_symbol
    for pre in ("task_", "pm_"):
        if n.startswith(pre):
            n = n[len(pre):]
    for suf in ("_spu_elf", "_spu_bin", "_elf", "_bin", ".elf", ".spu"):
        if n.endswith(suf):
            n = n[:-len(suf)]; break
    return n or binary_symbol


def _text_hash(blob: bytes) -> str:
    """SHA-1 of the SPU ELF's .text (stable across games for the same middleware),
    or of the whole blob if it isn't a parseable ELF."""
    if blob[:4] == b"\x7fELF" and len(blob) >= 0x34:
        e_shoff = struct.unpack_from(">I", blob, 0x20)[0]
        e_shentsize, e_shnum, e_shstrndx = struct.unpack_from(">HHH", blob, 0x2E)
        if e_shoff + e_shnum * e_shentsize <= len(blob):
            so, ssz = struct.unpack_from(">II", blob, e_shoff + e_shstrndx * e_shentsize + 16)
            shstr = blob[so:so + ssz]
            for i in range(e_shnum):
                b = e_shoff + i * e_shentsize
                nameo = struct.unpack_from(">I", blob, b)[0]
                off, size = struct.unpack_from(">II", blob, b + 16)
                z = shstr.find(b"\0", nameo)
                if shstr[nameo:z] == b".text":
                    return hashlib.sha1(blob[off:off + size]).hexdigest()
    return hashlib.sha1(blob).hexdigest()


def spu_elf_layout(blob: bytes) -> dict | None:
    """Parse an SPU ELF32 (big-endian) into the code/data-partition landmarks a matcher
    needs: entry PC, code base (.init), .text range, where data (.rodata) starts, and the
    loaded footprint (max non-NOBITS extent -- .bss beyond it is zeroed, not DMA'd). None
    if the blob is not a parseable SPU ELF (a raw job binary)."""
    if blob[:4] != b"\x7fELF" or len(blob) < 0x34:
        return None
    if struct.unpack_from(">H", blob, 18)[0] != SPU_MACHINE:
        return None
    shoff = struct.unpack_from(">I", blob, 0x20)[0]
    shent, shnum, shstrndx = struct.unpack_from(">HHH", blob, 0x2E)
    if shoff + shnum * shent > len(blob):
        return None
    sname_off, ssz = struct.unpack_from(">II", blob, shoff + shstrndx * shent + 16)
    shstr = blob[sname_off:sname_off + ssz]
    code_lo, code_hi, load_hi = None, 0, 0
    text = data_start = None
    for i in range(shnum):
        b = shoff + i * shent
        _no, typ, _fl, addr, off, size = struct.unpack_from(">IIIIII", blob, b)
        z = shstr.find(b"\0", _no)
        nm = shstr[_no:z]
        if nm == b".text":
            text = (off, size, addr)
        elif nm == b".rodata" and data_start is None:
            data_start = addr
        if nm in (b".init", b".text", b".fini") and size:
            code_lo = addr if code_lo is None else min(code_lo, addr)
            code_hi = max(code_hi, addr + size)
        if typ != 8 and size:                      # SHT_NOBITS (.bss) is not loaded
            load_hi = max(load_hi, addr + size)
    if text is None:
        return None
    t_off, t_sz, t_addr = text
    return {
        "entry_pc": struct.unpack_from(">I", blob, 0x18)[0],
        "code_base": code_lo, "text_ls": t_addr, "text_end": t_addr + t_sz,
        "data_start": data_start, "load_footprint": load_hi, "text_size": t_sz,
        "text_sha1": hashlib.sha1(blob[t_off:t_off + t_sz]).hexdigest(),
    }


def fingerprint_table(elf, min_size: int = 16) -> list[dict]:
    """Per embedded SPU image: the layout + .text fingerprint (SPU-ELFs) or a whole-blob
    hash (raw job binaries), so a flat local-store dump can be labelled by matching its
    code region. Symbols smaller than min_size are skipped (stray data symbols that share
    the _binary_ naming)."""
    syms = _all_symbols(elf)
    starts = {k[:-6]: v for k, v in syms.items()
              if k.startswith("_binary_") and k.endswith("_start")}
    ends = {k[:-4]: v for k, v in syms.items()
            if k.startswith("_binary_") and k.endswith("_end")}
    rows = []
    for key, start in sorted(starts.items(), key=lambda kv: kv[1]):
        end = ends.get(key)
        if not end or end - start < min_size:
            continue
        blob = read_vaddr(elf, start, end - start)
        if not blob:
            continue
        name = _clean_name(key[len("_binary_"):])
        cat, advice = classify(name)
        lay = spu_elf_layout(blob)
        if lay:
            rows.append({"name": name, "format": "spu-elf",
                         "category": cat, "advice": advice, **lay})
        else:
            rows.append({"name": name, "format": "raw", "category": cat,
                         "advice": advice, "blob_size": len(blob),
                         "blob_sha1": hashlib.sha1(blob).hexdigest()})
    return rows


def embedded_spu_images(elf) -> list[dict]:
    """Every embedded SPU image: name (best available), category, size, text hash."""
    syms = _all_symbols(elf)
    starts = {k[:-6]: v for k, v in syms.items()
              if k.startswith("_binary_") and k.endswith("_start")}
    ends = {k[:-4]: v for k, v in syms.items()
            if k.startswith("_binary_") and k.endswith("_end")}

    out = []
    seen_hashes = set()
    for key, start in sorted(starts.items(), key=lambda kv: kv[1]):
        end = ends.get(key)
        if not end or end <= start:
            continue
        blob = read_vaddr(elf, start, end - start)
        if not blob:
            continue
        is_elf = blob[:4] == b"\x7fELF" and \
            struct.unpack_from(">H", blob, 18)[0] == SPU_MACHINE
        binname = key[len("_binary_"):]
        name = _clean_name(binname)
        h = _text_hash(blob)
        if h in seen_hashes:
            continue
        seen_hashes.add(h)
        cat, advice = classify(name)
        out.append({"name": name, "binary_symbol": binname, "is_spu_elf": is_elf,
                    "size": end - start, "category": cat, "advice": advice, "text_sha1": h})
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description="Identify embedded SPU images (middleware vs custom)")
    ap.add_argument("inputs", nargs="+", help="Unstripped ELF(s) (see unfself.py)")
    ap.add_argument("--db", help="Fingerprint DB to identify against (JSON)")
    ap.add_argument("--update-db", metavar="JSON",
                    help="Harvest fingerprints from the inputs into this DB")
    ap.add_argument("--fingerprints", metavar="JSON", nargs="?", const="-",
                    help="Emit the code/data-layout + .text-fingerprint table (for matching "
                         "a flat local-store dump to a job); JSON path, or omit for stdout")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    files = []
    for p in args.inputs:
        files.extend(sorted(glob.glob(p)) if any(c in p for c in "*?") else [p])

    if args.fingerprints is not None:
        rows = []
        for path in files:
            try:
                elf = ELFFile(path); elf.load()
            except Exception as exc:
                print(f"{path}: {exc}", file=sys.stderr); continue
            rows.extend(fingerprint_table(elf))
        if args.fingerprints == "-":
            json.dump(rows, sys.stdout, indent=1); print()
        else:
            json.dump(rows, open(args.fingerprints, "w", encoding="utf-8"), indent=1)
            elfs = sum(1 for r in rows if r["format"] == "spu-elf")
            print(f"[spu_ident] {len(rows)} images ({elfs} SPU-ELF fingerprintable, "
                  f"{len(rows) - elfs} raw) -> {args.fingerprints}", file=sys.stderr)
        return 0

    db = {}
    if args.db and os.path.exists(args.db):
        db = json.load(open(args.db, encoding="utf-8"))

    harvest = dict(db)
    everything = {}
    for path in files:
        try:
            elf = ELFFile(path); elf.load()
        except Exception as exc:
            print(f"{path}: {exc}", file=sys.stderr); continue
        imgs = embedded_spu_images(elf)
        everything[path] = imgs
        for im in imgs:
            harvest.setdefault(im["text_sha1"], {"name": im["name"], "category": im["category"]})

    if args.update_db:
        json.dump(harvest, open(args.update_db, "w", encoding="utf-8"),
                  indent=1, sort_keys=True)
        print(f"[spu_ident] {len(harvest)} fingerprints -> {args.update_db}", file=sys.stderr)

    if args.json:
        json.dump(everything, sys.stdout, indent=1); print()
        return 0

    for path, imgs in everything.items():
        title = os.path.basename(os.path.dirname(path)) or os.path.basename(path)
        cats = {}
        for im in imgs:
            cats.setdefault(im["category"], 0)
            cats[im["category"]] += 1
        print(f"\n=== {title}: {len(imgs)} SPU images ===")
        for im in imgs:
            known = db.get(im["text_sha1"], {}).get("name") if db else None
            mark = f"  [db match: {known}]" if known else ""
            print(f"  {im['advice']:4} {im['category']:20} {im['name'][:44]}{mark}")
        summary = ", ".join(f"{v} {k}" for k, v in sorted(cats.items(), key=lambda x: -x[1]))
        print(f"  -> {summary}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
