#!/usr/bin/env python3
"""rpcs3_log2calltrace.py - RPCS3.log -> ps3-calltrace/1 (tools/calltrace.py).

The cheap reference trace: RPCS3 runs at full speed (LLVM) with its HLE and
lv2 log channels at Trace, and this converts the log. What the log can give,
and what it cannot (see docs/HLE_TRACE_DIFF.md for the full table):

  HLE import    thread, order, name -> NID, LR (the caller's return address),
                arguments as RPCS3 formats them (count = the function's
                arity, values where they parse as numbers). Return value only
                when RPCS3 reports a failure ("'f' failed with 0x8...", and
                only the first 3 of each distinct message per thread). No
                written memory. A function RPCS3 runs without logging is
                missing; one served by an LLE firmware module is not an HLE
                call at all.
  syscall       thread, order, number, name, r3 on return ("Syscall 'x' (n)
                finished" at PPU Trace), arguments when the lv2 module logs
                its entry line. No written memory.

Calls made by firmware code rather than by the title (LLE modules, whose
caller address is outside the title's ELF) are dropped unless --keep-prx.

  python rpcs3_log2calltrace.py RPCS3.log --imports EBOOT.imports.json
         [--elf EBOOT.elf] -o rpcs3_log.jsonl [--arity-out arity.json]

Python 3.9+, standard library only. Title-agnostic.
"""
import argparse
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import calltrace  # noqa: E402

LINE = re.compile(r"^\S*\s*(\d+:\d{2}:\d{2}\.\d+)\s+\{(.+?)\}\s+([^:{}]+?):\s(.*)$")
THREAD = re.compile(r"^PPU\[0x([0-9a-fA-F]+)\] Thread \((.*)\) \[(.*)\]$")
WHERE_HLE = re.compile(r"HLE:0x([0-9a-fA-F]+), LR:0x([0-9a-fA-F]+)")
WHERE_ADDR = re.compile(r"(?:(.+): )?0x([0-9a-fA-F]+)$")
CALL = re.compile(r"^(\w+)\((.*)\)\s*(?:->\s*(\S+))?\s*$")   # some lines end "-> <result>"
FINISHED = re.compile(r"Syscall '(\w+)' \((\d+)\) finished, r3=0x([0-9a-fA-F]+)")
FAILED = re.compile(r"'(\w+)' failed with 0x([0-9a-fA-F]+)")
CREATED = re.compile(r'Thread "(.*)" created \(id=0x([0-9a-fA-F]+)')   # names threads the firmware creates
NUM = re.compile(r"^\*?(-?0x[0-9a-fA-F]+|-?\d+)\b")


def match_call(msg):
    """'name(args)' or 'name(args) -> result', with the parentheses balanced:
    'f(): Allocated 0x30000000 (size=0x100000)' is a note, not a call."""
    c = CALL.match(msg)
    if not c:
        return None
    depth, q = 0, False
    body = c.group(2)
    for ch in body:
        if ch == '"':
            q = not q
        elif not q and ch == "(":
            depth += 1
        elif not q and ch == ")":
            depth -= 1
            if depth < 0:
                return None
    return c if depth == 0 else None


def split_args(s):
    """'a=1, b=*0x10, c="x, y"' -> ['a=1', 'b=*0x10', 'c="x, y"'] (top-level commas)."""
    out, depth, cur, q = [], 0, [], False
    for ch in s:
        if ch == '"':
            q = not q
        elif not q and ch in "([{":
            depth += 1
        elif not q and ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0 and not q:
            out.append("".join(cur).strip())
            cur = []
            continue
        cur.append(ch)
    if "".join(cur).strip():
        out.append("".join(cur).strip())
    return out


def arg_value(text):
    v = text.split("=", 1)[1].strip() if "=" in text else text.strip()
    m = NUM.match(v)
    if not m:
        return None
    n = int(m.group(1), 0)
    return n & 0xFFFFFFFFFFFFFFFF


def elf_text_ranges(path):
    data = open(path, "rb").read(0x10000)
    phoff, = struct.unpack(">Q", data[0x20:0x28])
    phentsize, phnum = struct.unpack(">HH", data[0x36:0x3A])
    out = []
    for i in range(phnum):
        p = data[phoff + i * phentsize: phoff + (i + 1) * phentsize]
        ptype, flags, _off, vaddr, _pa, _fs, memsz = struct.unpack(">IIQQQQQ", p[:48])
        if ptype == 1 and flags & 1:
            out.append((vaddr, vaddr + memsz))
    return out


def parse_lines(path):
    """Yield (tid, tname, where, channel, msg) for PPU-thread lines."""
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = LINE.match(line.rstrip("\r\n"))
            if not m:
                continue
            t = THREAD.match(m.group(2))
            if not t:
                continue
            yield int(t.group(1), 16), t.group(2), t.group(3), m.group(3).replace(" TODO", "").strip(), m.group(4)


