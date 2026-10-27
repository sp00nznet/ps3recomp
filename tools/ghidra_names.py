#!/usr/bin/env python3
"""
ghidra_names.py — build a {addr: name} map from Ghidra analysis JSON.

Consumes the output of ghidra_analyze.py (functions.json, symbols.json,
strings.json) and produces names.json:

    { "0x000CAA08": {"label": "...", "cident": "...", "src": "..."}, ... }

where:
    label  — human-readable name for a comment above the function
    cident — a valid, unique C identifier (for optional renaming)
    src    — which heuristic produced it (symbol|ghidra|string)

Naming priority (highest first):
  1. A real symbol/Ghidra name (not FUN_/LAB_/SUB_/auto) — e.g. demangled C++,
     FID library match, or import/export.
  2. A distinctive referenced string. PhyreEngine (and most C++ engines) embed
     __FILE__ / assert / function-name strings; the function that references a
     ".cpp"/"::"/identifier-like string is very likely that file/function.

The lifter keeps func_ADDR as the canonical symbol; these names are emitted as
comments (and optionally as aliases), so naming never breaks address dispatch.
"""
import argparse
import json
import os
import re
import sys
from collections import defaultdict

GENERIC_RE = re.compile(r'^(FUN_|LAB_|SUB_|DAT_|thunk_FUN_|caseD_|switchD_|s_|u_)', re.I)
HEX_NAME_RE = re.compile(r'^(func_)?[0-9A-Fa-f]{6,8}$')
IDENT_TOKEN = re.compile(r'[A-Za-z_][A-Za-z0-9_]{2,}')
# Tokens that make a string look like a source-location / symbol hint.
HINTY = re.compile(r'(::|\.cpp|\.c\b|\.h\b|/|\\|[A-Z][a-z]+[A-Z])')


def norm_addr(a):
    if isinstance(a, int):
        return f"0x{a & 0xFFFFFFFF:08X}"
    s = str(a)
    return f"0x{int(s, 16) & 0xFFFFFFFF:08X}"


def sanitize_ident(s, maxlen=48):
    """Turn an arbitrary string into a C identifier fragment."""
    # For C++ names: keep the class::method tail, collapse :: to __
    s = s.strip()
    # Drop a trailing "(args)" if present
    s = re.sub(r'\(.*$', '', s)
    s = s.replace('::', '__')
    s = re.sub(r'[^A-Za-z0-9_]', '_', s)
    s = re.sub(r'_+', '_', s).strip('_')
    if not s:
        return ""
    if s[0].isdigit():
        s = "_" + s
    return s[:maxlen]


def is_meaningful_name(name):
    if not name:
        return False
    # Strip a leading namespace prefix Ghidra adds (e.g. ".opd.FUN_00010200").
    base = name.rsplit(".", 1)[-1] if name.startswith(".") else name
    if GENERIC_RE.match(base):
        return False
    if HEX_NAME_RE.match(base):
        return False
    # Reject any name still carrying an auto-generated token anywhere.
    if re.search(r'(FUN_[0-9A-Fa-f]{6,}|LAB_[0-9A-Fa-f]{6,}|SUB_[0-9A-Fa-f]{6,})', name):
        return False
    return True


# Generic words / type tags that are never useful as a function name.
STOPWORDS = {
    "float", "int", "char", "bool", "void", "double", "short", "long", "uint",
    "string", "xml", "data", "name", "type", "value", "size", "count", "index",
    "output", "input", "error", "warning", "true", "false", "null", "none",
    "library", "binarydata", "animation", "scale", "weights", "buffer", "object",
    "node", "list", "array", "matrix", "vector", "color", "texture", "shader",
}


def string_hint(val):
    """Derive (confidence, name_fragment) from a referenced string.

    confidence: 3 = C++ qualified (A::B::c), 2 = source path basename or a
    multi-token function-ish identifier, 0 = reject. We only keep >= 2 so we
    never name a function after a bare type word or an ALL-CAPS enum tag.
    """
    v = val.strip()
    if len(v) < 5 or len(v) > 200:
        return (0, "")

    # 3: C++ qualified name anywhere in the string (assert/log messages).
    m = re.search(r'([A-Za-z_]\w*(?:::[A-Za-z_]\w*)+)', v)
    if m:
        return (3, sanitize_ident(m.group(1)))

    # 2: source-file path → basename (its .cpp implies the translation unit).
    if HINTY.search(v) and re.search(r'[/\\]', v):
        base = re.split(r'[/\\]', v)[-1]
        base = re.sub(r'\.(cpp|cxx|cc|c|h|hpp)$', '', base, flags=re.I)
        frag = sanitize_ident(base)
        if len(frag) >= 4 and frag.lower() not in STOPWORDS:
            return (2, frag)

    # 2: a single multi-part identifier token like PFoo_bar or PSSG_X_Y, but
    # reject ALL-CAPS enum tags and stopwords.
    if re.fullmatch(r'[A-Za-z_]\w*', v) and "_" in v:
        if not v.isupper() and v.lower() not in STOPWORDS and len(v) >= 5:
            return (2, sanitize_ident(v))

    return (0, "")


