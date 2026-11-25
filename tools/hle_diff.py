#!/usr/bin/env python3
"""hle_diff.py - differential firmware call trace: a recompiled title vs RPCS3.

Compares two ps3-calltrace/1 traces (tools/calltrace.py), typically the port's
(PS3_CALLTRACE=<file>) against RPCS3's (tools/rpcs3_probe/rpcs3_calltrace.py),
and reports where the two firmwares first behave differently for the same
title code:

  * threads are paired by name and creation order (thread ids differ);
  * each pair is aligned by call sequence (NID / syscall number), resyncing
    over calls one side makes and the other does not;
  * paired calls compare the return value, the arguments (only as many as
    the function takes, when known) and the guest memory the call wrote
    through its pointer arguments;
  * expected differences are tolerated: handles, ids and guest addresses are
    mapped on first use (0x85000100 on one side may be 3 on the other, as
    long as it stays so), time and input reads are volatile, polling loops
    are collapsed.

The report gives the first divergence per thread (the place to start) and a
summary per function: calls, mismatched returns, mismatched written memory,
calls present on only one side. Documentation: docs/HLE_TRACE_DIFF.md.

Usage:
  python tools/hle_diff.py PORT.jsonl RPCS3.jsonl [--imports EBOOT.imports.json]
         [--only PATTERNS] [--ignore PATTERNS] [--volatile PATTERNS]
         [--pair PORT_TID=RPCS3_TID ...] [--addr-map PORT_BASE=REF_BASE[:SIZE] ...]
         [--rank first|count] [--json out.json] [--top 40]

PATTERNS: comma list of function names (prefix with a trailing *), library
names (with --imports), NIDs (0x1234ABCD), "sc" or "sc:N".

Python 3.9+, standard library only.
"""
import argparse
import bisect
import fnmatch
import json
import os
import sys
from collections import defaultdict, OrderedDict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from calltrace import Trace, key_of, u, syscall_names  # noqa: E402

# Results that depend on when or with what input the call ran: their return
# value and written memory are not compared (the call itself still aligns).
DEFAULT_VOLATILE = [
    "sys_time_*", "_sys_time_*", "cellRtcGetCurrent*", "cellRtcGetTick",
    "cellPadGetData*", "cellPadGetInfo*", "cellPadGetRawData", "cellPadGetCapabilityInfo",
    "cellKbRead", "cellMouseGetData*", "cellAudioGetPortTimestamp",
    "cellAudioGetPortBlockTag", "cellGcmGetLastFlipTime", "cellGcmGetVBlankCount",
    "cellGcmGetFlipStatus", "cellGcmGetCurrentField", "sys_timer_get_information",
    "sys_process_get_status", "sys_memory_get_user_memory_size",
    "sys_ppu_thread_get_id", "sys_lwmutex_*", "sys_lwcond_*", "_sys_lwmutex_*",
    "_sys_lwcond_*", "sys_spinlock_*", "_sys_heap_*", "sys_rsx_*",
]

SEV = {"ret": 3, "mem": 3, "args": 2, "only": 2, "mem1": 1}
# Ranking tiers (--rank first): a host pointer returned first, then a call that returned, was given or wrote
# something different comes first, then calls made on one side only, then
# memory written on one side only (soft: see the window definition in the docs).
TIER = {"ret": 0, "args": 0, "mem": 0, "only": 1, "mem1": 2}
TIER_NAME = {-1: "host", 0: "value", 1: "only", 2: "mem1"}
HOST_TIER = -1      # a 64-bit return against a 32-bit one: a host pointer, always a bug


def parse_addr_map(specs):
    """--addr-map PORT_BASE=REF_BASE[:SIZE] (hex; SIZE defaults to 0x10000000):
    addresses in [PORT_BASE, PORT_BASE+SIZE) on the first side are the same
    addresses as REF_BASE+offset on the second (two layouts of user memory)."""
    out = []
    for spec in specs:
        try:
            a, rest = spec.split("=", 1)
            b, _, size = rest.partition(":")
            out.append((int(a, 16), int(b, 16), int(size, 16) if size else 0x10000000))
        except ValueError:
            raise SystemExit("--addr-map %s: expected PORT_BASE=REF_BASE[:SIZE] in hex" % spec)
    return out


QUANTITY_WORDS = ("Size", "Count", "Num", "Number", "Length", "Version", "Capacity")