def scan_threads_and_arity(path):
    threads, arity = {}, {}
    for tid, tname, _where, _ch, msg in parse_lines(path):
        threads.setdefault(tid, tname)
        cr = CREATED.search(msg)
        if cr:      # a thread that never logs a line of its own is still named
            threads.setdefault(int(cr.group(2), 16), cr.group(1))
        c = match_call(msg)
        if c:
            arity.setdefault(c.group(1), len(split_args(c.group(2))))
    return threads, arity


def convert(log, imports, text_ranges, keep_prx, out):
    import_nids = {int(e["nid"], 16) for e in imports}
    threads = {}
    arity = {}
    seq = {}
    open_hle = {}      # tid -> last HLE record not yet written
    sc_entry = {}      # tid -> (name, args_text)
    n_hle = n_sc = n_drop = 0

    def in_game(addr):
        return not text_ranges or any(a <= addr < b for a, b in text_ranges)

    def emit(rec):
        out.write(json.dumps(rec) + "\n")

    def flush(tid):
        rec = open_hle.pop(tid, None)
        if rec:
            emit(rec)

    for tid, tname, where, _ch, msg in parse_lines(log):
        if tid not in threads:
            threads[tid] = tname
            emit({"type": "thread", "tid": tid, "name": tname})
        fm = FINISHED.search(msg)
        if fm:
            name, num, r3 = fm.group(1), int(fm.group(2)), int(fm.group(3), 16)
            w = WHERE_ADDR.search(where)
            cia = int(w.group(2), 16) if w else 0
            from_prx = bool(w and w.group(1)) or not in_game(cia)
            ent = sc_entry.pop(tid, None)
            if from_prx and not keep_prx:
                n_drop += 1
                continue
            flush(tid)
            # "lr" for a syscall is the LR register (what the port records); the
            # log has only the sc site.
            rec = {"type": "call", "tid": tid, "seq": seq.get(tid, 0), "depth": 0, "kind": "sc",
                   "sc": num, "name": name, "lr": None, "site": "0x%08X" % cia,
                   "args": None, "ret": "0x%X" % r3, "wr": None, "src": "rpcs3-log"}
            if ent and ent[0] == name:
                parts = split_args(ent[1])
                rec["argc"] = len(parts)
                rec["args"] = ["0x%X" % v if v is not None else None for v in map(arg_value, parts)]
                rec["args_text"] = ent[1]
                arity.setdefault("sc:%d" % num, len(parts))
            seq[tid] = rec["seq"] + 1
            emit(rec)
            n_sc += 1
            continue
        fl = FAILED.search(msg)
        if fl:
            rec = open_hle.get(tid)
            if rec and rec["name"] == fl.group(1):
                rec["ret"] = "0x%X" % int(fl.group(2), 16)
            continue
        c = match_call(msg)
        if not c:
            continue
        name, argtext = c.group(1), c.group(2)
        h = WHERE_HLE.search(where)
        if h:
            nid = calltrace.compute_nid(name)
            if nid not in import_nids:
                continue          # a helper's log line, or a call RPCS3 makes itself
            lr = int(h.group(2), 16)
            if not in_game(lr) and not keep_prx:
                n_drop += 1
                continue
            flush(tid)
            parts = split_args(argtext)
            rec = {"type": "call", "tid": tid, "seq": seq.get(tid, 0), "depth": 0, "kind": "hle",
                   "nid": "0x%08X" % nid, "name": name, "lr": "0x%08X" % lr, "argc": len(parts),
                   "args": ["0x%X" % v if v is not None else None for v in map(arg_value, parts)],
                   "args_text": argtext, "ret": None, "wr": None, "src": "rpcs3-log"}
            if c.group(3) is not None and arg_value(c.group(3)) is not None:
                rec["ret"] = "0x%X" % arg_value(c.group(3))
            arity.setdefault(name, len(parts))
            seq[tid] = rec["seq"] + 1
            open_hle[tid] = rec
            n_hle += 1
        else:
            sc_entry[tid] = (name, argtext)   # an lv2 module's entry line; its "finished" follows
    for tid in list(open_hle):
        flush(tid)
    return n_hle, n_sc, n_drop, arity


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("--imports", required=True, help="EBOOT.imports.json: the title's imported NIDs")
    ap.add_argument("--elf", help="the title's ELF: calls from outside its code are firmware's own")
    ap.add_argument("--keep-prx", action="store_true", help="keep calls made by LLE firmware modules")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--arity-out", help="write {function: argument count} as seen in the log")
    args = ap.parse_args()
    with open(args.imports) as f:
        imports = json.load(f)
    ranges = elf_text_ranges(args.elf) if args.elf else []
    with open(args.out, "w") as out:
        out.write(json.dumps({"type": "header", "schema": calltrace.SCHEMA, "side": "rpcs3",
                              "method": "log", "win": 0, "log": os.path.basename(args.log)}) + "\n")
        n_hle, n_sc, n_drop, arity = convert(args.log, imports, ranges, args.keep_prx, out)
    print("[log2calltrace] %d HLE calls, %d syscalls, %d firmware-internal calls dropped -> %s" %
          (n_hle, n_sc, n_drop, args.out))
    if args.arity_out:
        with open(args.arity_out, "w") as f:
            json.dump(arity, f, indent=1, sort_keys=True)


if __name__ == "__main__":
    main()
