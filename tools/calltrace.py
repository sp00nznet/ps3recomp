"""calltrace.py - the shared firmware call trace format (ps3-calltrace/1).

One JSON object per line. Both sides of a differential trace write it: the
recompiled title (runtime/ppu/ppu_calltrace.h, PS3_CALLTRACE=<file>) and RPCS3
(tools/rpcs3_probe/rpcs3_calltrace.py over the GDB stub, or
tools/rpcs3_probe/rpcs3_log2calltrace.py from RPCS3.log). tools/hle_diff.py
compares two of them. Full description: docs/HLE_TRACE_DIFF.md.

Records:
  {"type":"header","schema":"ps3-calltrace/1","side":<str>,"win":<bytes>, ...}
  {"type":"thread","tid":<int>,"name":<str>}
  {"type":"call","tid":<int>,"seq":<int>,"depth":<int>,
   "kind":"hle"|"sc", "nid":"0x%08X" (hle) | "sc":<int> (sc), "name":<str>,
   "lr":"0x%08X", "args":["0x..."]*8 | null, "argc":<int>?, "args_text":<str>?,
   "ret":"0x..." | null, "wr":[[arg, off, len, "hex" | "#fnv1a64"], ...] | null,
   "t_us":<int>?, "frame":<int>?, "impl":<str>?}

A field that a capture method cannot give is null or absent, never invented:
the diff compares only what both sides recorded.

Python 3.9+, standard library only.
"""
import hashlib
import json
import os
import re
import struct

SCHEMA = "ps3-calltrace/1"

NID_SUFFIX = bytes([0x67, 0x59, 0x65, 0x99, 0x04, 0x25, 0x04, 0x90,
                    0x56, 0x64, 0x27, 0x49, 0x94, 0x89, 0x74, 0x1A])


def compute_nid(name):
    """PS3 NID of a function name (SHA-1 of name + suffix, first 4 bytes LE)."""
    return struct.unpack("<I", hashlib.sha1(name.encode() + NID_SUFFIX).digest()[:4])[0]


def fnv1a64(data):
    h = 0xCBF29CE484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def written_runs(pre, post, merge_gap=8, max_hex=32):
    """The runs of 4-byte words whose bytes differ between two equal-length
    windows, in the form the C side writes: [(off, len, "hex" | "#fnv")].
    Word-aligning runs keeps 32-bit ids/handles/addresses intact even when
    only some bytes differed from zero. Runs closer than merge_gap unchanged
    bytes are merged."""
    n = min(len(pre), len(post))
    out = []
    k = 0
    while k < n:
        ke = min(k + 4, n)
        if pre[k:ke] == post[k:ke]:
            k = ke
            continue
        s, e, gap = k, ke, 0
        j = ke
        while j < n:
            je = min(j + 4, n)
            if pre[j:je] != post[j:je]:
                e, gap = je, 0
            else:
                gap += je - j
                if gap >= merge_gap:
                    break
            j = je
        chunk = bytes(post[s:e])
        data = chunk.hex() if e - s <= max_hex else "#%016x" % fnv1a64(chunk)
        out.append((s, e - s, data))
        k = e
    return out


def syscall_names(toolkit_root=None):
    """Syscall number -> name, from runtime/syscalls/lv2_syscall_table.h."""
    root = toolkit_root or os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    path = os.path.join(root, "runtime", "syscalls", "lv2_syscall_table.h")
    names = {}
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = re.match(r"#define\s+SYS_(\w+)\s+(\d+)\b", line)
                if m and int(m.group(2)) not in names:
                    names[int(m.group(2))] = "sys_" + m.group(1).lower()
    except OSError:
        pass
    return names


class Trace:
    """A loaded trace: header, thread names, and calls per thread in seq order."""

    def __init__(self, path):
        self.path = path
        self.header = {}
        self.thread_names = {}
        self.calls = {}          # tid -> [call dict]
        self.first_seen = {}     # tid -> line index of its first call
        with open(path, encoding="utf-8", errors="replace") as f:
            for i, line in enumerate(f):
                line = line.strip()
                if not line:
                    continue
                try:
                    r = json.loads(line)
                except ValueError:
                    continue      # a torn last line from a killed process
                t = r.get("type")
                if t == "call":
                    tid = r["tid"]
                    r["_ln"] = i          # the order the side wrote it in, across threads
                    self.calls.setdefault(tid, []).append(r)
                    self.first_seen.setdefault(tid, i)
                elif t == "thread":
                    self.thread_names[r["tid"]] = r.get("name") or ""
                elif t == "header":
                    self.header = r
        for v in self.calls.values():
            v.sort(key=lambda c: c["seq"])

    @property
    def side(self):
        return self.header.get("side", os.path.basename(self.path))

    def ncalls(self):
        return sum(len(v) for v in self.calls.values())


def key_of(call):
    """What makes two calls 'the same function': the NID or syscall number."""
    if call["kind"] == "sc":
        return ("sc", int(call["sc"]))
    return ("hle", int(call["nid"], 16))


def u(v):
    """A record's hex string (or None) as an int (or None)."""
    if v is None:
        return None
    return int(v, 16) if isinstance(v, str) else int(v)
