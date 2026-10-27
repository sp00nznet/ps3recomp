#!/usr/bin/env python3
"""
endian_audit.py -- HLE out-param endianness heuristic auditor (INFO-level).

Purpose: the endianness/packing bug class (project blockers #11/#16/#18/#19d)
has so far been swept manually, module by module, as code gets exercised.
This is a STATIC heuristic pass: it ranks suspects, it does not prove bugs.
Expect noise -- false positives are handled via
tools/endian_audit_whitelist.txt, not by making the heuristic "smarter".

Heuristic (deliberately simple):
  For each exported (non-static, file-scope) function defined in libs/**/*.c
  whose signature has at least one pointer out-param of scalar or struct type
  (u16*/u32*/u64*/SomeStruct* -- NOT char*/void*/const-qualified-only), flag it
  INFO if the function body contains an assignment THROUGH that pointer
  (`*p = ...`, `p->field = ...`, `p[i] = ...`) but calls NEITHER `ps3_bswapNN(`
  NOR `vm_writeNN(` anywhere in the body.

This is a heuristic, not a data-flow analysis:
  - It does not verify the bswap/vm_write call actually touches the SAME
    out-param (any bswap/vm_write call anywhere in the function silences the
    flag for the whole function). That is intentional simplicity -- it
    trades false negatives for a MUCH simpler, auditable rule.
  - It does not track typedef chains beyond a small built-in scalar list plus
    a "looks like a PS3-struct-pointer" fallback (Capitalized* or CellFoo*).
  - const-qualified pointers (`const Foo* out`) are never flagged: they are
    almost always [in] params by SDK convention.
  - char*/void*/FILE*/function-pointer params are excluded: too often byte
    buffers or host-only handles (known false-positive classes to
    whitelist).

Usage:
    py -3 tools\\endian_audit.py [--libs-root libs] [--whitelist tools\\endian_audit_whitelist.txt]
                                 [--module MODNAME] [--verbose] [--top N]

Always exits 0 -- this is an advisory sweep, never a CI gate.
"""

import argparse
import os
import re
import sys

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------

# Boot-relevance priority: modules matching these substrings (case-insensitive,
# matched against the FILE STEM, e.g. "cellAudio") sort first, in this order.
# Anything not matching any bucket sorts after, alphabetically.
PRIORITY_BUCKETS = [
    ("audio", ["audio", "voice", "mic"]),
    ("fs", ["fs", "savedata"]),
    ("gcm_video", ["gcm", "video", "rsx", "resc"]),
    ("sysutil", ["sysutil", "sysmodule", "sys_interrupt", "userinfo"]),
]

# Bswap / vm_write call families that silence a flag (verified 2026-07-05
# against libs/filesystem/cellFs.c and runtime/ppu/ppu_memory.h -- the only
# two helper families used in this codebase for guest-endian stores).
SILENCING_CALL_RE = re.compile(r"\b(?:ps3_bswap(?:16|32|64)|vm_write(?:8|16|32|64))\s*\(")

# Built-in scalar out-param types we always recognize (host-endian-native
# types that the guest will read back big-endian).
SCALAR_PTR_TYPES = ("u8", "u16", "u32", "u64", "s8", "s16", "s32", "s64",
                    "b8", "ps3_addr_t", "float", "double",
                    "unsigned char", "unsigned short", "unsigned int", "unsigned long long",
                    "uint8_t", "uint16_t", "uint32_t", "uint64_t",
                    "int8_t", "int16_t", "int32_t", "int64_t")

# Excluded pointee types: byte buffers / opaque host handles / too noisy.
EXCLUDED_PTR_TYPES = ("char", "void", "FILE", "u8")  # u8*/char* = byte buffers by convention

# Struct-like pointee fallback: identifier starting uppercase (Cell*, Sce*, a
# project struct, etc.) -- these are almost always guest-visible ABI structs.
STRUCT_PTR_RE = re.compile(r"^[A-Z][A-Za-z0-9_]*$")

DEFAULT_WHITELIST = os.path.join("tools", "endian_audit_whitelist.txt")


