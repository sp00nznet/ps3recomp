"""Tests for tools/hle_diff.py and tools/calltrace.py (synthetic traces)."""
import contextlib
import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import calltrace  # noqa: E402
import hle_diff  # noqa: E402


def call(tid, seq, name, ret, args=None, wr=None, sc=None, argc=None):
    r = {"type": "call", "tid": tid, "seq": seq, "depth": 0, "lr": "0x00010000",
         "args": [hex(a) for a in (args or [0] * 8)], "ret": None if ret is None else hex(ret),
         "wr": wr if wr is not None else []}
    if sc is None:
        r.update(kind="hle", nid="0x%08X" % calltrace.compute_nid(name), name=name)
    else:
        r.update(kind="sc", sc=sc, name=name)
    if argc is not None:
        r["argc"] = argc
    return r


def write(path, side, threads, calls):
    with open(path, "w") as f:
        f.write(json.dumps({"type": "header", "schema": calltrace.SCHEMA, "side": side, "win": 256}) + "\n")
        for tid, name in threads.items():
            f.write(json.dumps({"type": "thread", "tid": tid, "name": name}) + "\n")
        for c in calls:
            f.write(json.dumps(c) + "\n")


class CallTraceTest(unittest.TestCase):
    def test_nid(self):
        # Known NID: cellSysutilCheckCallback = 0x189A74DA
        self.assertEqual(calltrace.compute_nid("cellSysutilCheckCallback"), 0x189A74DA)

    def test_written_runs_merge(self):
        pre = bytes(64)
        post = bytearray(64)
        post[0] = 1
        post[5] = 2          # adjacent 4-byte words [0..4) and [4..8): merged
        post[12] = 4         # 4-byte gap [8..12) < 8: merged
        post[40] = 3         # far: its own 4-byte aligned run [40..44)
        self.assertEqual(calltrace.written_runs(pre, bytes(post)),
                         [(0, 16, "01000000000200000000000004000000"), (40, 4, "03000000")])

    def test_written_runs_word_aligned_ids(self):
        # 64-bit thread id written into zeroed memory: port writes byte +7 (0x02),
        # RPCS3 writes bytes +4..+7 (0x01000001). Word-aligned runs align at +4 len 4
        # so hle_diff's cmp_words binds 2 <-> 0x01000001 cleanly.
        pre = bytes(16)
        post_a = bytearray(16)
        post_a[7] = 2
        post_b = bytearray(16)
        post_b[4:8] = b"\x01\x00\x00\x01"
        ra = calltrace.written_runs(pre, bytes(post_a))
        rb = calltrace.written_runs(pre, bytes(post_b))
        self.assertEqual(ra, [(4, 4, "00000002")])
        self.assertEqual(rb, [(4, 4, "01000001")])
        m = hle_diff.Mapper()
        self.assertTrue(hle_diff.cmp_words(ra[0][2], rb[0][2], m))
        self.assertEqual(m.same(2, 0x01000001), "map")

    def test_written_runs_hash(self):
        pre = bytes(64)
        post = bytes(range(1, 65))
        runs = calltrace.written_runs(pre, post)
        self.assertEqual(len(runs), 1)
        self.assertTrue(runs[0][2].startswith("#"))
        self.assertEqual(runs[0][2], "#%016x" % calltrace.fnv1a64(post))


