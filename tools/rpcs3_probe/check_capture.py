#!/usr/bin/env python3
"""check_capture.py - completeness of a GDB-stub capture (rpcs3_calltrace.py)
against the RPCS3.log of the same run (rpcs3_log2calltrace.py).

One RPCS3 run, two independent views of it: the GDB capture stops at every
traced call, the log has a line for every syscall RPCS3 finished (r3 included)
and for every HLE call RPCS3 logs. Thread ids are the same in both, so per
thread the log's calls that the capture should have traced (its filter is in
its header) must appear in it, in the same order, from the same call site,
with the same syscall result. What does not is a record the capture lost.

  python check_capture.py rpcs3_gdb.jsonl rpcs3_log.jsonl --imports EBOOT.imports.json
         [--json out.json] [--show 20]

Run RPCS3 with the log channels at Trace (PPU for the syscall lines) while the
capture runs (--log merges thread names, it does not need Trace), then convert
that log with rpcs3_log2calltrace.py --elf <the title's ELF>.

A log trace ends a little after the capture (RPCS3 runs until it is killed):
log calls after a thread's last matched call are reported as "after the
capture", not as lost.

Python 3.9+, standard library only. Title-agnostic.
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
from calltrace import Trace, key_of, u  # noqa: E402
from hle_diff import align  # noqa: E402
import rpcs3_calltrace  # noqa: E402


def traced_by(header, imports):
    """Predicate: would the capture with this header have traced the log call c?"""
    wanted = rpcs3_calltrace.make_filter(header.get("filter", ""), header.get("default_exclude", True))
    libs = {int(e["nid"], 16): e.get("library") for e in imports}

    def ok(c):
        if c["kind"] == "sc":
            return wanted("", "", sc=int(c["sc"]))
        nid = int(c["nid"], 16)
        return nid in libs and wanted(c.get("name") or "", libs[nid], nid=nid)
    return ok


def where(c):
    """The call site: the sc address for a syscall, the caller's LR for an
    import. RPCS3's log gives a finished syscall at the address after the sc
    (the PC has moved on), so a log record's site is moved back by 4."""
    if c["kind"] != "sc":
        return u(c.get("lr"))
    site = u(c.get("site"))
    return site - 4 if site is not None and c.get("src") == "rpcs3-log" else site


def check(gdb, log, ok):
    res = {"threads": {}, "lost": [], "ret_mismatch": [], "site_mismatch": [], "extra": [], "open": [],
           "internal": []}
    for tid in sorted(set(gdb.calls) | set(log.calls)):
        G = gdb.calls.get(tid, [])
        L = [c for c in log.calls.get(tid, []) if ok(c)]
        st = {"name": gdb.thread_names.get(tid) or log.thread_names.get(tid, ""), "gdb": len(G), "log": len(L),
              "matched": 0, "lost": 0, "internal": 0, "extra": 0, "after": 0,
              "open": sum(1 for c in G if c.get("open"))}
        ops = list(align(L, G, window=64, key=lambda c: (key_of(c), where(c))))
        last = max([i for op, i, _j in ops if op == "pair"], default=-1)
        prev = None                 # the last log call the capture has
        for op, i, j in ops:
            if op == "pair":
                lc, gc = L[i], G[j]
                prev = lc
                st["matched"] += 1
                if where(lc) is not None and where(gc) is not None and where(lc) != where(gc):
                    res["site_mismatch"].append((tid, lc, gc))
                if lc.get("ret") is not None and gc.get("ret") is not None and not gc.get("open") \
                        and (u(lc["ret"]) & 0xFFFFFFFF) != (u(gc["ret"]) & 0xFFFFFFFF):
                    res["ret_mismatch"].append((tid, lc, gc))
                if gc.get("open"):
                    res["open"].append((tid, gc))
            elif op in ("onlyA", "tailA"):
                if i > last:
                    st["after"] += 1           # past the capture's end on this thread
                elif prev is not None and (key_of(prev), where(prev)) == (key_of(L[i]), where(L[i])):
                    # the same function at the same call site as the call just
                    # matched: a call RPCS3's HLE made and logged inside that
                    # one (cellSysmoduleLoadModule logs the dependencies it
                    # loads with its caller's LR). A lost repeat of a call
                    # from one site would look the same: see "internal".
                    st["internal"] += 1
                    res["internal"].append((tid, L[i]))
                else:
                    st["lost"] += 1
                    res["lost"].append((tid, L[i]))
            else:
                gc = G[j]
                st["extra"] += 1
                res["extra"].append((tid, gc))
        res["threads"][tid] = st
    return res