# ---------------------------------------------------------------------------
# C source scanning helpers
# ---------------------------------------------------------------------------

def strip_comments_and_strings(text):
    """Replace comment/string/char-literal contents with spaces (preserving
    line structure and total length) so brace-matching and regexes never
    trip on braces/parens/quotes inside them."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            if j == -1:
                j = n
            out.append(" " * (j - i))
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            if j == -1:
                j = n
                span = text[i:j]
            else:
                j += 2
                span = text[i:j]
            out.append("".join(ch if ch == "\n" else " " for ch in span))
            i = j
        elif c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                if text[j] == "\\":
                    j += 2
                else:
                    j += 1
            j = min(j + 1, n)
            span = text[i:j]
            out.append("".join(ch if ch == "\n" else " " for ch in span))
            i = j
        elif c == "'":
            j = i + 1
            while j < n and text[j] != "'":
                if text[j] == "\\":
                    j += 2
                else:
                    j += 1
            j = min(j + 1, n)
            span = text[i:j]
            out.append("".join(ch if ch == "\n" else " " for ch in span))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


# Matches a top-level (column-0-ish) function definition start: a return
# type, a name, an open paren, params (possibly spanning lines), then '{' on
# the line where the params close (allow whitespace/newlines before '{').
# We intentionally require the function name + '(' to start at brace depth 0
# and not be preceded by ';' immediately (i.e. not a prototype) -- verified
# by requiring the matched '{' (not ';') to be the very next non-ws token
# after the ')'.
FUNC_HEAD_RE = re.compile(
    r"""^(?P<ret>[A-Za-z_][A-Za-z0-9_ \t]*?[ \t*])
        (?P<name>[A-Za-z_][A-Za-z0-9_]*)
        \s*\((?P<params>[^;{}]*?)\)
        \s*
        \{""",
    re.MULTILINE | re.VERBOSE | re.DOTALL,
)


def iter_toplevel_functions(clean_text):
    """Yield (name, ret_type, params_str, body_start, body_end) for every
    non-static, file-scope function DEFINITION in clean_text (comments/
    strings already blanked). body_start/body_end are indices of the
    matching '{' and '}' (inclusive of both braces)."""
    for m in FUNC_HEAD_RE.finditer(clean_text):
        ret = m.group("ret").strip()
        name = m.group("name")
        params = m.group("params")

        # Skip obvious non-functions: control keywords mis-parsed as "ret name(...)",
        # and macro-looking all-caps invocations.
        if ret.split()[-1] if ret.split() else "" in ("return", "if", "while", "for", "switch"):
            pass
        first_ret_tok = ret.split()[0] if ret.split() else ""
        if first_ret_tok in ("return", "if", "while", "for", "switch", "sizeof", "else"):
            continue
        if "static" in ret.split():
            continue  # not exported
        if name in ("if", "for", "while", "switch", "sizeof"):
            continue

        brace_start = m.end() - 1  # index of the '{'
        depth = 0
        i = brace_start
        n = len(clean_text)
        body_end = None
        while i < n:
            ch = clean_text[i]
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    body_end = i
                    break
            i += 1
        if body_end is None:
            continue  # unterminated (shouldn't happen in valid C); skip defensively

        yield name, ret, params, brace_start, body_end


# Splits a parameter list on top-level commas (none of our params contain
# function-pointer commas inside parens in practice, but guard anyway by
# tracking paren depth).
def split_params(params_str):
    parts = []
    depth = 0
    cur = []
    for ch in params_str:
        if ch == "(":
            depth += 1
            cur.append(ch)
        elif ch == ")":
            depth -= 1
            cur.append(ch)
        elif ch == "," and depth == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    if cur:
        parts.append("".join(cur))
    return [p.strip() for p in parts if p.strip()]


PARAM_RE = re.compile(
    r"""^(?P<quals1>(?:const|volatile|struct|unsigned|signed)\s+)*
        (?P<base>[A-Za-z_][A-Za-z0-9_]*)
        \s*
        (?P<stars>\**)
        \s*
        (?P<qual_after>(?:const\s+)*)
        (?P<pname>[A-Za-z_][A-Za-z0-9_]*)?
        (?:\s*\[[^\]]*\])?
        $""",
    re.VERBOSE,
)


def classify_out_params(params_str):
    """Return a list of (param_name, pointee_type, is_const_pointee) for
    params that look like NON-CONST pointer out-params of a scalar or
    struct-like type, excluding char*/void*/function-pointers."""
    results = []
    if params_str.strip() in ("", "void"):
        return results

    for raw in split_params(params_str):
        raw = raw.strip()
        if not raw or raw == "void":
            continue
        if "(" in raw and "*" in raw.split("(")[0]:
            # crude function-pointer param heuristic: "RetT (*name)(...)"
            continue

        # Detect const-ness of the POINTEE (const applied before the base type,
        # i.e. "const Foo* x" -- pointee is const, exclude; but "Foo* const x"
        # is a const pointer to non-const pointee -- NOT excluded).
        stripped = raw.strip()
        pointee_const = bool(re.match(r"^\s*const\b", stripped))

        m = PARAM_RE.match(stripped)
        if not m:
            continue
        stars = m.group("stars")
        if len(stars) != 1:
            continue  # only single-indirection out-params (u32*), skip **/none
        base = m.group("base")

        if pointee_const:
            continue  # [in] param by convention

        if base in EXCLUDED_PTR_TYPES:
            continue

        if base in SCALAR_PTR_TYPES:
            results.append((m.group("pname") or "?", base, False))
        elif STRUCT_PTR_RE.match(base):
            results.append((m.group("pname") or "?", base, False))
        # else: lowercase non-scalar typedef (e.g. "mutex_t*") -- skip, too
        # likely to be a host-only internal type rather than guest ABI.
    return results


# A store THROUGH a given pointer name: "*name =", "name->field =", "name[i] ="
def store_through_pointer_re(pname):
    esc = re.escape(pname)
    return re.compile(
        r"(?:\*\s*" + esc + r"\s*(?:=[^=]|[+\-*/|&^]=)"
        r"|\b" + esc + r"\s*->\s*[A-Za-z_][A-Za-z0-9_]*\s*(?:=[^=]|[+\-*/|&^]=)"
        r"|\b" + esc + r"\s*\[[^\]]*\]\s*(?:=[^=]|[+\-*/|&^]=))"
    )


# ---------------------------------------------------------------------------
# Priority bucket sort
# ---------------------------------------------------------------------------

def module_priority(stem):
    low = stem.lower()
    for idx, (_bucket_name, needles) in enumerate(PRIORITY_BUCKETS):
        for needle in needles:
            if needle in low:
                return idx
    return len(PRIORITY_BUCKETS)


# ---------------------------------------------------------------------------
# Whitelist
# ---------------------------------------------------------------------------

def load_whitelist(path):
    """tools/endian_audit_whitelist.txt format: one entry per line,
    `function_name<TAB or multiple-spaces>reason text`. '#' comment lines and
    blank lines ignored."""
    names = set()
    if not os.path.isfile(path):
        return names
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            name = re.split(r"\s+", line.strip(), maxsplit=1)[0]
            if name:
                names.add(name)
    return names


# ---------------------------------------------------------------------------
# Core scan
# ---------------------------------------------------------------------------

def scan_file(path):
    """Return list of dicts: {func, params, file, line} for suspect functions
    in one .c file."""
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        raw = f.read()
    clean = strip_comments_and_strings(raw)

    suspects = []
    for name, _ret, params_str, body_start, body_end in iter_toplevel_functions(clean):
        out_params = classify_out_params(params_str)
        if not out_params:
            continue

        body = clean[body_start:body_end + 1]

        if SILENCING_CALL_RE.search(body):
            continue  # some bswap/vm_write call exists somewhere in the body

        stored_params = []
        for pname, ptype, _const in out_params:
            if pname == "?":
                continue
            if store_through_pointer_re(pname).search(body):
                stored_params.append((pname, ptype))

        if not stored_params:
            continue  # out-param never actually stored through -> not a suspect

        line_no = raw.count("\n", 0, body_start) + 1
        # correct for the function head starting a few lines above body_start;
        # walk back to the function name occurrence for a more useful line no.
        name_idx = raw.rfind(name, 0, body_start)
        if name_idx != -1:
            line_no = raw.count("\n", 0, name_idx) + 1

        suspects.append({
            "func": name,
            "params": ", ".join(f"{t}* {n}" for n, t in stored_params),
            "file": path,
            "line": line_no,
        })
    return suspects


def find_c_files(libs_root):
    out = []
    for dirpath, _dirnames, filenames in os.walk(libs_root):
        for fn in filenames:
            if fn.endswith(".c"):
                out.append(os.path.join(dirpath, fn))
    return sorted(out)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        description="HLE out-param endianness heuristic auditor (INFO-level, advisory)."
    )
    ap.add_argument("--libs-root", default="libs", help="root to scan (default: libs)")
    ap.add_argument("--whitelist", default=DEFAULT_WHITELIST,
                    help="whitelist file (function names + reasons)")
    ap.add_argument("--module", default=None,
                    help="only scan files whose stem matches this substring")
    ap.add_argument("--top", type=int, default=10, help="how many top suspects to print")
    ap.add_argument("--verbose", action="store_true", help="print every suspect, not just top-N")
    args = ap.parse_args()

    if not os.path.isdir(args.libs_root):
        print(f"[endian_audit] libs root not found: {args.libs_root}", file=sys.stderr)
        sys.exit(0)  # advisory tool: never a hard failure exit

    whitelist = load_whitelist(args.whitelist)

    files = find_c_files(args.libs_root)
    if args.module:
        files = [f for f in files if args.module.lower() in os.path.basename(f).lower()]

    by_module = {}  # stem -> list[suspect dict]
    total_scanned_functions_with_outparams = 0

    for path in files:
        stem = os.path.splitext(os.path.basename(path))[0]
        suspects = scan_file(path)
        kept = []
        for s in suspects:
            total_scanned_functions_with_outparams += 1
            if s["func"] in whitelist:
                continue
            kept.append(s)
        if kept:
            by_module.setdefault(stem, []).extend(kept)

    # Sort modules by boot-relevance priority bucket, then alphabetically.
    module_order = sorted(by_module.keys(), key=lambda s: (module_priority(s), s.lower()))

    print("=" * 78)
    print("endian_audit.py -- HLE out-param endianness heuristic (INFO-level, advisory)")
    print(f"Scanned {len(files)} file(s) under '{args.libs_root}'"
          + (f" (module filter: {args.module})" if args.module else ""))
    print(f"Whitelist: {args.whitelist} ({len(whitelist)} entries loaded)")
    print("Rule: exported fn has a non-const scalar/struct pointer out-param that is")
    print("      STORED THROUGH in the body, with NO ps3_bswap*/vm_write* call anywhere")
    print("      in that function body.")
    print("=" * 78)

    total_suspects = sum(len(v) for v in by_module.values())
    print(f"\nTotal suspects (post-whitelist): {total_suspects}")
    print("\nPer-module suspect counts (boot-relevance order):")
    for stem in module_order:
        bucket_idx = module_priority(stem)
        bucket_name = PRIORITY_BUCKETS[bucket_idx][0] if bucket_idx < len(PRIORITY_BUCKETS) else "other"
        print(f"  [{bucket_name:9s}] {stem:24s} {len(by_module[stem]):4d}")

    # Flat suspect list in module-priority order for the "top-N" print.
    flat = []
    for stem in module_order:
        for s in by_module[stem]:
            flat.append(s)

    print(f"\nTop {args.top if not args.verbose else len(flat)} suspects:")
    show = flat if args.verbose else flat[:args.top]
    for s in show:
        print(f"  {s['file']}:{s['line']}  {s['func']}({s['params']})")

    if not flat:
        print("  (none)")

    print()
    print("Advisory tool -- exit code is always 0.")
    sys.exit(0)


if __name__ == "__main__":
    main()