class DiffTest(unittest.TestCase):
    def run_diff(self, a_calls, b_calls, extra=()):
        d = tempfile.mkdtemp()
        pa, pb, pj = (os.path.join(d, n) for n in ("a.jsonl", "b.jsonl", "r.json"))
        write(pa, "port", {1: "main"}, a_calls)
        write(pb, "rpcs3", {0x1000000: "main_thread"}, b_calls)
        out = subprocess.run([sys.executable, os.path.join(HERE, "hle_diff.py"), pa, pb, "--json", pj] + list(extra),
                             capture_output=True, text=True, check=True).stdout
        with open(pj) as f:
            return out, json.load(f)

    def test_identical(self):
        calls = [call(1, i, "cellFoo%d" % (i % 3), 0) for i in range(10)]
        b = [dict(c, tid=0x1000000) for c in calls]
        out, r = self.run_diff(calls, b)
        self.assertIsNone(r["first"][0]["diffs"])
        self.assertIn("no divergence", out)

    def test_handle_mapping_and_ret_mismatch(self):
        # create returns a handle through memory: 3 here, 0x85000100 there -> mapped
        a = [call(1, 0, "sys_mutex_create", 0, args=[0x100, 0x200] + [0] * 6, wr=[[0, 0, 4, "00000003"]], argc=2),
             call(1, 1, "sys_mutex_lock", 0, args=[3, 0] + [0] * 6, argc=2),
             call(1, 2, "cellFsOpen", 0x80010006, argc=5)]
        b = [call(0x1000000, 0, "sys_mutex_create", 0, args=[0x100, 0x200] + [0] * 6, wr=[[0, 0, 4, "85000100"]], argc=2),
             call(0x1000000, 1, "sys_mutex_lock", 0, args=[0x85000100, 0] + [0] * 6, argc=2),
             call(0x1000000, 2, "cellFsOpen", 0, argc=5)]
        out, r = self.run_diff(a, b)
        f = r["first"][0]
        self.assertEqual(f["a"]["name"], "cellFsOpen")
        self.assertEqual(f["diffs"][0][0], "ret")

    def test_resync_over_extra_call(self):
        a = [call(1, 0, "cellA", 0), call(1, 1, "cellExtra", 0), call(1, 2, "cellB", 0), call(1, 3, "cellC", 0)]
        b = [call(0x1000000, 0, "cellA", 0), call(0x1000000, 1, "cellB", 0), call(0x1000000, 2, "cellC", 0),
             call(0x1000000, 3, "cellExtra", 0)]
        out, r = self.run_diff(a, b)
        fns = {f["name"]: f for f in r["functions"]}
        self.assertEqual(fns["cellB"]["paired"], 1)
        self.assertEqual(fns["cellC"]["paired"], 1)
        # one extra call mid-stream (only), the other past the end of the shorter side (tail)
        self.assertEqual(fns["cellExtra"].get("only_a", 0), 1)
        self.assertEqual(fns["cellExtra"].get("tail", 0), 1)

    def test_collapse_polls(self):
        a = [call(1, 0, "cellA", 0)] + [call(1, 1 + i, "cellPoll", 1) for i in range(3)] + [call(1, 9, "cellB", 0)]
        b = [call(0x1000000, 0, "cellA", 0)] + [call(0x1000000, 1 + i, "cellPoll", 1) for i in range(40)] + \
            [call(0x1000000, 50, "cellB", 0)]
        out, r = self.run_diff(a, b)
        self.assertIsNone(r["first"][0]["diffs"])

    def test_never_called(self):
        a = [call(1, 0, "cellA", 0), call(1, 1, "cellOnlyPort", 0), call(1, 2, "cellB", 0)]
        b = [call(0x1000000, 0, "cellA", 0), call(0x1000000, 1, "cellB", 0)]
        out, r = self.run_diff(a, b)
        self.assertIn("cellOnlyPort", r["never_in_b"])
        self.assertIsNone(r["first"][0]["diffs"])

    def test_mem_mismatch(self):
        a = [call(1, 0, "cellGetInfo", 0, args=[0x1000] + [0] * 7, wr=[[0, 4, 4, "00000280"]], argc=1)]
        b = [call(0x1000000, 0, "cellGetInfo", 0, args=[0x1000] + [0] * 7, wr=[[0, 4, 4, "00000500"]], argc=1)]
        out, r = self.run_diff(a, b)
        self.assertEqual(r["first"][0]["diffs"][0][0], "mem")

    def test_quantity_return_not_mapped(self):
        # 0x400 vs 0x3C000 would pass as an id mapping; a size never maps
        a = [call(1, 0, "cellGcmGetDefaultCommandWordSize", 0x400)]
        b = [call(0x1000000, 0, "cellGcmGetDefaultCommandWordSize", 0x3C000)]
        out, r = self.run_diff(a, b)
        self.assertEqual(r["first"][0]["diffs"][0][0], "ret")

    def test_host_pointer_return(self):
        # a 64-bit return (a host pointer) against a guest address: flagged,
        # though the low 32 bits alone would pass as a mapped address
        a = [call(1, 0, "cellGcmGetReportDataAddressLocation", 0x7FF696A14B00)]
        b = [call(0x1000000, 0, "cellGcmGetReportDataAddressLocation", 0x32500000)]
        out, r = self.run_diff(a, b)
        self.assertIn("upper 32 bits", r["first"][0]["diffs"][0][1])
        # an s32 error sign-extended on one side only is not
        a = [call(1, 0, "cellFsOpen", 0xFFFFFFFF80010006)]
        b = [call(0x1000000, 0, "cellFsOpen", 0x80010006)]
        out, r = self.run_diff(a, b)
        self.assertFalse(r["first"][0].get("diffs"))

    def test_rank_by_kind_then_first(self):
        # a soft difference repeated 40 times early, a wrong return once
        # later, a host pointer once at the end: by default the host pointer
        # ranks first, then the return, then the soft one; --rank count
        # puts the frequent one first
        a, b = [], []
        for i in range(40):
            a.append(call(1, i, "cellFrequent", 0, args=[0x1000 + i * 16] + [0] * 7, wr=[[0, 0, 1, "01"]], argc=1))
            b.append(call(0x1000000, i, "cellFrequent", 0, args=[0x1000 + i * 16] + [0] * 7, wr=[], argc=1))
        a.append(call(1, 40, "cellOnce", 1))
        b.append(call(0x1000000, 40, "cellOnce", 2))
        a.append(call(1, 41, "cellGetPointer", 0x7FF600001000))
        b.append(call(0x1000000, 41, "cellGetPointer", 0x30001000))
        out, r = self.run_diff(a, b)
        order = [f["name"] for f in r["functions"] if f.get("kind")]
        self.assertEqual(order, ["cellGetPointer", "cellOnce", "cellFrequent"])
        self.assertEqual([f["kind"] for f in r["functions"][:3]], ["host", "value", "mem1"])
        self.assertEqual(r["functions"][0]["first_at"], "main#41")
        out, r = self.run_diff(a, b, ["--rank", "count"])
        self.assertEqual(r["functions"][0]["name"], "cellFrequent")

    def test_addr_map(self):
        # user memory at 0x4xxxxxxx on the port, 0x3xxxxxxx in RPCS3: an address
        # written as a partial word (only its top byte changed), an address
        # argument, an address return
        a = [call(1, 0, "sys_memory_allocate", 0, args=[0x100000, 0x400, 0xD0001000] + [0] * 5,
                  wr=[[2, 0, 1, "40"]], argc=3),
             call(1, 1, "cellUse", 0, args=[0x40100000] + [0] * 7, argc=1),
             call(1, 2, "cellGetArea", 0x40200000, argc=0),
             call(1, 3, "sys_memory_allocate", 0, args=[0x100000, 0x400, 0xD0001000] + [0] * 5,
                  wr=[[2, 0, 1, "41"]], argc=3)]
        b = [call(0x1000000, 0, "sys_memory_allocate", 0, args=[0x100000, 0x400, 0xD0001000] + [0] * 5,
                  wr=[[2, 0, 1, "30"]], argc=3),
             call(0x1000000, 1, "cellUse", 0, args=[0x30100000] + [0] * 7, argc=1),
             call(0x1000000, 2, "cellGetArea", 0x30200000, argc=0),
             call(0x1000000, 3, "sys_memory_allocate", 0, args=[0x100000, 0x400, 0xD0001000] + [0] * 5,
                  wr=[[2, 0, 1, "31"]], argc=3)]
        out, r = self.run_diff(a, b)
        self.assertEqual(r["first"][0]["a"]["name"], "sys_memory_allocate")
        out, r = self.run_diff(a, b, ["--addr-map", "0x40000000=0x30000000:0x1000000"])
        f = r["first"][0]
        # the first three map; the last is outside the 16 MB region given
        self.assertEqual((f["a"]["seq"], f["diffs"][0][0]), (3, "mem"))
        self.assertEqual(r["addr_map"], [["0x40000000", "0x30000000", "0x1000000"]])

    def test_baseline_triage(self):
        d = tempfile.mkdtemp()
        bp = os.path.join(d, "base.json")
        with open(bp, "w") as f:
            json.dump({"entries": {
                "cellKnown": {"class": "benign", "reason": "void r3", "kinds": ["value"]},
                "cellFixed": {"class": "open", "reason": "previously broken"},
            }}, f)
        a = [call(1, 0, "cellKnown", 1), call(1, 1, "cellNewBug", 0x7FF600001000), call(1, 2, "cellFixed", 0)]
        b = [call(0x1000000, 0, "cellKnown", 2), call(0x1000000, 1, "cellNewBug", 0x30001000), call(0x1000000, 2, "cellFixed", 0)]
        out, r = self.run_diff(a, b, ["--baseline", bp])
        self.assertEqual(r["new_unclassified"], ["cellNewBug"])
        self.assertEqual(r["resolved_baseline"], ["cellFixed"])
        self.assertIn("Triage vs baseline", out)

    def test_volatile_error_mismatch(self):
        # cellKbRead is in DEFAULT_VOLATILE, so 0 vs NO_DEVICE (setup diff) and
        # matching NO_DEVICE vs NO_DEVICE are ignored, while disagreeing error
        # codes (wrong error base 0x80121205 vs real 0x80121007) are caught.
        a = [call(1, 0, "cellKbRead", 0, args=[0, 0x1000] + [0] * 6, argc=2),
             call(1, 1, "cellKbRead", 0x80121007, args=[1, 0x1000] + [0] * 6, argc=2),
             call(1, 2, "cellKbRead", 0x80121205, args=[2, 0x1000] + [0] * 6, argc=2)]
        b = [call(0x1000000, 0, "cellKbRead", 0x80121007, args=[0, 0x1000] + [0] * 6, argc=2),
             call(0x1000000, 1, "cellKbRead", 0x80121007, args=[1, 0x1000] + [0] * 6, argc=2),
             call(0x1000000, 2, "cellKbRead", 0x80121007, args=[2, 0x1000] + [0] * 6, argc=2)]
        _, r = self.run_diff(a, b)
        self.assertEqual(r["first"][0]["a"]["seq"], 2)
        self.assertEqual(r["first"][0]["diffs"][0][0], "ret")
        self.assertIn("0x80121205 vs 0x80121007", r["first"][0]["diffs"][0][1])