def returns_quantity(name):
    """A function named for a quantity (cellGcmGetDefaultCommandWordSize,
    ...GetCount): its return is a value, never an id or address to map."""
    import re
    return bool(name) and bool(re.search(r"(?:%s)(?:[A-Z0-9_]|$)" % "|".join(QUANTITY_WORDS), name))


def high_word(v):
    """Upper 32 bits of a 64-bit r3, with sign extension of a 32-bit value
    (0xFFFFFFFF8001xxxx, an error code as s32) counted as none."""
    h = (v >> 32) & 0xFFFFFFFF
    return 0 if h == 0xFFFFFFFF and v & 0x80000000 else h


def is_error(v):
    """CELL error codes are 0x80XXyyyy with XX != 0. RPCS3's lv2 ids are
    0x8X000000 + n and are not errors."""
    return (v >> 24) & 0xF0 == 0x80 and (v >> 16) & 0xFF != 0


class Mapper:
    """Bijective value map for handles/ids/addresses, bound on first use.

    Kept per type tag (the top byte of the other side's value): RPCS3 tags its
    lv2 ids by object type (0x85.. mutex, 0x86.. cond) while an HLE may number
    every type from 1, so mutex 4 and cond 4 here are two different ids
    there."""

    def __init__(self, regions=()):
        self.ab, self.ba = {}, {}
        self.regions = list(regions)     # [(base_a, base_b, size)]

    def xlate(self, a):
        """A value of the first side in a mapped address region, moved to the
        other side's layout (--addr-map); anything else unchanged."""
        for base_a, base_b, size in self.regions:
            if base_a <= a < base_a + size:
                return a - base_a + base_b
        return a

    def same(self, a, b):
        if a == b or self.xlate(a) == b:
            return "eq"
        if is_error(a) or is_error(b):
            return None
        if a < 0x10000 and b < 0x10000:
            return None        # small numbers (counts, sizes, flags) must match
        ka, kb = (a, b >> 24), (b, a >> 24)
        if self.ab.get(ka) == b:
            return "map"
        if ka in self.ab or kb in self.ba:
            return None
        self.ab[ka], self.ba[kb] = b, a
        return "map"


class Matcher:
    def __init__(self, patterns, libs):
        self.pats = [p.strip() for p in (patterns or "").split(",") if p.strip()]
        self.libs = libs

    def __bool__(self):
        return bool(self.pats)

    def match(self, key, name):
        for p in self.pats:
            if p == "sc" and key[0] == "sc":
                return True
            if p.startswith("sc:") and key == ("sc", int(p[3:])):
                return True
            if p.lower().startswith("0x") and key[0] == "hle" and key[1] == int(p, 16):
                return True
            if name and fnmatch.fnmatchcase(name, p):
                return True
            lib = self.libs.get(key)
            if lib and fnmatch.fnmatchcase(lib, p):
                return True
        return False


def norm_thread_name(n):
    n = (n or "").strip().lower()
    return "main" if n in ("main", "main_thread", "_main_thread") else n


def pair_threads(ta, tb, forced):
    pairs = [(int(a), int(b)) for a, b in (f.split("=") for f in forced)]
    used_a = {a for a, _ in pairs}
    used_b = {b for _, b in pairs}

    def groups(t, used):
        g = OrderedDict()
        for tid in sorted(t.calls, key=lambda x: t.first_seen[x]):
            if tid in used:
                continue
            g.setdefault(norm_thread_name(t.thread_names.get(tid)), []).append(tid)
        return g

    ga, gb = groups(ta, used_a), groups(tb, used_b)
    for name, la in ga.items():
        lb = gb.get(name, [])
        for a, b in zip(la, lb):
            pairs.append((a, b))
    pa = {a for a, _ in pairs}
    pb = {b for _, b in pairs}
    return pairs, [t for t in ta.calls if t not in pa], [t for t in tb.calls if t not in pb]


def lead_args(c, arity):
    """The arguments the function takes (r3 alone when its arity is unknown),
    low 32 bits: what makes two consecutive calls the same poll."""
    n = c.get("argc", arity.get(key_of(c), 1))
    a = c.get("args") or []
    return tuple((u(x) & 0xFFFFFFFF) if x is not None else None for x in a[:n])


def collapse(calls, arity):
    """Consecutive calls of one function with the same arguments and result ->
    one entry with a repeat count. A poll that spins 3 times here and 40 times
    there aligns; a loop over pads 0..3 does not merge."""
    out = []
    for c in calls:
        if out:
            p = out[-1]
            if key_of(p) == key_of(c) and p.get("ret") == c.get("ret") and not c.get("wr") and not p.get("wr") \
                    and lead_args(p, arity) == lead_args(c, arity):
                p["_rep"] = p.get("_rep", 1) + 1
                continue
        out.append(dict(c))
    return out