def name(c):
    if c["kind"] == "sc":
        return "%s(sc %d)" % (c.get("name") or "syscall", c["sc"])
    return c.get("name") or c.get("nid")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("gdb", help="rpcs3_calltrace.py output")
    ap.add_argument("log", help="rpcs3_log2calltrace.py output of the same run's RPCS3.log")
    ap.add_argument("--imports", required=True, help="EBOOT.imports.json")
    ap.add_argument("--json")
    ap.add_argument("--show", type=int, default=20)
    args = ap.parse_args()
    with open(args.imports) as f:
        imports = json.load(f)
    gdb, log = Trace(args.gdb), Trace(args.log)
    if gdb.header.get("method") != "gdb" or log.header.get("method") != "log":
        print("warning: expected a GDB capture first and a log trace second")
    res = check(gdb, log, traced_by(gdb.header, imports))

    print("%-34s %6s %6s %7s %5s %8s %5s %6s %5s" % ("thread", "gdb", "log", "matched", "lost", "internal", "extra",
                                                       "after", "open"))
    tot = {"gdb": 0, "log": 0, "matched": 0, "lost": 0, "internal": 0, "extra": 0, "after": 0, "open": 0}
    unreached = 0
    for tid, st in res["threads"].items():
        for k in tot:
            tot[k] += st[k]
        if not st["gdb"]:
            unreached += 1        # created after the capture ended, or never stopped at a traced call
            continue
        print("0x%-8X %-23.23s %6d %6d %7d %5d %8d %5d %6d %5d" % (
            tid, st["name"], st["gdb"], st["log"], st["matched"], st["lost"], st["internal"], st["extra"], st["after"],
            st["open"]))
    if unreached:
        print("%-34s (%d threads with log calls only, counted in 'after')" % ("...", unreached))
    print("%-34s %6d %6d %7d %5d %8d %5d %6d %5d" % ("total", tot["gdb"], tot["log"], tot["matched"], tot["lost"],
                                                      tot["internal"], tot["extra"], tot["after"], tot["open"]))
    print("returns that differ: %d, call sites that differ: %d" % (len(res["ret_mismatch"]), len(res["site_mismatch"])))
    for title, lst in (("lost (in the log, not in the capture)", res["lost"]),
                       ("internal (logged by RPCS3 inside the call before, same site)", res["internal"]),
                       ("returns that differ", res["ret_mismatch"]),
                       ("call sites that differ", res["site_mismatch"]),
                       ("open in the capture where the log has the call", res["open"])):
        if lst:
            print("\n%s:" % title)
            for item in lst[:args.show]:
                tid, c = item[0], item[1]
                more = (" ret %s vs %s" % (c.get("ret"), item[2].get("ret"))) if len(item) > 2 else ""
                w = where(c)
                print("  0x%X #%d %s at %s%s" % (tid, c["seq"], name(c), "0x%08X" % w if w is not None else "?", more))
    if args.json:
        with open(args.json, "w") as f:
            json.dump({"totals": tot, "threads": {"0x%X" % t: s for t, s in res["threads"].items()},
                       "lost": [[t, c] for t, c in res["lost"]],
                       "internal": [[t, c] for t, c in res["internal"]],
                       "ret_mismatch": [[t, a, b] for t, a, b in res["ret_mismatch"]],
                       "site_mismatch": [[t, a, b] for t, a, b in res["site_mismatch"]]}, f, indent=1)
    sys.exit(1 if res["lost"] or res["ret_mismatch"] else 0)


if __name__ == "__main__":
    main()