def load(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return json.load(f)


def main():
    ap = argparse.ArgumentParser(description="Build name map from Ghidra JSON")
    ap.add_argument("ghidra_dir", help="Dir with functions.json/symbols.json/strings.json")
    ap.add_argument("-o", "--output", default=None, help="Output names.json")
    ap.add_argument("--min-string-len", type=int, default=4)
    args = ap.parse_args()

    fdir = args.ghidra_dir
    funcs = load(os.path.join(fdir, "functions.json"))
    func_addrs = {norm_addr(f["addr"]) for f in funcs}
    try:
        syms = load(os.path.join(fdir, "symbols.json"))
    except FileNotFoundError:
        syms = []
    try:
        strings = load(os.path.join(fdir, "strings.json"))
    except FileNotFoundError:
        strings = []

    # addr -> best symbol name (prefer non-generic, primary, imported/user source)
    sym_name = {}
    SRC_RANK = {"IMPORTED": 3, "USER_DEFINED": 3, "ANALYSIS": 2, "DEFAULT": 0}
    sym_rank = defaultdict(int)
    for s in syms:
        a = norm_addr(s["addr"])
        if a not in func_addrs:
            continue
        nm = s.get("name", "")
        if not is_meaningful_name(nm):
            continue
        rank = SRC_RANK.get(s.get("source", ""), 1) + (1 if s.get("primary") else 0)
        if rank > sym_rank[a]:
            sym_rank[a] = rank
            sym_name[a] = nm

    # addr -> candidate string hints (a function may reference several).
    # Keep (confidence, brevity, fragment) so we can pick the best per function.
    str_hint = defaultdict(list)
    for st in strings:
        conf, hint = string_hint(st.get("val", ""))
        if conf < 2 or not hint:
            continue
        refs = st.get("refs", [])
        # A string referenced by many functions is a shared helper message, not
        # a per-function name. Qualified C++ names (conf 3) are inherently
        # function-specific so tolerate a wider fan-out; path/identifier hints
        # (conf 2) must be near-unique.
        if len(refs) > (8 if conf >= 3 else 3):
            continue
        for ref in refs:
            ra = norm_addr(ref)
            if ra in func_addrs:
                str_hint[ra].append((conf, hint, st.get("val", "")))

    # Ghidra's own function name (may be FID/demangled)
    ghidra_name = {norm_addr(f["addr"]): f.get("name", "") for f in funcs}

    names = {}
    used_idents = set()
    counts = defaultdict(int)

    def uniq(cident, addr):
        base = cident or "fn"
        cand = base
        if cand in used_idents:
            cand = f"{base}_{addr[2:]}"  # append addr hex for uniqueness
        used_idents.add(cand)
        return cand

    for addr in sorted(func_addrs):
        label = None
        src = None
        # 1. real symbol or Ghidra name
        if addr in sym_name:
            label, src = sym_name[addr], "symbol"
        elif is_meaningful_name(ghidra_name.get(addr, "")):
            label, src = ghidra_name[addr], "ghidra"
        # 2. string hint — pick highest confidence, then shortest fragment
        elif addr in str_hint:
            cand = sorted(str_hint[addr], key=lambda t: (-t[0], len(t[1])))[0]
            label, src = cand[1], "string"
        if not label:
            continue
        cident = sanitize_ident(label)
        if not cident:
            continue
        cident = uniq(cident, addr)
        names[addr] = {"label": label, "cident": cident, "src": src}
        counts[src] += 1

    out = args.output or os.path.join(fdir, "names.json")
    with open(out, "w", encoding="utf-8") as f:
        json.dump(names, f, indent=1, sort_keys=True)
    total = len(func_addrs)
    named = len(names)
    print(f"Functions: {total}")
    print(f"Named:     {named} ({100*named//max(total,1)}%)")
    for k in ("symbol", "ghidra", "string"):
        print(f"  via {k}: {counts[k]}")
    print(f"Wrote {out}")


if __name__ == "__main__":
    main()