class LogConvertTest(unittest.TestCase):
    """rpcs3_probe/rpcs3_log2calltrace.py on lines in RPCS3's log formats."""

    def test_convert(self):
        sys.path.insert(0, os.path.join(HERE, "rpcs3_probe"))
        import rpcs3_log2calltrace as l2c
        nid = calltrace.compute_nid("cellSysutilRegisterCallback")
        log = "\n".join([
            "\u00b7! 0:00:00.000100 SYS: RPCS3 v0.0.43",
            "\u00b7T 0:00:01.000001 {PPU[0x1000000] Thread (main_thread) [HLE:0x00010008, LR:0x00012344]} "
            "cellSysutil: cellSysutilRegisterCallback(slot=0, func=*0x5000, userdata=*0x0)",
            "\u00b7E 0:00:01.000002 {PPU[0x1000000] Thread (main_thread) [HLE:0x00010008, LR:0x00012344]} "
            "cellSysutil: 'cellSysutilRegisterCallback' failed with 0x8002b101 : CELL_SYSUTIL_ERROR_VALUE [1]",
            "\u00b7T 0:00:01.000003 {PPU[0x1000000] Thread (main_thread) [0x00012400]} "
            "sys_mutex: sys_mutex_lock(mutex_id=0x85000100, timeout=0x0)",
            "\u00b7T 0:00:01.000004 {PPU[0x1000000] Thread (main_thread) [0x00012400]} "
            "PPU: Syscall 'sys_mutex_lock' (100) finished, r3=0x0",
            "\u00b7T 0:00:01.000005 {PPU[0x1000001] Thread (worker) [libsre: 0x30001000]} "
            "PPU: Syscall 'sys_event_flag_wait' (82) finished, r3=0x0",
            "\u00b7N 0:00:01.000006 {PPU[0x1000000] Thread (main_thread) [HLE:0x00010010, LR:0x00012500]} "
            "cellGame: unrelatedHelper(x=1)",
            "\u00b7W 0:00:01.000007 {PPU[0x1000000] Thread (main_thread) [liblv2: 0x02bd4d60]} "
            "sys_ppu_thread: _sys_ppu_thread_create(): Thread \"_gcm_intr_thread\" created (id=0x1000002, func=*0x2c182d8)",
        ]) + "\n"
        d = tempfile.mkdtemp()
        lp, op = os.path.join(d, "RPCS3.log"), os.path.join(d, "o.jsonl")
        with open(lp, "w", encoding="utf-8") as f:
            f.write(log)
        imports = [{"library": "cellSysutil", "nid": "0x%08x" % nid, "stub": "0x10000"}]
        with open(op, "w") as out:
            n_hle, n_sc, n_drop, arity = l2c.convert(lp, imports, [], False, out)
        self.assertEqual((n_hle, n_sc, n_drop), (1, 1, 1))
        with open(op) as f:
            recs = [json.loads(x) for x in f]
        calls = [r for r in recs if r["type"] == "call"]
        self.assertEqual(calls[0]["name"], "cellSysutilRegisterCallback")
        self.assertEqual(calls[0]["ret"], "0x8002B101")
        self.assertEqual(calls[0]["argc"], 3)
        self.assertEqual(calls[0]["args"][:2], ["0x0", "0x5000"])
        self.assertEqual(calls[0]["lr"], "0x00012344")
        self.assertEqual((calls[1]["sc"], calls[1]["ret"], calls[1]["args"]), (100, "0x0", ["0x85000100", "0x0"]))
        self.assertEqual(arity["cellSysutilRegisterCallback"], 3)
        self.assertEqual({r["tid"]: r["name"] for r in recs if r["type"] == "thread"},
                         {0x1000000: "main_thread", 0x1000001: "worker"})
        threads, _ = l2c.scan_threads_and_arity(lp)
        self.assertEqual(threads[0x1000002], "_gcm_intr_thread")    # named by its creation line


