#!/usr/bin/env python3
"""rpcs3_calltrace.py - firmware call trace of a title running in RPCS3, over
RPCS3's GDB stub, in the ps3-calltrace/1 format (tools/calltrace.py).

The reference side of tools/hle_diff.py. It puts a breakpoint on every import
stub of the title (the lift's EBOOT.imports.json: the same addresses the
recompiled title routes through ps3_hle_call) and on every `sc` in the title's
own code (the same calls the port's lv2_syscall sees). At a stub it records the
thread, r3..r10 and a window of guest memory behind each pointer argument, then
breaks at the caller's return address (same thread, same r1) and records r3 and
the windows again. The bytes that changed are the call's written memory,
computed exactly as the port side computes them.

Needs: RPCS3 with the PPU decoder set to Interpreter (RPCS3 refuses breakpoints
under LLVM) and its GDB server on (config.yml: Miscellaneous / GDB Server,
default 127.0.0.1:2345). Every hit pauses the emulator, so filter to what you
are looking at; lwmutex/lwcond/spinlock traffic is excluded by default.

  python rpcs3_calltrace.py --rpcs3 rpcs3.exe --title <disc or EBOOT.BIN>
      --imports analysis/EBOOT.imports.json --elf EBOOT.elf --out rpcs3.jsonl
      [--seconds 300] [--max-calls N] [--filter PATTERNS] [--win 256]
      [--log <RPCS3 dir>/RPCS3.log] [--arity arity.json]

--attach skips the launch and connects to an RPCS3 already waiting on the stub.
--log merges thread names (and argument counts) from the RPCS3.log of the run.

Python 3.9+, standard library only. Title-agnostic.
"""
import argparse
import collections
import fnmatch
import json
import os
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
from rsp import RSPClient, RSPError  # noqa: E402
import calltrace  # noqa: E402

DEFAULT_EXCLUDE = ["sys_lwmutex_*", "_sys_lwmutex_*", "sys_lwcond_*", "_sys_lwcond_*",
                   "sys_spinlock_*", "sys_ppu_thread_get_id", "sys_time_*", "_sys_time_*",
                   "sys_timer_usleep", "sys_process_is_stack", "_sys_heap_*"]


def nid_names():
    """NID -> name from the toolkit's NID database, when it is importable."""
    try:
        import nid_database
        db = nid_database.get_default_db()
        return {nid: name for nid, _mod, name in db.all_entries()}
    except Exception:
        return {}


