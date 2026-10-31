#!/usr/bin/env python3
"""Check PS3RECOMP_TRACE output: runs tests/test_frame_trace.c's binary.

    test_frame_trace.py <path to test_frame_trace>

Off, nothing is written. On, the file must load with json, carry the event
counts the binary reports with nothing dropped, name each synthetic thread,
nest every thread's spans (a child inside its parent, the parents the API
calls imply), pair every async begin with an end, and have reached the disk
before exit through the size-triggered flush.
"""
import collections
import decimal
import json
import os
import subprocess
import sys
import tempfile

PARENT = {
    "spu.run": "spu.job",
    "spu.mbox_wait": "spu.run",
    "spu.lockline_retry": "spu.job",
    "spu.job": None,
    "ppu.gcm_flip_poll": None,
}


def fail(msg):
    print("FAIL: " + msg)
    sys.exit(1)


def run(exe, mode, env):
    p = subprocess.run([exe, mode], env=env, capture_output=True, text=True, timeout=120)
    if p.returncode != 0:
        fail(f"{mode}: exit {p.returncode}\n{p.stdout}\n{p.stderr}")
    return p.stdout


def check_nesting(spans, tid):
    """spans: (ts, end, name) of one thread. Sorted by start, then the longer
    first, every span must end inside the one it starts in."""
    spans.sort(key=lambda s: (s[0], -s[1]))
    stack = []
    for ts, end, name in spans:
        while stack and stack[-1][1] <= ts:
            stack.pop()
        if stack and end > stack[-1][1]:
            fail(f"tid {tid}: {name} [{ts}, {end}] overlaps {stack[-1][2]} "
                 f"[{stack[-1][0]}, {stack[-1][1]}] without nesting")
        parent = stack[-1][2] if stack else None
        if name in PARENT and PARENT[name] != parent:
            fail(f"tid {tid}: {name} at {ts} is inside {parent}, expected {PARENT[name]}")
        stack.append((ts, end, name))


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    exe = sys.argv[1]
    base = {k: v for k, v in os.environ.items() if not k.startswith("PS3RECOMP_TRACE")}

    out = run(exe, "off", base)
    if "off ok" not in out:
        fail("off: " + out)

    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "trace.json")
        env = dict(base, PS3RECOMP_TRACE=path, PS3RECOMP_TRACE_FLUSH_MB="0.25")
        expect = json.loads(run(exe, "on", env).strip().splitlines()[-1])
        with open(path, encoding="utf-8") as f:
            events = json.load(f, parse_float=decimal.Decimal)   # exact: ts are 3-decimal us

    if not isinstance(events, list) or not events:
        fail("not a non-empty JSON array")
    if expect["midrun_bytes"] < 4096:
        fail(f"only {expect['midrun_bytes']} bytes on disk before exit: no size-triggered flush")

    proc = [e for e in events if e.get("ph") == "M" and e.get("name") == "process_name"]
    if len(proc) != 1 or events[-1] is not proc[0]:
        fail("process_name metadata missing or not last")
    if proc[0]["args"]["dropped"] != 0:
        fail(f"{proc[0]['args']['dropped']} events dropped")

    counts = collections.Counter()
    spans = collections.defaultdict(list)
    opened = {}
    names = {}
    records = 0
    for e in events:
        ph = e.get("ph")
        for key in ("name", "ph", "pid"):
            if key not in e:
                fail(f"event without {key}: {e}")
        if ph == "M":
            if e["name"] == "thread_name":
                names[e["tid"]] = e["args"]["name"]
            continue
        if not isinstance(e.get("tid"), int) or not isinstance(e.get("ts"), decimal.Decimal):
            fail(f"bad tid/ts: {e}")
        if e["ts"] < 0:
            fail(f"negative ts: {e}")
        if ph == "X":
            if not isinstance(e.get("dur"), decimal.Decimal) or e["dur"] < 0:
                fail(f"bad dur: {e}")
            spans[e["tid"]].append((e["ts"], e["ts"] + e["dur"], e["name"]))
        elif ph == "i":
            if e.get("s") != "t":
                fail(f"instant not thread-scoped: {e}")
        elif ph == "b":
            key = (e["tid"], e["name"], e["id"])
            if key in opened:
                fail(f"async begin twice before its end: {e}")
            opened[key] = e["ts"]
            continue          # counted at its end
        elif ph == "e":
            key = (e["tid"], e["name"], e["id"])
            if key not in opened:
                fail(f"async end without a begin: {e}")
            if e["ts"] < opened.pop(key):
                fail(f"async end before its begin: {e}")
        else:
            fail(f"unexpected phase {ph}: {e}")
        counts[e["name"]] += 1
        records += 1
    if opened:
        fail(f"async begins without an end: {sorted(opened)}")
    if records != proc[0]["args"]["events"]:
        fail(f"{records} records in the file, process metadata says {proc[0]['args']['events']}")

    for name, n in expect["counts"].items():
        if counts[name] != n:
            fail(f"{name}: {counts[name]} in the file, {n} recorded")
    extra = set(counts) - set(expect["counts"])
    if extra:
        fail(f"unexpected events: {sorted(extra)}")

    worker_tids = [t for t, n in names.items() if n.startswith("synthetic ")]
    if sorted(names[t] for t in worker_tids) != [f"synthetic {i}" for i in range(expect["threads"])]:
        fail(f"thread names: {names}")
    for tid in worker_tids:
        if not spans[tid]:
            fail(f"thread {names[tid]} has no spans")
    for tid, s in spans.items():
        if tid not in worker_tids:
            fail(f"spans on unnamed tid {tid}")
        check_nesting(s, tid)

    print(f"ok: {records} records, {len(worker_tids)} threads, "
          f"{expect['midrun_bytes']} bytes flushed before exit")
    return 0


if __name__ == "__main__":
    sys.exit(main())