class FakeStub:
    """RPCS3's GDB stub and PPU breakpoints as observed (v0.0.43) and as its
    source reads, at the level the capture rig sees them:

      * a thread executing a breakpoint is debug-paused and calls pause_from:
        the first one since the last vCont becomes `pausedBy` and ends the
        wait with S05; any other one stays paused, unreported ("parked");
      * at the end of a wait the whole emulator pauses, the stub selects
        `pausedBy` (qC names it; after a ^C that is the previous one) and
        removes its debug pause;
      * vCont;c removes the debug pause of the Hc thread and resumes all;
        vCont;s does the same plus a step flag: that thread pauses after its
        next instruction, while every other thread runs too;
      * a thread released on a breakpoint executes the original instruction;
      * an emulator pause can catch a thread at a breakpoint address before it
        executed the breakpoint.

    Threads run scripts of states {pc, sp, lr, r3, r11, block}; `block` is the
    number of ticks the instruction takes (a blocking sc keeps its pc).
    One tick advances every runnable thread by one action, in a seeded random
    order, so two threads can hit breakpoints in the same tick."""

    def __init__(self, scripts, seed=1, ticks_per_wait=12):
        import random
        self.rng = random.Random(seed)
        self.th = {tid: {"i": 0, "fired": False, "pause": False, "step": False, "left": s[0].get("block", 0),
                         "script": s} for tid, s in scripts.items()}
        self.bps = set()
        self.paused = True
        self.paused_by = None
        self.sel = self.hg = self.hc = next(iter(scripts))
        self.running = False
        self.ticks_per_wait = ticks_per_wait
        self.out = []
        self.history = []

    # -- what RSPClient offers ------------------------------------------------
    def add_breakpoint(self, a):
        self.bps.add(a)
        return True

    def remove_breakpoint(self, a):
        self.bps.discard(a)
        return True

    def read_mem(self, addr, n):
        return bytes(n)

    def state(self, tid):
        t = self.th[tid]
        return t["script"][min(t["i"], len(t["script"]) - 1)]

    def read_registers(self):
        s = self.state(self.hg)
        return {"pc": s["pc"], "r1": s.get("sp", 0xD0000000), "lr": s.get("lr", 0), "r3": s.get("r3", 0),
                "r11": s.get("r11", 0)}

    def interrupt(self):
        self.paused = True

    def _send_packet(self, p):
        if p.startswith("Hg"):
            self.hg = self.sel = int(p[2:], 16)
            self.out.append("OK")
        elif p.startswith("Hc"):
            self.hc = self.sel = int(p[2:], 16)
            self.out.append("OK")
        elif p == "qC":
            self.out.append("QC%016x" % self.sel)
        elif p == "qfThreadInfo":
            alive = [t for t, v in self.th.items() if v["i"] < len(v["script"])]
            self.out.append("m" + ",".join("%x" % t for t in alive) + "l" if alive else "l")
        elif p in ("vCont;c", "vCont;s"):
            t = self.th[self.hc]
            t["pause"] = False
            if p.endswith("s"):
                t["step"] = True
            self.paused = False
            self.running = True
        else:
            raise AssertionError(p)

    def _recv_packet(self, deadline=None):
        return self.out.pop(0).encode() if self.out else b""

    def wait_stop(self, deadline=None):
        if not self.running:
            return b""
        for _ in range(self.ticks_per_wait):
            if self.paused:
                break
            if not self.tick():
                break
        if not self.paused:
            if all(v["i"] >= len(v["script"]) for v in self.th.values()):
                return b"W00"
            return b""            # nothing reported in time: the rig interrupts
        self.running = False
        if self.paused_by is not None:
            self.sel = self.paused_by
            self.th[self.paused_by]["pause"] = False
        return b"S05"

    # -- the emulated threads -------------------------------------------------
    def pause_from(self, tid):
        if not self.paused:
            self.paused = True
            self.paused_by = tid

    def tick(self):
        moved = False
        order = list(self.th)
        self.rng.shuffle(order)
        for tid in order:
            t = self.th[tid]
            if t["pause"] or t["i"] >= len(t["script"]):
                continue
            moved = True
            s = t["script"][t["i"]]
            if s["pc"] in self.bps and not t["fired"]:
                t["fired"] = True
                if not t["step"]:
                    t["pause"] = True
                    self.pause_from(tid)
                    continue
            if t["left"] > 0:
                t["left"] -= 1
            else:
                t["i"] += 1
                t["fired"] = False
                if t["i"] < len(t["script"]):
                    t["left"] = t["script"][t["i"]].get("block", 0)
            if t["step"]:
                t["step"] = False
                t["pause"] = True
                self.pause_from(tid)
        return moved