def elf_sc_sites(path):
    """(address, syscall number or None) of each `sc` (0x44000002) in the
    executable segments of a PPU ELF. The number is the `li r11,N` among the
    eight instructions before the `sc` (up to a branch); a site without one is
    kept with None and filtered at run time only."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"\x7fELF":
        raise SystemExit("%s: not a decrypted ELF" % path)
    phoff, = struct.unpack(">Q", data[0x20:0x28])
    phentsize, phnum = struct.unpack(">HH", data[0x36:0x3A])
    sites = []
    for i in range(phnum):
        p = data[phoff + i * phentsize: phoff + (i + 1) * phentsize]
        ptype, flags, off, vaddr, _pa, filesz = struct.unpack(">IIQQQQ", p[:40])
        if ptype != 1 or not flags & 1:
            continue
        seg = data[off:off + filesz]
        for k in range(0, len(seg) - 3, 4):
            if seg[k:k + 4] == b"\x44\x00\x00\x02":
                num = None
                for j in range(k - 4, max(k - 36, -1), -4):
                    w, = struct.unpack(">I", seg[j:j + 4])
                    if w >> 26 in (16, 18) or (w >> 26 == 19 and (w >> 1) & 0x3FF in (16, 528)):
                        break                       # b, bc, bclr, bcctr: another path
                    if w & 0xFFFF0000 == 0x39600000:       # li r11,N (addi r11,0,N)
                        num = w & 0x7FFF
                        break
                    if (w >> 21) & 31 == 11 or (w >> 16) & 31 == 11:
                        break                       # anything else naming r11 (mr r11,..., a load): unknown
                sites.append((vaddr + k, num))
    return sites


def make_filter(text, default_exclude=True):
    """wanted(name, lib, nid=None, sc=None) for a --filter text (names with *,
    libraries, NIDs, sc, sc:N; '-' excludes; later rules win), plus the
    default exclusions. Also used by check_capture.py to know what a capture
    traced from its header."""
    rules = [p.strip() for p in (text or "").split(",") if p.strip()]
    if default_exclude:
        rules += ["-" + p for p in DEFAULT_EXCLUDE]
    includes = [r for r in rules if not r.startswith("-")]

    def wanted(name, lib, nid=None, sc=None):
        ok = not includes
        for r in rules:
            neg = r.startswith("-")
            p = r[1:] if neg else r
            hit = (p == "sc" and sc is not None) or (sc is not None and p == "sc:%d" % sc) or \
                  (nid is not None and p.lower().startswith("0x") and int(p, 16) == nid) or \
                  (name and fnmatch.fnmatchcase(name, p)) or (lib and fnmatch.fnmatchcase(lib, p))
            if hit:
                ok = not neg
        return ok
    return wanted


def launch(rpcs3, title):
    flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
    return subprocess.Popen([os.path.abspath(rpcs3), "--no-gui", os.path.abspath(title)], stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL, creationflags=flags)


class Tracer:
    def __init__(self, rsp, out, win, stubs, sc_sites, names, arity, max_calls, max_per_site=0):
        self.rsp, self.out, self.win = rsp, out, win
        self.stubs = dict(stubs)      # addr -> nid (active entry breakpoints)
        self.sc_sites = set(sc_sites)
        self.names = names
        self.arity = arity
        self.max_calls = max_calls
        self.max_per_site = max_per_site
        self.site_hits = collections.Counter()
        self.retiring = set()         # entry bps that reached max_per_site
        self.retiring_ret = set()     # return bps whose pending list emptied (sweep before removing)
        self.retired = set()          # entry bps removed after reaching max_per_site
        self.pending = {}             # return addr -> [(tid, sp, rec, snaps)]
        self.ret_bps = set()
        self.seq = {}
        self.depth = {}
        self.written = 0
        self.t0 = time.time()
        self.sweeps = 0
        self.parked_found = 0
        self.stops = 0
        self.interrupts = 0
        self.releases = 0
        self.verbose = False
        self.sweep_every = 10.0
        self.release_wait = 0.5         # before a ^C, with parked threads still to release
        self.stale_release = 30.0       # a thread seen inside an sc is released again after this
        self.queue = collections.deque()   # parked threads recorded, to release (Hc + vCont;c)
        self.released = {}              # tid -> time of its last release
        self.at = {}                    # tid -> (pc, sp) of the breakpoint it was last recorded at
        self.hc = None                  # the stub's Hc thread, as last set
        self.safe = None                # a thread whose release runs nothing unrecorded
        self.named = {}

    def pc(self, regs):
        for n in ("pc", "cia", "nip", "iar"):
            if n in regs:
                return regs[n] & 0xFFFFFFFF
        raise RSPError("no PC register")

    def snap(self, args, argc):
        out = []
        for k, v in enumerate(args):
            if (argc is not None and k >= argc) or v >> 32 or not (0x10000 <= v < 0xE0000000) or not self.win:
                out.append(None)
                continue
            try:
                out.append(self.rsp.read_mem(v, self.win))
            except RSPError:
                out.append(None)
        return out

    def current_thread(self, after_interrupt=False):
        """qC: the thread the stub selected at the last stop (its `pausedBy`).
        After a ^C that raced a breakpoint stop, the stub also answers the ^C
        itself, outside vCont, with an empty packet: skip it."""
        self.rsp._send_packet("qC")
        r = self.rsp._recv_packet().decode("latin-1")
        if not r and after_interrupt:
            try:
                r = self.rsp._recv_packet(deadline=time.time() + 0.5).decode("latin-1")
            except RSPError:
                pass
        return int(r[2:], 16) if r.startswith("QC") else None

    def select_g(self, tid):
        """Hg: registers are read from this thread (Hc is left alone)."""
        self.rsp._send_packet("Hg%x" % tid)
        self.rsp._recv_packet()

    def select_c(self, tid):
        """Hc: the thread whose debug pause the next vCont;c removes."""
        if tid != self.hc:
            self.rsp._send_packet("Hc%x" % tid)
            self.rsp._recv_packet()
            self.hc = tid

    def begin(self, pc, tid, sp, regs):
        args = [regs.get("r%d" % i, 0) for i in range(3, 11)]
        if pc in self.stubs:
            nid = self.stubs[pc]
            key = ("hle", nid)
            rec = {"type": "call", "kind": "hle", "nid": "0x%08X" % nid, "name": self.names.get(nid, "")}
            ret_addr = regs.get("lr", 0) & 0xFFFFFFFF
        else:
            num = regs.get("r11", 0) & 0xFFFFFFFF
            key = ("sc", num)
            rec = {"type": "call", "kind": "sc", "sc": num, "site": "0x%08X" % pc}
            ret_addr = pc + 4
        argc = self.arity.get(key)
        seq = self.seq.get(tid, 0)
        self.seq[tid] = seq + 1
        d = self.depth.get(tid, 0)
        self.depth[tid] = d + 1
        # lr: the LR register at the call, as the port records it (for an import
        # stub that is the caller's return address; for an sc, the return
        # address of the function that contains it)
        rec.update(tid=tid, seq=seq, depth=d, t_us=int((time.time() - self.t0) * 1e6),
                   lr="0x%08X" % (regs.get("lr", 0) & 0xFFFFFFFF), args=["0x%X" % a for a in args])
        if argc is not None:
            rec["argc"] = argc
        snaps = self.snap(args, argc)
        self.pending.setdefault(ret_addr, []).append((tid, sp, rec, snaps))
        if ret_addr not in self.ret_bps and ret_addr not in self.stubs and ret_addr not in self.sc_sites:
            self.rsp.add_breakpoint(ret_addr)
            self.ret_bps.add(ret_addr)
        if self.max_per_site > 0:
            self.site_hits[pc] += 1
            # Never retire sys_ppu_thread_create: it names threads spawned late in the run.
            if self.site_hits[pc] >= self.max_per_site and rec.get("name") != "sys_ppu_thread_create":
                self.retiring.add(pc)

    def finish(self, rec, snaps, regs):
        rec["ret"] = "0x%X" % regs.get("r3", 0)
        args = [int(a, 16) for a in rec["args"]]
        wr = []
        for k, pre in enumerate(snaps):
            if pre is None:
                continue
            try:
                post = self.rsp.read_mem(args[k], len(pre))
            except RSPError:
                continue
            for off, ln, data in calltrace.written_runs(pre, post):
                wr.append([k, off, ln, data])
        rec["wr"] = wr
        self.out.write(json.dumps(rec) + "\n")
        self.written += 1
        if rec.get("name") == "sys_ppu_thread_create" and regs.get("r3", 0) & 0xFFFFFFFF == 0:
            self.name_created_thread(args)
        self.out.flush()

    def name_thread(self, tid, name):
        if tid not in self.named:
            self.named[tid] = name
            self.out.write(json.dumps({"type": "thread", "tid": tid, "name": name}) + "\n")

    def name_created_thread(self, args):
        """sys_ppu_thread_create(u64 *id, entry, arg, prio, stack, flags, name):
        the new thread's id and name, so the trace pairs threads without a log."""
        try:
            tid = int.from_bytes(self.rsp.read_mem(args[0], 8), "big")
            raw = self.rsp.read_mem(args[6], 64) if args[6] else b""
        except RSPError:
            return
        self.name_thread(tid, raw.split(b"\0", 1)[0].decode("latin-1"))

    def threads(self):
        out = []
        self.rsp._send_packet("qfThreadInfo")
        while True:
            r = self.rsp._recv_packet().decode("latin-1")
            if not r.startswith("m"):
                break
            out += [int(x, 16) for x in r[1:].rstrip("l").split(",") if x]
            if r.endswith("l"):
                break
            self.rsp._send_packet("qsThreadInfo")
        return out

    def on_bp(self, pc):
        return pc in self.stubs or pc in self.sc_sites or pc in self.ret_bps

    def inside_sc(self, tid, pc, sp):
        """A thread blocked in a traced syscall reports the `sc` as its pc."""
        return pc in self.sc_sites and any(t == tid and s == sp for t, s, _r, _n in self.pending.get(pc + 4, ()))

    def handle(self, tid, regs=None):
        """A thread seen at a stop: record the call or return at its breakpoint.

        Returns "new" (recorded now), "seen" (on the breakpoint it was last
        recorded at, same stack: it has not moved since, because a thread
        cannot come back to a breakpoint without stopping at another one in
        between, the call's return or the next stub) or None (not on a
        breakpoint; its last position is forgotten)."""
        if regs is None:
            self.select_g(tid)
            regs = self.rsp.read_registers()
        pc = self.pc(regs)
        sp = regs.get("r1", 0) & 0xFFFFFFFF
        if not self.on_bp(pc):
            self.at.pop(tid, None)
            return None
        if self.at.get(tid) == (pc, sp) or self.inside_sc(tid, pc, sp):
            return "seen"
        self.at[tid] = (pc, sp)
        # A return first: a stub can also be a return address (a tail call).
        lst = self.pending.get(pc)
        if lst:
            for i, (ptid, psp, rec, snaps) in enumerate(lst):
                if ptid == tid and psp == sp:
                    lst.pop(i)
                    self.finish(rec, snaps, regs)
                    self.depth[tid] = rec["depth"]
                    break
            if not lst:
                self.pending.pop(pc, None)
                if pc not in self.stubs and pc not in self.sc_sites and pc in self.ret_bps:
                    if self.retired:
                        self.retiring_ret.add(pc)
                    else:
                        self.rsp.remove_breakpoint(pc)
                        self.ret_bps.discard(pc)
        if pc in self.stubs or pc in self.sc_sites:
            self.begin(pc, tid, sp, regs)
        return "new"

    def sweep(self, keep=None):
        """With the emulator stopped: record, and queue for release, every
        thread on a breakpoint that no stop reply reported. RPCS3 debug-pauses
        every thread that hits a breakpoint but reports only the first (the
        stub's `pausedBy`); a vCont;c removes the debug pause from that thread
        and from the Hc thread only, so the others stay paused until each is
        the Hc thread of a vCont;c. A thread the emulator pause caught just
        before it executed a breakpoint instruction is recorded here as well;
        when it then hits the breakpoint the stop is "seen". `keep` is the
        thread already handled at this stop."""
        self.sweeps += 1
        now = time.time()
        for tid in self.threads():
            if tid == keep:
                continue
            self.select_g(tid)
            regs = self.rsp.read_registers()
            pc = self.pc(regs)
            sp = regs.get("r1", 0) & 0xFFFFFFFF
            if self.verbose:
                print("[calltrace]   thread 0x%X pc 0x%08X lr 0x%08X r1 0x%08X calls %d" % (
                    tid, pc, regs.get("lr", 0) & 0xFFFFFFFF, sp, self.seq.get(tid, 0)), flush=True)
            st = self.handle(tid, regs)
            if st is None:
                if self.safe is None:
                    self.safe = tid
                continue
            if tid in self.queue:
                continue
            if st == "new":
                self.parked_found += 1
            elif self.inside_sc(tid, pc, sp):
                # most likely blocked in its syscall (nothing to release); now
                # and then released anyway, in case it is parked on the `sc`
                # breakpoint itself
                if now - self.released.get(tid, 0) < self.stale_release:
                    continue
            elif now - self.released.get(tid, 0) < 1.0:
                continue
            self.queue.append(tid)

    def retire_sites(self, keep=None):
        """Remove entry breakpoints that reached max_per_site and return
        breakpoints whose pending calls finished after a retirement. Checks all
        threads first while those breakpoints are still active so any thread
        already debug-paused on one is recorded and queued for release before
        the breakpoint is removed."""
        retiring_all = self.retiring | self.retiring_ret
        if not retiring_all:
            return
        for tid in self.threads():
            if tid == keep:
                continue
            self.select_g(tid)
            regs = self.rsp.read_registers()
            pc = self.pc(regs)
            if pc not in retiring_all:
                continue
            st = self.handle(tid, regs)
            if st == "new":
                self.parked_found += 1
            if tid not in self.queue:
                self.queue.append(tid)
        for pc in list(self.retiring):
            self.retiring.discard(pc)
            self.stubs.pop(pc, None)
            self.sc_sites.discard(pc)
            self.retired.add(pc)
            if pc in self.pending:
                # Still a return target for an in-flight call; handle() will retire it once empty.
                self.ret_bps.add(pc)
            else:
                self.rsp.remove_breakpoint(pc)
                self.ret_bps.discard(pc)
        for pc in list(self.retiring_ret):
            self.retiring_ret.discard(pc)
            if pc not in self.pending and pc not in self.stubs and pc not in self.sc_sites:
                self.rsp.remove_breakpoint(pc)
                self.ret_bps.discard(pc)

    def run(self, seconds, quiet=3.0):
        # RPCS3 names the title's first thread main_thread (the lowest id). At
        # boot it is the only one; from a savestate the others are named by
        # --log (RPCS3.log) or stay unnamed.
        present = sorted(self.threads())
        if present:
            self.name_thread(present[0], "main_thread")
            self.safe = present[0]
        for a in list(self.stubs) + list(self.sc_sites):
            if not self.rsp.add_breakpoint(a):
                print("[calltrace] breakpoint refused at 0x%08X (PPU decoder must be Interpreter)" % a)
        deadline = time.time() + seconds
        try:
            self.loop(deadline, quiet)
        except RSPError as e:
            print("[calltrace] stub stopped answering (%s); last packets in <out>.rsp.txt" % e, flush=True)
        try:
            self.rsp.interrupt()
            self.rsp.wait_stop(deadline=time.time() + 10)
        except RSPError:
            pass
        # calls still open (a thread blocked inside one) are written without a return
        for lst in self.pending.values():
            for _tid, _sp, rec, _snaps in lst:
                rec["ret"] = None
                rec["wr"] = None
                rec["open"] = True
                self.out.write(json.dumps(rec) + "\n")
        print("[calltrace] %d stops, %d interrupts, %d sweeps, %d parked threads found, %d releases" % (
            self.stops, self.interrupts, self.sweeps, self.parked_found, self.releases))

    def loop(self, deadline, quiet):
        """No single-stepping. RPCS3 resumes a thread paused on a breakpoint by
        executing the original instruction (the breakpoint does not fire
        again), so a recorded thread needs only a vCont;c. A step would resume
        every other thread too (the stub resumes the whole emulator for it),
        and a breakpoint stop of one of them would be taken for the step's
        end, after the stub had already released that thread: the records the
        first captures lost.

        Each stop leaves one thread released by the stub, the one qC names; it
        is recorded before anything runs again. Each vCont;c releases one more,
        the Hc thread: a thread a sweep found parked, else the thread just
        handled (already free, so the release lets nothing unrecorded run)."""
        next_report = time.time() + 30
        next_sweep = time.time() + self.sweep_every
        while time.time() < deadline and (not self.max_calls or self.written < self.max_calls):
            if self.queue:
                target = self.queue.popleft()
                # It may have run since it was queued (the stub released it
                # as a stale pausedBy, or a syscall it was blocked in
                # returned) and be parked on a breakpoint not recorded yet:
                # the release would let it pass. Look first.
                self.handle(target)
                self.released[target] = time.time()
                self.releases += 1
            else:
                target = self.safe
            if target is not None:
                self.select_c(target)
            self.rsp._send_packet("vCont;c")
            # with threads still queued, wait briefly, then interrupt to release
            # the next (a ^C sent at once after vCont;c can leave the stub
            # unresponsive; one sent while it waits is safe)
            wait = self.release_wait if self.queue else quiet
            rep = self.rsp.wait_stop(deadline=min(deadline, time.time() + wait) if wait else deadline)
            if rep[:1] in (b"W", b"X"):
                break
            interrupted = not rep
            if interrupted:
                if time.time() >= deadline:
                    break
                self.rsp.interrupt()
                self.interrupts += 1
                if not self.rsp.wait_stop(deadline=time.time() + 10):
                    raise RSPError("no reply to an interrupt")
            else:
                self.stops += 1
            # The stub has removed this thread's debug pause: record it now.
            # After a ^C it is the thread that stopped last (the stub keeps
            # that id), which may since have parked on another breakpoint.
            tid = self.current_thread(after_interrupt=interrupted)
            self.safe = tid
            if tid is not None:
                self.handle(tid)
            if self.retiring or self.retiring_ret:
                self.retire_sites(keep=tid)
                next_sweep = time.time() + self.sweep_every
            elif (interrupted and not self.queue) or tid is None or \
                    (self.sweep_every and time.time() >= next_sweep):
                # quiet, or stops keep coming so no quiet period does: look
                # for threads parked on a breakpoint
                self.sweep(keep=tid)
                next_sweep = time.time() + self.sweep_every
            if time.time() >= next_report:
                next_report = time.time() + 30
                print("[calltrace] %d calls, %d threads, %.0f s, %d stops, %d sweeps, %d parked found, %d retired" % (
                    self.written, len(self.seq), time.time() - self.t0, self.stops, self.sweeps,
                    self.parked_found, len(self.retired)), flush=True)