def align(a, b, window=256, key=key_of):
    """Greedy alignment of two call lists by key (the function, by default)
    with bounded resync. Yields ("pair", i, j), ("onlyA", i, None),
    ("onlyB", None, j)."""
    ka = [key(c) for c in a]
    kb = [key(c) for c in b]
    pos_b = defaultdict(list)
    for j, k in enumerate(kb):
        pos_b[k].append(j)
    i = j = 0
    while i < len(a) and j < len(b):
        if ka[i] == kb[j]:
            yield ("pair", i, j)
            i += 1
            j += 1
            continue
        best = None
        for di in range(0, min(window, len(a) - i)):
            if best and di >= best[0] + best[1]:
                break
            lst = pos_b.get(ka[i + di])
            if not lst:
                continue
            x = bisect.bisect_left(lst, j)
            if x < len(lst) and lst[x] - j < window:
                dj = lst[x] - j
                # prefer a resync that the next call confirms
                conf = (i + di + 1 < len(a) and lst[x] + 1 < len(b) and ka[i + di + 1] == kb[lst[x] + 1])
                cost = di + dj + (0 if conf else 2)
                if best is None or cost < best[0] + best[1] + best[2]:
                    best = (di, dj, 0 if conf else 2)
        if best is None:
            yield ("onlyA", i, None)
            yield ("onlyB", None, j)
            i += 1
            j += 1
            continue
        for k in range(best[0]):
            yield ("onlyA", i + k, None)
        for k in range(best[1]):
            yield ("onlyB", None, j + k)
        i += best[0]
        j += best[1]
    # Past the end of the shorter side the other simply ran longer (more
    # frames, a later kill): its tail is not a divergence.
    for k in range(i, len(a)):
        yield ("tailA", k, None)
    for k in range(j, len(b)):
        yield ("tailB", None, k)


def wr_map(c, argc):
    m = {}
    for arg, off, ln, data in c.get("wr") or []:
        if argc is not None and arg >= argc:
            continue        # a register the function does not take: not a pointer arg
        m[(arg, off)] = (ln, data)
    return m


def cmp_words(da, db, mapper):
    """Two equal-length hex runs, compared as big-endian words with id mapping."""
    if len(da) != len(db) or len(da) % 8:
        return da == db
    for k in range(0, len(da), 8):
        wa, wb = int(da[k:k + 8], 16), int(db[k:k + 8], 16)
        if wa != wb and mapper.same(wa, wb) is None:
            return False
    return True


def runs_same_mapped(addr, da, db, mapper):
    """Two equal-length written runs at guest address `addr` that differ only
    by --addr-map translation. A run is byte-granular (bytes a call wrote with
    the value they already had are not in it), so an address stored in guest
    memory often shows as a partial word ("40" against "30" when only the top
    byte changed): each 4-byte word the run touches is compared with the bytes
    outside the run taken as zero on both sides."""
    if not mapper.regions or da.startswith("#") or db.startswith("#") or len(da) != len(db):
        return False
    try:
        ba, bb = bytes.fromhex(da), bytes.fromhex(db)
    except ValueError:
        return False
    end = addr + len(ba)
    for w in range(addr & ~3, end, 4):
        wa = wb = 0
        for p in range(w, w + 4):
            inside = addr <= p < end
            wa = (wa << 8) | (ba[p - addr] if inside else 0)
            wb = (wb << 8) | (bb[p - addr] if inside else 0)
        if wa != wb and mapper.xlate(wa) != wb:
            return False
    return True


def arg_kinds(text):
    """Kinds of a function's arguments from RPCS3's formatting of them:
    'ptr' (*0x...), 'id' (a name ending in id / handle), else 'val'."""
    import re
    out = []
    for part in re.split(r",\s*(?![^()]*\))", text or ""):
        if "=" not in part:
            continue
        n, v = part.split("=", 1)
        n, v = n.strip().lower(), v.strip()
        if v.startswith("*"):
            out.append("ptr")
        elif n.endswith("id") or "handle" in n or n.endswith("_id") or n in ("fd", "port", "queue", "eq"):
            out.append("id")
        else:
            out.append("val")
    return out


