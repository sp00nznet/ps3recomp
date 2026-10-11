"""Committed expected transcripts ("goldens") for the conformance runners.

A golden is RPCS3's transcript block for one generated ELF, stored as
golden/<stem>.<sha256 of the ELF, 16 hex>.txt next to the runner. The
generators are deterministic, so the same ELF always finds its golden and a
run needs no RPCS3; an ELF from a changed generator (or another --seed or
--only) finds none, and the runner says to regenerate with --regen-oracle,
which runs RPCS3 once and replaces the stem's previous golden.
"""
import glob
import hashlib
import os
import re


def path_for(golden_dir, stem, elf):
    sha = hashlib.sha256(open(elf, "rb").read()).hexdigest()[:16]
    return os.path.join(golden_dir, "%s.%s.txt" % (stem, sha))


def save(golden_dir, stem, elf, oracle_txt, tag, replace_others=True):
    """Keep the tag's BEGIN..END block of oracle_txt as the golden for elf."""
    text = open(oracle_txt, "rb").read().decode("latin-1")
    m = re.search(re.escape(tag) + r" BEGIN.*?" + re.escape(tag) + r" END[^\n]*\n?", text, re.S)
    if not m:
        return None
    os.makedirs(golden_dir, exist_ok=True)
    dst = path_for(golden_dir, stem, elf)
    if replace_others:
        for old in glob.glob(os.path.join(golden_dir, stem + ".*.txt")):
            if old != dst:
                os.remove(old)
    with open(dst, "w", encoding="latin-1") as f:
        f.write(m.group(0) if m.group(0).endswith("\n") else m.group(0) + "\n")
    return dst


def find(golden_dir, stem, elf):
    """The golden for this exact ELF, or None (print why)."""
    p = path_for(golden_dir, stem, elf)
    if os.path.exists(p):
        return p
    print("no golden for this %s.elf in %s: rerun with --regen-oracle (needs RPCS3)"
          % (stem, os.path.relpath(golden_dir)))
    return None
