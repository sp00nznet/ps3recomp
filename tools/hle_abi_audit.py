#!/usr/bin/env python3
"""hle_abi_audit.py -- differential ABI-signature audit for HLE functions.

Same technique as the PPU/SPU decode conformance audits, applied one layer up:
instead of diffing our instruction lifter against the PowerISA reference, this
diffs our HLE C function signatures (libs/**/*.c) against RPCS3's real,
shipped implementation of the same PS3 module functions
(rpcs3/rpcs3/Emu/Cell/Modules/*.cpp).

This is the bug class that produced blocker #16 (cellGcmGetTimeStampLocation),
#18 (CellFsStat), #19d, and cellGcmGetTiledPitchSize: we quietly turn a
function that PS3 games expect to *return* a value into one that writes an
out-param and returns an error code (or vice versa), or we drop/add a
parameter. Those are silent ABI mismatches -- they compile fine, and only
misbehave at runtime when a caller reads the return register/parameter that
we didn't populate the way it expects.

RPCS3 (rpcs3/ in this repo) is GPLv2. We read ONLY the function signature
(return type + parameter types) as ground truth for the real PS3 ABI --
never copy its function bodies. This repo stays MIT/clean-room.

Usage:
    py -3 tools/hle_abi_audit.py [--libs DIR] [--oracle DIR] [--whitelist FILE]
                                  [--json OUT.json]

Stdlib-only, py -3, LF line endings, 4-space indent.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from dataclasses import dataclass, field


# ---------------------------------------------------------------------------
# Boot-relevance ordering (hardcoded, per the task spec): gcm/video, fs,
# audio, sysutil first, everything else after, alphabetically within a tier.
# ---------------------------------------------------------------------------

MODULE_PRIORITY = [
    "cellGcmSys",
    "cellVideoOut",
    "cellResc",
    "sys_fs",
    "cellFsUtility",
    "cellAudio",
    "cellSysutil",
    "cellSysmodule",
]


def module_rank(module: str) -> tuple:
    try:
        return (0, MODULE_PRIORITY.index(module))
    except ValueError:
        return (1, module)


# ---------------------------------------------------------------------------
# Shared signature model
# ---------------------------------------------------------------------------

NAME_PREFIXES = ("cell", "sce", "sys_")

_IDENT = r"[A-Za-z_][A-Za-z0-9_]*"


@dataclass
class Param:
    raw_type: str
    name: str
    is_pointer: bool
    base_type: str  # normalized scalar base type, e.g. "u32", "char", "void"


@dataclass
class Signature:
    name: str
    return_raw: str
    return_is_pointer: bool
    return_base: str
    params: list = field(default_factory=list)  # list[Param]
    file: str = ""
    line: int = 0
    # True if this function's body demonstrably treats its s32/int return
    # as a status code (see body_looks_like_status_return()). Only ever set
    # on OUR side (we have the body; for RPCS3 we deliberately never read
    # bodies -- signature-only, per the clean-room rule in the module
    # docstring). None means "not applicable / not scanned" (e.g. RPCS3
    # signatures, or a non-ambiguous return type).
    return_is_status_by_body: object = None

    def param_sig(self) -> str:
        return ", ".join(f"{p.raw_type} {p.name}".strip() for p in self.params)

    def pretty(self) -> str:
        return f"{self.return_raw} {self.name}({self.param_sig()})"


# ---------------------------------------------------------------------------
# Type normalization
# ---------------------------------------------------------------------------

# Scalar-equivalence classes: our spelling vs RPCS3's spelling collapse to the
# same "kind" for comparison purposes. This is intentionally coarse (KIND,
# not exact width) per the task spec -- we only want to catch the OUT-PARAM
# vs RETURN class and the POINTER vs VALUE class, not int-vs-long nitpicks.
_INT_ALIASES = {
    "u8", "u16", "u32", "u64", "s8", "s16", "s32", "s64",
    "int", "unsigned", "unsigned int", "unsigned char", "unsigned short",
    "unsigned long", "unsigned long long", "long", "long long",
    "short", "char", "signed char", "bool", "size_t", "ssize_t",
    "float", "f32", "f64", "double",
    "be_t<u8>", "be_t<u16>", "be_t<u32>", "be_t<u64>",
    "be_t<s8>", "be_t<s16>", "be_t<s32>", "be_t<s64>",
}

# Return-type "error status" family, UNAMBIGUOUS spellings only: a type
# whose name can *only* mean "this is a status code" on either side of the
# diff. error_code/CellError/CellFsErrno never denote a real payload value
# in this codebase, so treating them as "error" kind is safe.
#
# s32/int are DELIBERATELY EXCLUDED from this set: the Cell ABI is
# ambiguous about it (s32 is CELL_OK-or-negative-error in some functions,
# but a genuine signed count/value return in others, e.g.
# cellPamfReaderGetNumberOfStreams). Treating every s32 as "error" produced
# a measured false positive (that function flagged against RPCS3's `u8`
# even though both are plain value returns). s32/int are classified as
# "value" like any other scalar -- see AMBIGUOUS_STATUS_TYPES below for the
# one place their ambiguity still matters.
ERROR_RETURN_TYPES = {
    "error_code", "cellerror", "cellfserrno",
}

# s32/int returns are ambiguous (see above), but a s32/int return paired
# with a `void` return on the other side is still worth flagging: either
# we invented a status return the ABI doesn't have, or RPCS3 collapsed a
# real error path to void because it's asserted CELL_OK -- both are
# information a verify-then-implement reviewer wants to see. So: s32/int
# vs void keeps flagging (via classify_return's return kind = "error" only
# in that one contrast); s32/int vs another concrete integer type does not.
AMBIGUOUS_STATUS_TYPES = {"s32", "int"}

VOID_TYPES = {"void"}


def strip_cv(t: str) -> str:
    t = t.strip()
    t = re.sub(r"\bconst\b", "", t)
    t = re.sub(r"\bvolatile\b", "", t)
    t = re.sub(r"\s+", " ", t).strip()
    return t


def normalize_base(t: str) -> str:
    """Reduce a type spelling to a normalized base-type token."""
    t = strip_cv(t)
    t = t.replace("*", "").strip()
    t = re.sub(r"\s+", " ", t)

    # RPCS3 vm::ptr<T> / vm::cptr<T> / vm::bptr<T> / vm::pptr<T> -> T
    m = re.match(r"^vm::(?:ptr|cptr|bptr|pptr)<\s*(.+?)\s*>$", t)
    if m:
        t = m.group(1)
        t = strip_cv(t)

    # be_t<T> -> T (RPCS3 big-endian wrapper is transparent for our purposes)
    m = re.match(r"^be_t<\s*(.+?)\s*>$", t)
    if m:
        t = m.group(1)

    t = t.lower()
    if t in ("unsigned",):
        t = "unsigned int"
    return t


#: Names of our own typedefs that resolve (transitively) to a function
#: pointer, e.g. `typedef void (*CellGcmFlipHandler)(u32 head);`. Populated
#: by scan_our_fnptr_typedefs() before extraction runs. A guest callback
#: pointer is semantically a pointer (an address) on both sides, so these
#: must be classified as pointer-kind, matching RPCS3's `vm::ptr<void(...)>`
#: spelling of the same callback parameter -- otherwise every callback-taking
#: HLE function false-flags as param-pointer-vs-value.
OUR_FNPTR_TYPEDEFS: set = set()

#: Names of RPCS3-side `using X = vm::ptr<T>;` / `using X = vm::pptr<T>;`
#: aliases (e.g. cellSaveData.cpp's `PSetBuf`, `PSetList`). Populated by
#: scan_oracle_ptr_aliases(). Needed because RPCS3 sometimes types a
#: parameter with a local alias name instead of spelling out vm::ptr<...>
#: inline; without resolving the alias, the same false pointer-vs-value
#: mismatch appears, just with the polarity flipped (RPCS3 side looks like
#: a bare value because the alias name doesn't match any known pattern).
ORACLE_PTR_ALIASES: set = set()

#: Our own typedef-to-integer "handle" idiom used throughout the Cell SDK
#: (CellAdecHandle = u32, CellDmuxHandle = u32, ...). RPCS3 frequently
#: models the *same* guest-visible u32 handle as `vm::ptr<InternalContext>`
#: / `vm::pptr<InternalContext>` internally (the handle value IS the guest
#: address of RPCS3's own tracking struct) -- that is an RPCS3
#: implementation choice, not evidence the real PS3 ABI passes a pointer;
#: the actual wire-visible quantity is a 4-byte handle either way. Populated
#: by scan_our_handle_typedefs().
OUR_HANDLE_TYPEDEFS: set = set()

_HANDLE_NAME_RE = re.compile(r"^(Cell|Sce)[A-Za-z0-9]*Handle$")


def is_pointer_type(t: str) -> bool:
    """True if the RAW spelling denotes a pointer/out-param-capable type.

    Our side: trailing '*', OR a typedef name known (via
    OUR_FNPTR_TYPEDEFS) to resolve to a function pointer.
    RPCS3 side: vm::ptr<T> / vm::cptr<T> / vm::bptr<T> / vm::pptr<T>
    (vm::ref is rare and not used by module signatures we scan), OR a
    `using` alias (via ORACLE_PTR_ALIASES) that resolves to one of those.
    """
    t = strip_cv(t)
    bare = t.replace("*", "").strip()
    if bare in OUR_FNPTR_TYPEDEFS or bare in ORACLE_PTR_ALIASES:
        return True
    if re.match(r"^vm::(?:ptr|cptr|bptr|pptr)<", t):
        return True
    if "*" in t:
        return True
    return False


def is_handle_typedef(t: str) -> bool:
    """True if `t` is one of our `Cell*Handle`/`Sce*Handle` typedefs that
    resolve to a plain integer (the Cell SDK opaque-handle idiom)."""
    t = strip_cv(t).replace("*", "").strip()
    return t in OUR_HANDLE_TYPEDEFS or bool(_HANDLE_NAME_RE.match(t))


def make_param(raw_type: str, name: str) -> Param:
    raw_type = strip_cv(raw_type)
    return Param(
        raw_type=raw_type,
        name=name,
        is_pointer=is_pointer_type(raw_type),
        base_type=normalize_base(raw_type),
    )


_RETURN_STMT_RE = re.compile(r"return\s+([^;]*?)\s*;")
_STATUS_LITERAL_RE = re.compile(
    r"^\(?\s*(?:\([A-Za-z_][A-Za-z0-9_]*\)\s*)?"
    r"(?:CELL_OK"
    r"|CELL_[A-Za-z0-9_]*_ERROR[A-Za-z0-9_]*"      # CELL_GCM_ERROR_INVALID_VALUE
    r"|CELL_E[A-Z][A-Z0-9_]*"                      # CELL_EFAULT, CELL_ENOENT (POSIX-errno style)
    r"|SCE_[A-Za-z0-9_]*_ERROR[A-Za-z0-9_]*"
    r")\s*\)?$"
)


def find_matching_brace(text: str, open_brace_pos: int) -> int:
    """Given the index of a '{' in text, return the index of its matching
    '}' (naive depth counter; text_nc has comments already blanked out, so
    braces inside string literals are the only remaining risk, and none of
    the HLE sources in this repo put braces inside string literals in a
    function body in a way that would fool this)."""
    depth = 0
    i = open_brace_pos
    n = len(text)
    while i < n:
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return n - 1


def body_looks_like_status_return(body: str) -> bool:
    """True if EVERY `return <expr>;` in a function body's expr is a
    CELL_OK / CELL_*_ERROR* / SCE_*_ERROR* literal (bare, or cast to an
    error enum) -- i.e. the s32/int return is unambiguously a status code
    in THIS function, never a plain computed value.

    Requiring ALL returns (not just one) to be status literals is what
    distinguishes cellGcmGetTiledPitchSize (every return is CELL_OK or a
    CELL_GCM_ERROR_* constant) from cellPamfReaderGetNumberOfStreams (the
    validation path returns CELL_PAMF_ERROR_INVALID_ARG, but the success
    path returns `reader->numStreams` -- a real value, so this function is
    NOT status-only despite having one status-shaped return statement).
    A function with zero return statements (e.g. a stub that falls off the
    end) is not claimed as status-by-body either -- no evidence either way.
    """
    returns = [m.group(1).strip() for m in _RETURN_STMT_RE.finditer(body)]
    if not returns:
        return False
    return all(_STATUS_LITERAL_RE.match(r) for r in returns)


def classify_return(raw: str) -> tuple:
    """Returns (is_pointer, base_kind) for a return type.

    base_kind is one of: "void", "error", "value", "ambiguous-status",
    "pointer" -- the KIND axis the task spec cares about (real value vs
    void vs error-status vs pointer-return), not exact width.
    "ambiguous-status" (s32/int) compares equal to "value" EXCEPT against
    "void" -- see diff_signatures()'s return-kind block for the one place
    that distinction is applied.
    """
    raw = strip_cv(raw)
    if is_pointer_type(raw):
        return True, "pointer"
    base = normalize_base(raw)
    if base in VOID_TYPES:
        return False, "void"
    if base in ERROR_RETURN_TYPES:
        return False, "error"
    if base in AMBIGUOUS_STATUS_TYPES:
        return False, "ambiguous-status"
    if base in _INT_ALIASES:
        return False, "value"
    # Unknown aggregate/struct-by-value return, or a class type (RPCS3 has
    # some, e.g. custom result wrappers) -- treat as an opaque "value" kind
    # distinct from "error"/"void" so a value<->void/error mismatch still
    # flags, but two unknown-but-equal spellings don't false-positive.
    return False, f"value:{base}"


# ---------------------------------------------------------------------------
# Typedef/alias pre-scans (must run before signature extraction so the
# pointer/handle classifiers above have their name sets populated)
# ---------------------------------------------------------------------------

_OUR_FNPTR_TYPEDEF_RE = re.compile(
    r"typedef\s+[A-Za-z_][A-Za-z0-9_\s]*?\(\s*\*\s*(" + _IDENT + r")\s*\)\s*\("
)

_OUR_HANDLE_TYPEDEF_RE = re.compile(
    r"typedef\s+(?:u8|u16|u32|u64|s8|s16|s32|s64|int|unsigned\s+int)\s+"
    r"(" + _IDENT + r")\s*;"
)

_ORACLE_PTR_ALIAS_RE = re.compile(
    r"^using\s+(" + _IDENT + r")\s*=\s*vm::(?:ptr|cptr|bptr|pptr)\s*<",
    re.MULTILINE,
)


def scan_our_fnptr_typedefs(root: str) -> set:
    """Scan libs/**/*.h (and .c) for `typedef Ret (*Name)(...)` function
    pointer typedefs; these are guest callback addresses and must be
    classified as pointer-kind (see OUR_FNPTR_TYPEDEFS docstring above)."""
    names = set()
    for dirpath, _dirnames, filenames in os.walk(root):
        for fn in filenames:
            if not (fn.endswith(".h") or fn.endswith(".c")):
                continue
            path = os.path.join(dirpath, fn)
            try:
                with open(path, "r", encoding="utf-8", errors="replace") as f:
                    text = f.read()
            except OSError:
                continue
            for m in _OUR_FNPTR_TYPEDEF_RE.finditer(text):
                names.add(m.group(1))
    return names


def scan_our_handle_typedefs(root: str) -> set:
    """Scan libs/**/*.h for `typedef u32 CellXxxHandle;`-shaped opaque-handle
    typedefs (see OUR_HANDLE_TYPEDEFS docstring above)."""
    names = set()
    for dirpath, _dirnames, filenames in os.walk(root):
        for fn in filenames:
            if not fn.endswith(".h"):
                continue
            path = os.path.join(dirpath, fn)
            try:
                with open(path, "r", encoding="utf-8", errors="replace") as f:
                    text = f.read()
            except OSError:
                continue
            for m in _OUR_HANDLE_TYPEDEF_RE.finditer(text):
                name = m.group(1)
                if _HANDLE_NAME_RE.match(name):
                    names.add(name)
    return names


def scan_oracle_ptr_aliases(root: str) -> set:
    """Scan RPCS3's Modules/**/*.cpp and *.h for `using X = vm::ptr<T>;`
    (and cptr/bptr/pptr) local type aliases (see ORACLE_PTR_ALIASES
    docstring above)."""
    names = set()
    for dirpath, _dirnames, filenames in os.walk(root):
        for fn in filenames:
            if not (fn.endswith(".cpp") or fn.endswith(".h")):
                continue
            path = os.path.join(dirpath, fn)
            try:
                with open(path, "r", encoding="utf-8", errors="replace") as f:
                    text = f.read()
            except OSError:
                continue
            for m in _ORACLE_PTR_ALIAS_RE.finditer(text):
                names.add(m.group(1))
    return names


# ---------------------------------------------------------------------------
# Our side: libs/**/*.c parser
# ---------------------------------------------------------------------------

# A "definition head" is: <return-type> <name>(<params>) NOT followed by ';'
# i.e. it has a matching '{' (allowing whitespace/newlines in between).
# We scan brace-depth-aware across the whole file so multi-line signatures
# (e.g. cellGcmSetDisplayBuffer's wrapped params) are joined correctly.

_RETTYPE_HEAD = re.compile(
    r"^(?P<static>static\s+)?"
    r"(?P<ret>(?:const\s+)?[A-Za-z_][A-Za-z0-9_:<>\s\*]*?)\s+"
    r"(?P<name>" + _IDENT + r")\s*\((?P<params>[^;{}]*)\)\s*$",
    re.MULTILINE,
)


def _split_top_level_commas(s: str):
    parts = []
    depth = 0
    cur = []
    for ch in s:
        if ch in "<(":
            depth += 1
            cur.append(ch)
        elif ch in ">)":
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


def parse_params(params_str: str) -> list:
    params_str = params_str.strip()
    if params_str == "" or params_str == "void":
        return []
    out = []
    for part in _split_top_level_commas(params_str):
        part = part.strip()
        if not part:
            continue
        # Drop default values (not expected in C, but be safe)
        part = re.split(r"=", part, maxsplit=1)[0].strip()
        # Function pointer param: RetType (*name)(args)
        m = re.match(r"^(.+?\(\s*\*\s*)(" + _IDENT + r")(\s*\).*)$", part)
        if m:
            raw_type = (m.group(1) + m.group(3)).strip()
            name = m.group(2)
            out.append(make_param(raw_type, name))
            continue
        # Normal: Type name  (name may be absent, e.g. prototypes -- rare in .c)
        m = re.match(r"^(.*?[\s\*])(" + _IDENT + r")$", part)
        if m:
            raw_type = m.group(1).strip()
            name = m.group(2)
        else:
            # No name at all (just a type, e.g. "void")
            raw_type = part
            name = ""
        out.append(make_param(raw_type, name))
    return out


def extract_ours(root: str) -> dict:
    """Scan libs/**/*.c for exported (non-static) function definitions whose
    name starts with a recognized PS3 module prefix. Returns name -> Signature
    (first definition wins if duplicated -- duplicates are rare/a separate
    issue, not this tool's job).
    """
    sigs = {}
    for dirpath, _dirnames, filenames in os.walk(root):
        for fn in filenames:
            if not fn.endswith(".c"):
                continue
            path = os.path.join(dirpath, fn)
            try:
                with open(path, "r", encoding="utf-8", errors="replace") as f:
                    text = f.read()
            except OSError:
                continue

            # Strip comments (block and line) so brace/paren scanning across
            # commented-out signatures doesn't misfire, and so a ')' inside a
            # comment never confuses the head matcher.
            text_nc = re.sub(r"/\*.*?\*/", lambda m: re.sub(r"[^\n]", " ", m.group(0)), text, flags=re.DOTALL)
            text_nc = re.sub(r"//[^\n]*", "", text_nc)

            for m in re.finditer(
                r"^(?P<static>static\s+)?"
                r"(?P<ret>[A-Za-z_][A-Za-z0-9_:<>\s\*]*?)\s+"
                r"(?P<name>" + _IDENT + r")\s*\(\s*(?P<params>[^;{}]*?)\s*\)\s*\n?\s*\{",
                text_nc,
                flags=re.MULTILINE,
            ):
                if m.group("static"):
                    continue
                name = m.group("name")
                if not name.startswith(NAME_PREFIXES):
                    continue
                if name in ("sys_fs",):  # guard against accidental prefix-only match
                    continue
                ret_raw = m.group("ret").strip()
                # Reject false positives: control-flow keywords, or a
                # "return" caught by the regex, etc.
                if ret_raw in ("return", "else", "if", "while", "for", "switch"):
                    continue
                params_raw = m.group("params")
                line_no = text_nc.count("\n", 0, m.start()) + 1
                is_ptr, base_kind = classify_return(ret_raw)

                status_by_body = None
                if base_kind == "ambiguous-status":
                    # m.end() - 1 is the '{' itself (regex ends "...\{").
                    open_brace = m.end() - 1
                    close_brace = find_matching_brace(text_nc, open_brace)
                    body = text_nc[open_brace:close_brace + 1]
                    status_by_body = body_looks_like_status_return(body)

                sig = Signature(
                    name=name,
                    return_raw=ret_raw,
                    return_is_pointer=is_ptr,
                    return_base=base_kind,
                    params=parse_params(params_raw),
                    file=os.path.relpath(path, root_start(root)),
                    line=line_no,
                    return_is_status_by_body=status_by_body,
                )
                if name not in sigs:
                    sigs[name] = sig
    return sigs


def root_start(root: str) -> str:
    # Report paths relative to the repo root (parent of libs/), matching the
    # convention used elsewhere in this repo's reports (e.g. libs/video/x.c).
    return os.path.dirname(os.path.normpath(root))


# ---------------------------------------------------------------------------
# Oracle side: rpcs3/rpcs3/Emu/Cell/Modules/*.cpp parser
# ---------------------------------------------------------------------------

REG_FUNC_RE = re.compile(
    r"REG_FUNC\s*\(\s*(?P<module>" + _IDENT + r")\s*,\s*(?P<name>" + _IDENT + r")\s*\)"
)


def extract_oracle(root: str) -> tuple:
    """Returns (sigs, name_to_module) where sigs: name -> Signature and
    name_to_module: name -> module string (from REG_FUNC registrations, the
    authoritative "this is a real HLE export" signal)."""
    sigs = {}
    name_to_module = {}

    for dirpath, _dirnames, filenames in os.walk(root):
        for fn in filenames:
            if not fn.endswith(".cpp"):
                continue
            path = os.path.join(dirpath, fn)
            try:
                with open(path, "r", encoding="utf-8", errors="replace") as f:
                    text = f.read()
            except OSError:
                continue

            for m in REG_FUNC_RE.finditer(text):
                name_to_module.setdefault(m.group("name"), m.group("module"))

            text_nc = re.sub(r"/\*.*?\*/", lambda mm: re.sub(r"[^\n]", " ", mm.group(0)), text, flags=re.DOTALL)
            text_nc = re.sub(r"//[^\n]*", "", text_nc)

            for m in re.finditer(
                r"^(?P<ret>[A-Za-z_][A-Za-z0-9_:<>\s\*&,]*?)\s+"
                r"(?P<name>" + _IDENT + r")\s*\(\s*(?P<params>[^;{}]*?)\s*\)\s*\n?\s*\{",
                text_nc,
                flags=re.MULTILINE,
            ):
                name = m.group("name")
                if not name.startswith(NAME_PREFIXES):
                    continue
                if "::" in m.group("ret"):
                    # e.g. "audio_ringbuffer::backend_write_callback" is
                    # actually matched with ret holding the class-qualified
                    # leftover; also guard the name itself below.
                    pass
                ret_raw = m.group("ret").strip()
                if ret_raw in ("return", "else", "if", "while", "for", "switch", "template"):
                    continue
                # Skip templated / out-of-line class-scoped defs like
                # "void fmt_class_string<CellAudioError>::format" -- those
                # have "::" immediately before the matched name's own text;
                # detect via a scan backwards for "::" right before name.
                pre_start = max(0, m.start("name") - 3)
                if text_nc[pre_start:m.start("name")].endswith("::"):
                    continue
                params_raw = m.group("params")
                line_no = text_nc.count("\n", 0, m.start()) + 1

                params = parse_params(params_raw)
                # Strip RPCS3's implicit leading ppu_thread& (or cpu_thread&)
                # context param -- it's plumbing, not part of the PS3 ABI the
                # guest program observes.
                if params and re.match(r"^(ppu_thread|cpu_thread|spu_thread)\s*&$",
                                        strip_cv(params[0].raw_type)):
                    params = params[1:]

                is_ptr, base_kind = classify_return(ret_raw)
                sig = Signature(
                    name=name,
                    return_raw=ret_raw,
                    return_is_pointer=is_ptr,
                    return_base=base_kind,
                    params=params,
                    file=os.path.relpath(path, root_start_oracle(root)),
                    line=line_no,
                )
                # First definition wins (some modules have forward decls with
                # bodies further down guarded by #ifdef -- rare; good enough).
                if name not in sigs:
                    sigs[name] = sig
    return sigs, name_to_module


def root_start_oracle(root: str) -> str:
    return root  # report paths relative to rpcs3/ itself (already deep enough)


# ---------------------------------------------------------------------------
# Diff logic
# ---------------------------------------------------------------------------

@dataclass
class Mismatch:
    name: str
    module: str
    kind: str  # "return-kind" | "param-count" | "param-pointer-vs-value"
    detail: str
    ours: Signature
    theirs: Signature


def diff_signatures(ours: Signature, theirs: Signature) -> list:
    problems = []

    # --- Return type KIND mismatch ---
    ours_kind = ours.return_base.split(":")[0]
    theirs_kind = theirs.return_base.split(":")[0]

    def _effective_kind(sig: Signature, k: str, other_k: str) -> str:
        # "ambiguous-status" (s32/int) is resolved, in priority order:
        #  1. body evidence (OUR side only -- does the function's own body
        #     `return CELL_OK` / a CELL_*_ERROR* constant? If so it's
        #     unambiguously a status code in THIS function, regardless of
        #     what the other side returns.
        #  2. otherwise collapse to "value" UNLESS the other side is
        #     "void" -- an invented/dropped status return against a void
        #     ABI is still worth flagging (see AMBIGUOUS_STATUS_TYPES).
        if k == "ambiguous-status":
            if sig.return_is_status_by_body:
                return "error"
            return "error" if other_k == "void" else "value"
        return k

    ours_eff = _effective_kind(ours, ours_kind, theirs_kind)
    theirs_eff = _effective_kind(theirs, theirs_kind, ours_kind)

    if ours.return_is_pointer != theirs.return_is_pointer:
        problems.append((
            "return-kind",
            f"pointer-return mismatch: ours={'ptr' if ours.return_is_pointer else 'value'} "
            f"({ours.return_raw}) vs RPCS3={'ptr' if theirs.return_is_pointer else 'value'} "
            f"({theirs.return_raw})",
        ))
    elif ours_eff != theirs_eff:
        # The headline bug class: one side is void/error (out-param carries
        # the real result) and the other side returns a real value.
        problems.append((
            "return-kind",
            f"ours returns {ours_eff} ({ours.return_raw}) but RPCS3 returns "
            f"{theirs_eff} ({theirs.return_raw})",
        ))

    # --- Param count mismatch ---
    count_differs = len(ours.params) != len(theirs.params)
    if count_differs:
        problems.append((
            "param-count",
            f"ours has {len(ours.params)} param(s), RPCS3 has {len(theirs.params)}",
        ))

    # --- Per-position pointer-vs-value mismatch (only over the common
    #     prefix length; count mismatch already flagged above). CAVEAT: when
    #     param-count also differs, a position-by-position compare can be
    #     comparing params that don't actually correspond (e.g. RPCS3 has an
    #     extra leading request-handle param that shifts everything after
    #     it by one) -- flagged findings in that case are marked
    #     "(unaligned)" so the report doesn't overstate confidence in the
    #     exact position, while still surfacing that SOME divergence exists.
    #     A count match means positions are apples-to-apples.
    for i in range(min(len(ours.params), len(theirs.params))):
        po, pt = ours.params[i], theirs.params[i]
        if po.is_pointer != pt.is_pointer:
            # BENIGN: our side spells an opaque Cell SDK handle as a
            # typedef'd integer (CellAdecHandle = u32) while RPCS3 spells
            # the *same* guest-visible 4-byte handle as vm::ptr<InternalCtx>
            # / vm::pptr<InternalCtx> (RPCS3 uses the handle value as the
            # guest address of its own tracking struct). Both sides carry
            # one pointer-sized guest quantity; this is not a real ABI
            # divergence -- see is_handle_typedef()'s docstring.
            if (not po.is_pointer and is_handle_typedef(po.raw_type) and pt.is_pointer):
                continue
            tag = " (unaligned -- param count differs, position may not correspond)" if count_differs else ""
            problems.append((
                "param-pointer-vs-value",
                f"param #{i + 1}: ours `{po.raw_type} {po.name}` is "
                f"{'a pointer' if po.is_pointer else 'a value'} but RPCS3's "
                f"`{pt.raw_type}` is {'a pointer' if pt.is_pointer else 'a value'}{tag}",
            ))

    return problems


# ---------------------------------------------------------------------------
# Whitelist
# ---------------------------------------------------------------------------

def load_whitelist(path: str) -> dict:
    """Whitelist file format (one entry per line, '#'-comments allowed):

        function_name : free-text reason

    A whitelisted name suppresses ALL mismatch kinds for that function. This
    is deliberately coarse (per-function, not per-kind) to keep the file
    simple to audit; add a fresh line with a reason for every suppression.
    """
    entries = {}
    if not os.path.isfile(path):
        return entries
    with open(path, "r", encoding="utf-8") as f:
        for raw_line in f:
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            if ":" not in line:
                continue
            name, reason = line.split(":", 1)
            entries[name.strip()] = reason.strip()
    return entries


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------

def build_report(libs_root: str, oracle_root: str, whitelist_path: str):
    # Pre-scan typedefs/aliases on both sides so the pointer/handle
    # classifiers have full name sets before any signature is parsed.
    global OUR_FNPTR_TYPEDEFS, ORACLE_PTR_ALIASES, OUR_HANDLE_TYPEDEFS
    OUR_FNPTR_TYPEDEFS = scan_our_fnptr_typedefs(libs_root)
    ORACLE_PTR_ALIASES = scan_oracle_ptr_aliases(oracle_root)
    OUR_HANDLE_TYPEDEFS = scan_our_handle_typedefs(libs_root)

    ours = extract_ours(libs_root)
    theirs, name_to_module = extract_oracle(oracle_root)
    whitelist = load_whitelist(whitelist_path)

    mismatches = []
    matched = []
    ours_only = []
    theirs_only = []

    for name, our_sig in sorted(ours.items()):
        their_sig = theirs.get(name)
        if their_sig is None:
            ours_only.append(our_sig)
            continue
        problems = diff_signatures(our_sig, their_sig)
        if problems:
            if name in whitelist:
                continue
            module = name_to_module.get(name, "?")
            for kind, detail in problems:
                mismatches.append(Mismatch(name, module, kind, detail, our_sig, their_sig))
        else:
            matched.append(name)

    for name, their_sig in sorted(theirs.items()):
        if name not in ours:
            theirs_only.append(their_sig)

    mismatches.sort(key=lambda m: (module_rank(m.module), m.name, m.kind))

    return {
        "ours_total": len(ours),
        "theirs_total": len(theirs),
        "matched": matched,
        "mismatches": mismatches,
        "ours_only": ours_only,
        "theirs_only": theirs_only,
        "whitelist": whitelist,
    }


def format_report(report: dict) -> str:
    lines = []
    lines.append("HLE ABI audit -- ours (libs/**/*.c) vs RPCS3 oracle (rpcs3/.../Modules/*.cpp)")
    lines.append(f"functions parsed: ours={report['ours_total']}  RPCS3={report['theirs_total']}")
    lines.append(f"name overlap checked: {len(report['matched']) + len({m.name for m in report['mismatches']})}")
    lines.append(f"matched (ABI-compatible): {len(report['matched'])}")
    lines.append(f"MISMATCHES: {len({m.name for m in report['mismatches']})} function(s), "
                 f"{len(report['mismatches'])} finding(s)")
    lines.append("")

    if report["mismatches"]:
        lines.append("=" * 78)
        lines.append("MISMATCHES (grouped by module, boot-relevance order)")
        lines.append("=" * 78)
        current_module = None
        for m in report["mismatches"]:
            if m.module != current_module:
                current_module = m.module
                lines.append("")
                lines.append(f"-- module: {current_module} --")
            lines.append(f"[{m.kind}] {m.name}")
            lines.append(f"    ours   : {m.ours.pretty()}")
            lines.append(f"             ({m.ours.file}:{m.ours.line})")
            lines.append(f"    RPCS3  : {m.theirs.pretty()}")
            lines.append(f"             ({m.theirs.file}:{m.theirs.line})")
            lines.append(f"    reason : {m.detail}")
            lines.append("")
    else:
        lines.append("(no mismatches)")

    lines.append("=" * 78)
    lines.append(f"INFO: ours-only (RPCS3 doesn't implement/we couldn't match) = {len(report['ours_only'])}")
    lines.append(f"INFO: RPCS3-only (we don't implement) = {len(report['theirs_only'])}")
    lines.append("=" * 78)

    return "\n".join(lines)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    here = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.dirname(here)
    parser.add_argument("--libs", default=os.path.join(repo_root, "libs"),
                         help="root of our HLE C sources (default: libs/)")
    parser.add_argument("--oracle", default=os.path.join(repo_root, "rpcs3", "rpcs3", "Emu", "Cell", "Modules"),
                         help="root of RPCS3's Modules/ (default: rpcs3/rpcs3/Emu/Cell/Modules)")
    parser.add_argument("--whitelist", default=os.path.join(here, "hle_abi_audit_whitelist.txt"),
                         help="known-benign name:reason list")
    parser.add_argument("--json", default=None, help="also write a JSON report to this path")
    args = parser.parse_args(argv)

    if not os.path.isdir(args.libs):
        print(f"error: libs root not found: {args.libs}", file=sys.stderr)
        return 2
    if not os.path.isdir(args.oracle):
        print(f"error: oracle root not found: {args.oracle}", file=sys.stderr)
        return 2

    report = build_report(args.libs, args.oracle, args.whitelist)
    print(format_report(report))

    if args.json:
        def sig_json(s: Signature) -> dict:
            return {
                "name": s.name,
                "return_type": s.return_raw,
                "params": [{"type": p.raw_type, "name": p.name} for p in s.params],
                "file": s.file,
                "line": s.line,
            }

        payload = {
            "ours_total": report["ours_total"],
            "theirs_total": report["theirs_total"],
            "matched": report["matched"],
            "mismatches": [
                {
                    "name": m.name,
                    "module": m.module,
                    "kind": m.kind,
                    "detail": m.detail,
                    "ours": sig_json(m.ours),
                    "rpcs3": sig_json(m.theirs),
                }
                for m in report["mismatches"]
            ],
            "ours_only": [sig_json(s) for s in report["ours_only"]],
            "theirs_only": [sig_json(s) for s in report["theirs_only"]],
        }
        with open(args.json, "w", encoding="utf-8", newline="\n") as f:
            json.dump(payload, f, indent=2)
            f.write("\n")

    return 1 if report["mismatches"] else 0


if __name__ == "__main__":
    sys.exit(main())