def sim_scripts(nthreads=4, ncalls=12, seed=7):
    """Threads making calls through two import stubs and one blocking sc,
    with loops back to the same stub at the same stack: (scripts, stubs,
    sc_sites, expected calls per thread as (kind, ret) in order)."""
    import random
    rng = random.Random(seed)
    STUB_A, STUB_B, BODY, SC = 0x20000, 0x20020, 0x80000000, 0x30010
    scripts, expect = {}, {}
    for n in range(nthreads):
        tid = 0x1000000 + n
        sp = 0xD0010000 - n * 0x10000
        s, e = [{"pc": 0x10000 + n * 0x100, "sp": sp}], []
        for k in range(ncalls):
            filler = 0x10004 + n * 0x100 + (k % 3) * 0x10
            r3 = (n << 8) | k
            what = rng.choice(("a", "b", "sc"))
            if what == "sc":
                s += [{"pc": SC, "sp": sp, "r11": 82, "block": rng.choice((0, 0, 3, 20))},
                      {"pc": SC + 4, "sp": sp, "r3": r3}]
                e.append(("sc", r3))
            else:
                stub = STUB_A if what == "a" else STUB_B
                ret = filler + 8
                s += [{"pc": stub, "sp": sp, "lr": ret},
                      {"pc": BODY, "sp": sp, "block": rng.choice((0, 1, 5))},
                      {"pc": ret, "sp": sp, "r3": r3}]
                e.append(("hle", r3))
            s.append({"pc": filler, "sp": sp})
        scripts[tid] = s
        expect[tid] = e
    return scripts, {STUB_A: 0x1111, STUB_B: 0x2222}, [SC], expect