def merge_log(out_path, log_path):
    """Thread names and argument counts from the run's RPCS3.log."""
    import rpcs3_log2calltrace as l2c
    threads, arity = l2c.scan_threads_and_arity(log_path)
    with open(out_path, "a") as f:
        for tid, name in threads.items():
            f.write(json.dumps({"type": "thread", "tid": tid, "name": name}) + "\n")
    return threads, arity


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rpcs3")
    ap.add_argument("--title")
    ap.add_argument("--attach", action="store_true", help="connect to a running RPCS3")
    ap.add_argument("--port", type=int, default=2345)
    ap.add_argument("--imports", required=True, help="EBOOT.imports.json (stub address per NID)")
    ap.add_argument("--elf", help="the title's decrypted ELF, for its sc sites")
    ap.add_argument("--out", required=True)
    ap.add_argument("--seconds", type=float, default=300)
    ap.add_argument("--max-calls", type=int, default=0)
    ap.add_argument("--max-per-site", type=int, default=0,
                    help="retire an entry breakpoint after N hits (0 = never), letting hot loops run at full speed")
    ap.add_argument("--filter", help="only these (names with *, libraries, NIDs, sc, sc:N); '-' excludes")
    ap.add_argument("--no-default-exclude", action="store_true")
    ap.add_argument("--win", type=int, default=256)
    ap.add_argument("--quiet", type=float, default=3,
                    help="after this many seconds without a stop, interrupt and release threads parked on a "
                         "breakpoint that RPCS3 did not report (0 = never)")
    ap.add_argument("--sweep-every", type=float, default=10,
                    help="also look for parked threads every this many seconds while stops keep coming (0 = never)")
    ap.add_argument("--release-wait", type=float, default=0.5,
                    help="seconds a released parked thread runs before the next one is released")
    ap.add_argument("--verbose", action="store_true", help="print every thread's pc at each sweep")
    ap.add_argument("--arity", help="JSON {name: argc} (rpcs3_log2calltrace.py --arity-out)")
    ap.add_argument("--log", help="RPCS3.log of this run, to merge thread names")
    args = ap.parse_args()

    names = nid_names()
    with open(args.imports) as f:
        imports = json.load(f)
    wanted = make_filter(args.filter, not args.no_default_exclude)
    stubs = {}
    for e in imports:
        nid = int(e["nid"], 16)
        if wanted(names.get(nid, ""), e.get("library"), nid=nid):
            stubs[int(e["stub"], 16)] = nid
    sc_sites = []
    if args.elf:
        sc_sites = [a for a, num in elf_sc_sites(args.elf) if wanted("", "", sc=-1 if num is None else num)]
    arity = {}
    if args.arity:
        with open(args.arity) as f:
            for name, n in json.load(f).items():
                if name.startswith("sc:"):
                    arity[("sc", int(name[3:]))] = n
                else:
                    arity[("hle", calltrace.compute_nid(name))] = n
    print("[calltrace] %d import stubs, %d sc sites" % (len(stubs), len(sc_sites)))

    proc = None
    if not args.attach:
        if not (args.rpcs3 and args.title):
            raise SystemExit("--rpcs3 and --title (or --attach)")
        proc = launch(args.rpcs3, args.title)
    rsp = RSPClient(port=args.port, timeout=30)
    with open(args.out, "w") as out:
        out.write(json.dumps({"type": "header", "schema": calltrace.SCHEMA, "side": "rpcs3",
                              "method": "gdb", "win": args.win, "filter": args.filter or "",
                              "max_per_site": args.max_per_site,
                              "default_exclude": not args.no_default_exclude}) + "\n")
        try:
            rsp.connect(retries=240, delay=0.5)
            rsp.handshake()
            tr = Tracer(rsp, out, args.win, stubs, sc_sites, names, arity, args.max_calls,
                        max_per_site=args.max_per_site)
            tr.verbose = args.verbose
            tr.sweep_every = args.sweep_every
            tr.release_wait = args.release_wait
            tr.run(args.seconds, args.quiet)
            print("[calltrace] %d calls written to %s" % (tr.written, args.out))
        finally:
            # the last packets each way, next to the trace: what the stub last said
            with open(args.out + ".rsp.txt", "w") as h:
                for t, d, p in rsp.history:
                    h.write("%.3f %s %s\n" % (t, d, p))
            try:
                rsp.close()
            except Exception:
                pass
            if proc:
                proc.kill()
                proc.wait(timeout=60)     # RPCS3 holds its log open until it exits
    if args.log:
        threads, _ = merge_log(args.out, args.log)
        print("[calltrace] %d thread names merged from %s" % (len(threads), args.log))


if __name__ == "__main__":
    main()