def compare(ca, cb, mapper, volatile, arity, win, kinds=None):
    """Differences between two paired calls: list of (kind, detail)."""
    out = []
    key = key_of(ca)
    argc = ca.get("argc", cb.get("argc", arity.get(key)))
    ra, rb = u(ca.get("ret")), u(cb.get("ret"))
    if volatile:
        # Even on volatile functions (input polls, timestamps), if both sides
        # return a CELL error code and the codes disagree, that is an ABI bug
        # (e.g. wrong error base 0x80121205 vs 0x80121007), never timing noise.
        if ra is not None and rb is not None:
            ra32, rb32 = ra & 0xFFFFFFFF, rb & 0xFFFFFFFF
            if is_error(ra32) and is_error(rb32) and ra32 != rb32:
                out.append(("ret", "ret 0x%08X vs 0x%08X" % (ra32, rb32)))
    else:
        if ra is not None and rb is not None and high_word(ra) != high_word(rb):
            # a 64-bit value where the other side has a 32-bit one: typically
            # a host pointer handed to the guest (guest addresses are 32-bit)
            out.append(("ret", "ret 0x%X vs 0x%X (upper 32 bits)" % (ra, rb)))
        elif ra is not None and rb is not None:
            ra &= 0xFFFFFFFF
            rb &= 0xFFFFFFFF
            if returns_quantity(ca.get("name")) and ra != rb:
                out.append(("ret", "ret 0x%08X vs 0x%08X" % (ra, rb)))
            elif mapper.same(ra, rb) is None:
                out.append(("ret", "ret 0x%08X vs 0x%08X" % (ra, rb)))
    aa, ab = ca.get("args"), cb.get("args")
    if aa and ab and argc:
        bad = []
        for k in range(min(argc, len(aa), len(ab))):
            if aa[k] is None or ab[k] is None:
                continue        # a value the capture could not read as a number
            x, y = u(aa[k]) & 0xFFFFFFFF, u(ab[k]) & 0xFFFFFFFF
            kind = kinds[k] if kinds and k < len(kinds) else None
            if kind == "val" and x != y and mapper.xlate(x) != y:
                bad.append("r%d 0x%X vs 0x%X" % (k + 3, x, y))
            elif kind != "val" and mapper.same(x, y) is None:
                bad.append("r%d 0x%X vs 0x%X" % (k + 3, x, y))
        if bad:
            out.append(("args", ", ".join(bad)))
    if not volatile and ca.get("wr") is not None and cb.get("wr") is not None:
        wa, wb = wr_map(ca, argc), wr_map(cb, argc)
        for k in sorted(set(wa) | set(wb)):
            if k[1] >= win:
                continue
            if k in wa and k in wb:
                (la, da), (lb, db) = wa[k], wb[k]
                base = u(aa[k[0]]) if aa and k[0] < len(aa) and aa[k[0]] is not None else 0
                if da != db and not (la == lb and cmp_words(da, db, mapper)) and \
                        not runs_same_mapped(((base or 0) & 0xFFFFFFFF) + k[1], da, db, mapper):
                    out.append(("mem", "arg%d+0x%X: %s vs %s" % (k[0], k[1], da[:24], db[:24])))
            else:
                side = "port" if k in wa else "rpcs3"
                ln, d = (wa.get(k) or wb.get(k))
                out.append(("mem1", "arg%d+0x%X len %d written only by %s (%s)" % (k[0], k[1], ln, side, d[:24])))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("a", help="first trace (the port)")
    ap.add_argument("b", help="second trace (the reference, RPCS3)")
    ap.add_argument("--imports", help="EBOOT.imports.json from the lift: library names per NID")
    ap.add_argument("--only", help="compare only these functions/libraries")
    ap.add_argument("--ignore", help="drop these functions/libraries from both sides")
    ap.add_argument("--volatile", help="extra patterns whose results are not compared")
    ap.add_argument("--no-default-volatile", action="store_true")
    ap.add_argument("--pair", action="append", default=[], help="force a thread pair A_TID=B_TID")
    ap.add_argument("--no-collapse", action="store_true", help="do not collapse repeated identical calls")
    ap.add_argument("--window", type=int, default=256, help="resync lookahead (calls)")
    ap.add_argument("--addr-map", action="append", default=[], metavar="A=B[:SIZE]",
                    help="the first side's address region at A is the second side's at B (hex, SIZE "
                         "default 0x10000000), e.g. 0x40000000=0x30000000; repeatable")
    ap.add_argument("--rank", choices=("first", "count"), default="first",
                    help="order functions by kind of divergence, then by where it first occurs "
                         "(default), or by a score weighted by how many calls diverge")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--context", type=int, default=3)
    ap.add_argument("--baseline", help="JSON triage baseline of classified divergences (highlights NEW and resolved items)")
    ap.add_argument("--coverage", action="store_true", help="print per-library import coverage (requires --imports)")
    ap.add_argument("--fail-on-new", action="store_true", help="exit non-zero if unclassified host/value divergences exist")
    ap.add_argument("--json", help="write the full result here")
    args = ap.parse_args()

    ta, tb = Trace(args.a), Trace(args.b)
    libs = {}
    lib_imports = defaultdict(set)
    if args.imports:
        with open(args.imports) as f:
            imports = json.load(f)
        for e in imports:
            k = ("hle", int(e["nid"], 16))
            lib = e.get("library", "")
            libs[k] = lib
            lib_imports[lib].add(k)
    names = {}
    for k, v in syscall_names().items():
        names[("sc", k)] = v
    arity = {}
    kinds = {}
    for t in (tb, ta):            # the reference's names win
        for calls in t.calls.values():
            for c in calls:
                k = key_of(c)
                if c.get("name"):
                    names[k] = c["name"]
                if c.get("argc") is not None:
                    arity[k] = c["argc"]
                if c.get("args_text") and k not in kinds:
                    kinds[k] = arg_kinds(c["args_text"])
    only = Matcher(args.only, libs)
    ignore = Matcher(args.ignore, libs)
    vol = Matcher(",".join(([] if args.no_default_volatile else DEFAULT_VOLATILE) +
                           ([args.volatile] if args.volatile else [])), libs)
    win = min(ta.header.get("win", 256), tb.header.get("win", 256))

    def keep(c):
        k = key_of(c)
        n = names.get(k)
        if only and not only.match(k, n):
            return False
        return not (ignore and ignore.match(k, n))

    def nm(k):
        n = names.get(k)
        if n:
            return n
        return ("syscall %d" % k[1]) if k[0] == "sc" else "0x%08X" % k[1]

    keys_a = {key_of(c) for v in ta.calls.values() for c in v if keep(c)}
    keys_b = {key_of(c) for v in tb.calls.values() for c in v if keep(c)}
    # If either trace retired hot breakpoints after max_per_site hits, extra
    # calls on the other side after a saturated function's last recorded call
    # are expected (not a divergence).
    mps_a = ta.header.get("max_per_site") or 0
    mps_b = tb.header.get("max_per_site") or 0
    sat_a = set()
    sat_b = set()
    if mps_a > 0:
        cnt_a = defaultdict(int)
        for v in ta.calls.values():
            for c in v:
                cnt_a[key_of(c)] += 1
        sat_a = {k for k, n in cnt_a.items() if n >= mps_a}
    if mps_b > 0:
        cnt_b = defaultdict(int)
        for v in tb.calls.values():
            for c in v:
                cnt_b[key_of(c)] += 1
        sat_b = {k for k, n in cnt_b.items() if n >= mps_b}

    pairs, lone_a, lone_b = pair_threads(ta, tb, args.pair)
    mapper = Mapper(parse_addr_map(args.addr_map))
    fn = defaultdict(lambda: defaultdict(int))     # key -> counters
    examples = defaultdict(list)
    firsts = []
    never_b, never_a = defaultdict(int), defaultdict(int)
    firstpos = defaultdict(dict)       # key -> tier -> (position, "thread#seq")

    def note_first(k, tier, pos, where):
        cur = firstpos[k].get(tier)
        if cur is None or pos < cur[0]:
            firstpos[k][tier] = (pos, where)

    for tid_a, tid_b in pairs:
        A = [c for c in ta.calls[tid_a] if keep(c)]
        B = [c for c in tb.calls[tid_b] if keep(c)]
        tname = ta.thread_names.get(tid_a) or tb.thread_names.get(tid_b) or str(tid_a)
        for c in A:
            fn[key_of(c)]["calls_a"] += 1
            if key_of(c) not in keys_b:
                never_b[key_of(c)] += 1
                note_first(key_of(c), TIER["only"], c.get("_ln", 0), "%s#%d" % (tname, c["seq"]))
        for c in B:
            fn[key_of(c)]["calls_b"] += 1
            if key_of(c) not in keys_a:
                never_a[key_of(c)] += 1
        A = [c for c in A if key_of(c) in keys_b]
        B = [c for c in B if key_of(c) in keys_a]
        # If one side retired hot entry breakpoints (--max-per-site), drop the
        # other side's excess calls to those saturated functions (beyond the
        # count recorded on the retired side for this thread) BEFORE aligning,
        # so thousands of later per-frame polls don't blow past --window.
        if sat_b:
            lim_b = defaultdict(int)
            for c in B:
                lim_b[key_of(c)] += 1
            seen_a = defaultdict(int)
            A_kept = []
            for c in A:
                k = key_of(c)
                if k in sat_b:
                    seen_a[k] += 1
                    if seen_a[k] > lim_b[k]:
                        fn[k]["tail"] += 1
                        continue
                A_kept.append(c)
            A = A_kept
        if sat_a:
            lim_a = defaultdict(int)
            for c in A:
                lim_a[key_of(c)] += 1
            seen_b = defaultdict(int)
            B_kept = []
            for c in B:
                k = key_of(c)
                if k in sat_a:
                    seen_b[k] += 1
                    if seen_b[k] > lim_a[k]:
                        fn[k]["tail"] += 1
                        continue
                B_kept.append(c)
            B = B_kept
        if not args.no_collapse:
            A, B = collapse(A, arity), collapse(B, arity)
        # Last index of each saturated function on each side of this thread,
        # so calls on the other side after retirement are treated like tail.
        last_a = {}
        if sat_a:
            for idx, c in enumerate(A):
                if key_of(c) in sat_a:
                    last_a[key_of(c)] = idx
        last_b = {}
        if sat_b:
            for idx, c in enumerate(B):
                if key_of(c) in sat_b:
                    last_b[key_of(c)] = idx
        first = None
        first_soft = None
        hist = []
        pos = 0                 # where in the first side's trace (its line), for --rank first
        cur_i = cur_j = -1
        for op, i, j in align(A, B, args.window):
            if i is not None:
                cur_i = i
            if j is not None:
                cur_j = j
            if op == "pair":
                ca, cb = A[i], B[j]
                k = key_of(ca)
                pos = ca.get("_ln", pos)
                fn[k]["paired"] += 1
                diffs = compare(ca, cb, mapper, vol.match(k, names.get(k)), arity, win, kinds.get(k))
                for kind, d in diffs:
                    fn[k][kind] += 1
                    note_first(k, HOST_TIER if kind == "ret" and "upper 32" in d else TIER[kind], pos,
                               "%s#%d" % (tname, ca["seq"]))
                    if len(examples[(k, kind)]) < 3:
                        examples[(k, kind)].append(d)
                hard = [d for d in diffs if SEV[d[0]] >= 2]
                ev = (ca, cb, diffs)
                if hard and first is None:
                    first = (ev, list(hist))
                elif diffs and first_soft is None and first is None:
                    first_soft = (ev, list(hist))
                hist.append(("pair", ca, cb))
            elif op in ("tailA", "tailB"):
                fn[key_of(A[i] if op == "tailA" else B[j])]["tail"] += 1
                continue
            else:
                c = A[i] if op == "onlyA" else B[j]
                k = key_of(c)
                if (op == "onlyA" and k in sat_b and cur_j >= last_b.get(k, -1)) or \
                        (op == "onlyB" and k in sat_a and cur_i >= last_a.get(k, -1)):
                    fn[k]["tail"] += 1
                    continue
                fn[k]["only_a" if op == "onlyA" else "only_b"] += 1
                if op == "onlyA":
                    pos = c.get("_ln", pos)
                if not vol.match(k, names.get(k)):
                    note_first(k, TIER["only"], pos, "%s#%d %s" % (tname, c["seq"], "a" if op == "onlyA" else "b"))
                if vol.match(k, names.get(k)):
                    pass        # one poll more or less is timing, not a divergence
                elif first is None:
                    first = (((c, None) if op == "onlyA" else (None, c)) + ([("only", "called only by %s" % ("port" if op == "onlyA" else "rpcs3"))],), list(hist))
                hist.append((op, c))
            if len(hist) > args.context:
                hist.pop(0)
        firsts.append((tid_a, tid_b, first, first_soft, len(A), len(B)))

    sa, sb = ta.side, tb.side
    print("hle_diff: %s (%d calls, %d threads) vs %s (%d calls, %d threads)" %
          (sa, ta.ncalls(), len(ta.calls), sb, tb.ncalls(), len(tb.calls)))
    print("memory window compared: %d bytes; thread pairs: %d; unpaired: %s only %s, %s only %s\n" %
          (win, len(pairs), sa, [(t, ta.thread_names.get(t, "")) for t in lone_a],
           sb, [(t, tb.thread_names.get(t, "")) for t in lone_b]))

    def show(c):
        if c is None:
            return "-"
        k = key_of(c)
        rep = " x%d" % c["_rep"] if c.get("_rep") else ""
        return "#%d %s lr=%s ret=%s%s" % (c["seq"], nm(k), c.get("lr"), c.get("ret"), rep)

    print("== First divergence per thread ==")
    for tid_a, tid_b, first, first_soft, na, nb in firsts:
        tn = ta.thread_names.get(tid_a) or tb.thread_names.get(tid_b) or ""
        print("thread %s %d <-> %d (%s): %d vs %d calls aligned" % (sa, tid_a, tid_b, tn, na, nb))
        if first is None:
            print("   no divergence%s" % ("" if first_soft is None else
                  "; first soft: %s %s" % (show(first_soft[0][0]), first_soft[0][2])))
            continue
        (ca, cb, diffs), hist = first
        for h in hist:
            if h[0] == "pair":
                print("     ok   %s" % show(h[1]))
            else:
                print("     %s %s" % (h[0], show(h[1])))
        print("  >> %s: %s" % (sa, show(ca)))
        print("  >> %s: %s" % (sb, show(cb)))
        for kind, d in diffs:
            print("       [%s] %s" % (kind, d))

    for k in never_a:
        note_first(k, TIER["only"], float("inf"), "(second side only)")
    rows = []
    for k, cnt in fn.items():
        score = cnt["ret"] * 3 + cnt["mem"] * 3 + cnt["args"] * 2 + cnt["only_a"] + cnt["only_b"] + cnt["mem1"] * 0.5
        if k in never_a or k in never_b:
            score += 5
        fp = firstpos.get(k)
        tier = min(fp) if fp else None
        rows.append((score, k, cnt, tier, fp[tier] if fp else None))
    if args.rank == "first":
        # by the worst kind of divergence, then by where that kind first
        # happens: one wrong return early in the boot ranks above a soft
        # difference repeated a thousand times
        rows.sort(key=lambda r: (r[3] is None, r[3] if r[3] is not None else 9,
                                 r[4][0] if r[4] else 0, -r[0], nm(r[1])))
        title = "kind of divergence, then first occurrence"
    else:
        rows.sort(key=lambda r: (-r[0], nm(r[1])))
        title = "divergence score"
    print("\n== Functions by %s (top %d) ==" % (title, args.top))
    print("%-38s %-5s %-26s %7s %7s %7s %5s %5s %5s %5s %6s %6s" %
          ("function", "kind", "first at", "calls_a", "calls_b", "paired", "ret", "mem", "args", "mem1",
           "only_a", "only_b"))
    for score, k, cnt, tier, fp in rows[:args.top]:
        if score == 0 and tier is None:
            break
        lib = libs.get(k)
        label = nm(k) + (" [%s]" % lib if lib else "")
        print("%-38.38s %-5s %-26.26s %7d %7d %7d %5d %5d %5d %5d %6d %6d" %
              (label, TIER_NAME.get(tier, "-"), fp[1] if fp else "-", cnt["calls_a"], cnt["calls_b"],
               cnt["paired"], cnt["ret"], cnt["mem"], cnt["args"], cnt["mem1"], cnt["only_a"], cnt["only_b"]))
        for kind in ("ret", "mem", "args", "mem1"):
            for e in examples.get((k, kind), [])[:2]:
                print("      %-4s %s" % (kind, e))
    if never_a or never_b:
        print("\n== Called on one side only (never on the other) ==")
        for t in (ta, tb):
            if t.header.get("method") == "log":
                print("  (%s is an RPCS3.log trace: a function it lacks there may be unlogged, or run by\n"
                      "   an LLE firmware module, rather than not called; check with the GDB-stub trace)" % t.side)
        for k, n in sorted(never_b.items(), key=lambda x: -x[1]):
            print("  %-40s %6d calls, only in %s" % (nm(k), n, sa))
        for k, n in sorted(never_a.items(), key=lambda x: -x[1]):
            print("  %-40s %6d calls, only in %s" % (nm(k), n, sb))

    baseline_entries = {}
    if args.baseline:
        with open(args.baseline) as f:
            baseline_entries = json.load(f).get("entries", {})
    new_rows = []
    classified_rows = []
    seen_divergent_names = set()
    for score, k, cnt, tier, fp in rows:
        if score == 0 and tier is None:
            continue
        name = nm(k)
        kind_name = TIER_NAME.get(tier, "-")
        seen_divergent_names.add(name)
        be = baseline_entries.get(name)
        if be:
            allowed = be.get("kinds")
            if kind_name == "host" and (not allowed or "host" not in allowed):
                new_rows.append((score, k, cnt, tier, fp))
            elif allowed and kind_name not in allowed:
                new_rows.append((score, k, cnt, tier, fp))
            else:
                classified_rows.append((name, kind_name, be.get("class", ""), be.get("reason", "")))
        else:
            new_rows.append((score, k, cnt, tier, fp))
    resolved_baseline = [n for n in sorted(baseline_entries) if n not in seen_divergent_names]
    if args.baseline:
        print("\n== Triage vs baseline (%s) ==" % os.path.basename(args.baseline))
        print("  new/unclassified: %d  |  classified: %d  |  resolved/absent: %d" %
              (len(new_rows), len(classified_rows), len(resolved_baseline)))
        if new_rows:
            print("  NEW / UNCLASSIFIED:")
            for score, k, cnt, tier, fp in new_rows[:args.top]:
                lib = libs.get(k)
                label = nm(k) + (" [%s]" % lib if lib else "")
                print("    %-38.38s %-5s %-22.22s paired=%-4d ret=%-3d mem=%-3d args=%-3d mem1=%-3d" %
                      (label, TIER_NAME.get(tier, "-"), fp[1] if fp else "-",
                       cnt["paired"], cnt["ret"], cnt["mem"], cnt["args"], cnt["mem1"]))
        if resolved_baseline:
            print("  Resolved / no longer diverging: %s" % ", ".join(resolved_baseline[:20]))

    coverage_out = {}
    if args.coverage and lib_imports:
        print("\n== Library import coverage ==")
        print("%-20s %6s %6s %6s %6s %6s" % ("library", "total", "port", "ref", "paired", "div"))
        div_keys = {k for _s, k, _c, tier, _fp in rows if tier in (HOST_TIER, 0)}
        for lib in sorted(lib_imports):
            ks = lib_imports[lib]
            n_tot = len(ks)
            n_a = sum(1 for k in ks if k in keys_a)
            n_b = sum(1 for k in ks if k in keys_b)
            n_p = sum(1 for k in ks if fn[k]["paired"] > 0)
            n_d = sum(1 for k in ks if k in div_keys)
            coverage_out[lib] = {"total": n_tot, "port": n_a, "ref": n_b, "paired": n_p, "div": n_d}
            print("%-20.20s %6d %6d %6d %6d %6d" % (lib, n_tot, n_a, n_b, n_p, n_d))

    if args.json:
        res = {
            "a": args.a, "b": args.b, "win": win,
            "pairs": pairs, "unpaired_a": lone_a, "unpaired_b": lone_b,
            "first": [{"tid_a": t1, "tid_b": t2, "calls_a": na, "calls_b": nb,
                       "a": (f[0][0] if f else None), "b": (f[0][1] if f else None),
                       "diffs": (f[0][2] if f else None)}
                      for t1, t2, f, _, na, nb in firsts],
            "rank": args.rank,
            "addr_map": [["0x%X" % a, "0x%X" % b, "0x%X" % n] for a, b, n in mapper.regions],
            "functions": [{"key": list(k), "name": nm(k), "library": libs.get(k), "score": s,
                           "kind": TIER_NAME.get(tier), "first_at": fp[1] if fp else None,
                           **dict(cnt), "examples": {kind: examples.get((k, kind), [])
                                                     for kind in ("ret", "mem", "args", "mem1")}}
                          for s, k, cnt, tier, fp in rows],
            "never_in_b": {nm(k): n for k, n in never_b.items()},
            "never_in_a": {nm(k): n for k, n in never_a.items()},
            "new_unclassified": [nm(k) for _s, k, _c, _t, _fp in new_rows],
            "resolved_baseline": resolved_baseline,
            "coverage": coverage_out,
        }
        with open(args.json, "w") as f:
            json.dump(res, f, indent=1)
    if args.fail_on_new and any(tier in (HOST_TIER, 0) for _s, _k, _c, tier, _fp in new_rows):
        sys.exit(2)


if __name__ == "__main__":
    main()