class GdbRigTest(unittest.TestCase):
    """The pure parts of the GDB-stub capture (no RPCS3 needed)."""

    def setUp(self):
        sys.path.insert(0, os.path.join(HERE, "rpcs3_probe"))

    def test_sc_numbers_from_elf(self):
        import struct
        import rpcs3_calltrace
        LI_R11 = 0x39600000          # li r11,N
        SC, NOP, BLR = 0x44000002, 0x60000000, 0x4E800020
        code = [LI_R11 | 141, NOP, SC, BLR,           # sys_timer_usleep
                LI_R11 | 128, 0x7C6B1B78, SC, BLR,    # mr r11,r3 after the li: number unknown
                LI_R11 | 100, BLR, SC]                # a branch before the sc: unknown
        text = b"".join(struct.pack(">I", w) for w in code)
        vaddr, off = 0x10000, 0x40 + 56
        ehdr = bytearray(0x40)
        ehdr[:4] = b"\x7fELF"
        ehdr[0x20:0x28] = struct.pack(">Q", 0x40)
        ehdr[0x36:0x3A] = struct.pack(">HH", 56, 1)
        phdr = struct.pack(">IIQQQQQQ", 1, 5, off, vaddr, vaddr, len(text), len(text), 0x10000)
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "t.elf")
            with open(p, "wb") as f:
                f.write(bytes(ehdr) + phdr + text)
            sites = rpcs3_calltrace.elf_sc_sites(p)
        self.assertEqual(sites, [(vaddr + 8, 141), (vaddr + 24, None), (vaddr + 40, None)])

    def test_capture_loses_nothing(self):
        """Threads hitting breakpoints together, blocking syscalls, loops back
        to the same stub: every call is recorded once, in order, with its
        return (the first captures lost ~1% of returns and some calls)."""
        import io
        import rpcs3_calltrace
        # 4 threads; then 8 threads with blocked threads re-released at every
        # sweep (stale_release 0), where a queued thread often runs on to a
        # new breakpoint before its turn (lost one call in 4 of 15 seeds
        # before releases looked at the thread first)
        cases = [(seed, 4, 12, 30.0) for seed in range(1, 9)] + [(seed, 8, 30, 0.0) for seed in range(1, 16)]
        for seed, nthreads, ncalls, stale in cases:
            scripts, stubs, sc, expect = sim_scripts(nthreads=nthreads, ncalls=ncalls, seed=seed)
            stub = FakeStub(scripts, seed=seed)
            out = io.StringIO()
            tr = rpcs3_calltrace.Tracer(stub, out, 16, stubs, sc, {}, {}, 0)
            tr.stale_release = stale
            with contextlib.redirect_stdout(io.StringIO()):
                tr.run(20, quiet=0.01)
            calls = [json.loads(x) for x in out.getvalue().splitlines()]
            calls = [r for r in calls if r["type"] == "call"]
            got = {}
            for r in sorted(calls, key=lambda r: r["seq"]):
                got.setdefault(r["tid"], []).append((r["kind"], None if r.get("ret") is None else int(r["ret"], 16)))
            self.assertEqual(got, expect, "seed %d, %d threads" % (seed, nthreads))
            self.assertGreater(tr.parked_found, 0)        # the case that needs a sweep did occur

    def test_max_per_site_retires_cleanly(self):
        """--max-per-site N retires hot entry breakpoints after N hits without
        losing in-flight returns or stalling parked threads."""
        import io
        import rpcs3_calltrace
        for seed in range(1, 9):
            scripts, stubs, sc, expect = sim_scripts(nthreads=4, ncalls=16, seed=seed)
            stub = FakeStub(scripts, seed=seed)
            out = io.StringIO()
            tr = rpcs3_calltrace.Tracer(stub, out, 16, stubs, sc, {}, {}, 0, max_per_site=3)
            with contextlib.redirect_stdout(io.StringIO()):
                tr.run(20, quiet=0.01)
            calls = [json.loads(x) for x in out.getvalue().splitlines() if json.loads(x)["type"] == "call"]
            # No call lost its return value.
            self.assertTrue(all(c.get("ret") is not None and not c.get("open") for c in calls), "seed %d" % seed)
            # Every stub/sc site hit >= 3 times was retired and its breakpoint removed.
            self.assertGreater(len(tr.retired), 0)
            for pc in tr.retired:
                self.assertNotIn(pc, stub.bps)
            # Total recorded calls is strictly less than the un-retired total (4*16=64).
            self.assertLess(len(calls), 64)
            # Every thread still finished its entire script (no thread left debug-paused).
            self.assertTrue(all(v["i"] >= len(v["script"]) for v in stub.th.values()), "seed %d" % seed)

    def test_check_capture_against_log(self):
        """check_capture.py: one run seen by the GDB capture and by the log."""
        import check_capture

        def g(seq, name, lr, ret, **kw):
            c = call(0x1000000, seq, name, ret, **kw)
            c["lr"] = lr
            return c

        def lg(seq, name, lr, ret=None, sc=None, site=None):
            c = call(0x1000000, seq, name, ret, sc=sc)
            c.update(lr=lr, src="rpcs3-log", wr=None)
            if site:
                c["site"] = site
            return c
        gdb = [g(0, "cellSysmoduleLoadModule", "0x00041CF8", 0),
               g(1, "sys_mutex_lock", "0x00337A10", 0, sc=100),
               g(2, "cellGameBootCheck", "0x0003EA30", 0),
               g(3, "cellGcmSetFlipMode", "0x0002FCB4", 0)]           # not logged by RPCS3: extra
        gdb[1]["site"] = "0x00337A60"
        log = [lg(0, "cellSysmoduleLoadModule", "0x00041CF8"),
               lg(1, "cellSysmoduleLoadModule", "0x00041CF8"),        # a dependency RPCS3 loads: internal
               lg(2, "sys_mutex_lock", None, ret=0, sc=100, site="0x00337A64"),   # logged at sc+4
               lg(3, "cellGameContentPermit", "0x0003EA74"),          # missing in the capture: lost
               lg(4, "cellGameBootCheck", "0x0003EA30"),
               lg(5, "cellGamePatchCheck", "0x0003EA84")]             # after the capture's end
        d = tempfile.mkdtemp()
        pg, pl = os.path.join(d, "g.jsonl"), os.path.join(d, "l.jsonl")
        write(pg, "rpcs3", {0x1000000: "main_thread"}, gdb)
        write(pl, "rpcs3", {0x1000000: "main_thread"}, log)
        res = check_capture.check(calltrace.Trace(pg), calltrace.Trace(pl), lambda c: True)
        st = res["threads"][0x1000000]
        self.assertEqual((st["matched"], st["lost"], st["internal"], st["extra"], st["after"]), (3, 1, 1, 1, 1))
        self.assertEqual(res["lost"][0][1]["name"], "cellGameContentPermit")
        self.assertEqual(res["site_mismatch"], [])
        self.assertEqual(res["ret_mismatch"], [])

    def test_fallback_register_layout(self):
        import rsp
        c = rsp.RSPClient()
        c._fallback_registers()
        size = {n: s for n, _o, s in c.reg_layout}
        self.assertEqual(sum(size.values()) * 2, 1112)     # RPCS3's `g` reply length
        self.assertEqual((size["cr"], size["xer"], size["fpscr"], size["lr"]), (4, 4, 4, 8))
        self.assertEqual(c.reg_by_name["lr"][0], 64 * 8 + 8 + 8 + 4)


if __name__ == "__main__":
    unittest.main()
