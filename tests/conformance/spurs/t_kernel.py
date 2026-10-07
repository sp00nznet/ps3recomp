"""The SPURS kernel itself, observed from policy modules and the instance:
selection by per-SPU priority (and priority 0 = never on that SPU), what a
running module's selectWorkload poll answers, one local store per SPU (a
module image reloaded only on a switch; the rest of LS persists), idle-SPU
requests, the instance while a module runs, and the exitIfNoWork life cycle
with WakeUp.

Scheduling-order checks use 1-SPU instances, so that every outcome recorded
is determined by the kernel's rules rather than by which SPU got there first."""
from spurs_lib import P, S

PM_BASE = 0xA00
LS_COUNTER = 0x3F000          # module-entry counter kept in local store (outside any image)
# the data line (128 bytes) a module gets as its workload argument
L = dict(count=0, inside=4, saw2=8, spin=12, st_ready=16, st_signal=20, st_flag=24,
         mode=32, snap=128, poll=256)
MODE_POLL = 1
SNAP_SIZE = 96                # r4, LS 0x1C0..0x1EF, LS 0x220 (uniqueIds), LS counter


def pm_code():
    """r0 = exitToKernel, r4 = {line EA, module EA}, r5 = poll status. Live
    values stay in r80+ (the kernel's select may clobber volatile registers)."""
    a = S.SpuAsm(PM_BASE)
    a.il(127, 0)
    a.ori(93, 0, 0)                             # exit link
    a.rotqbyi(91, 4, 4)                         # line EA -> preferred
    a.ila(90, 0x30000)                          # line buffer
    a.ila(89, 0x30100)                          # snapshot buffer
    a.lqa(81, LS_COUNTER); a.ai(81, 81, 1); a.stqa(81, LS_COUNTER)

    def word(rt, k):
        a.fsmbi(60, 0xF000); a.and_(rt, rt, 60)
        if k:
            a.rotqbyi(rt, rt, 16 - 4 * k)

    a.andi(30, 5, 1); word(30, 0)
    a.rotmi(31, 5, 1); a.andi(31, 31, 1); word(31, 1)
    a.rotmi(32, 5, 2); a.andi(32, 32, 1); word(32, 2)
    a.il(33, 1); word(33, 3)
    a.or_(30, 30, 31); a.or_(30, 30, 32); a.or_(30, 30, 33)
    a.il(34, 1); a.fsmbi(61, 0xFF00); a.and_(34, 34, 61)          # {1, 1, 0, 0}

    def getllar():
        a.wrch(S.MFC_LSA, 90); a.wrch(S.MFC_EAH, 127); a.wrch(S.MFC_EAL, 91)
        a.il(50, S.GETLLAR); a.wrch(S.MFC_Cmd, 50); a.rdch(50, S.MFC_RdAtomicStat)

    def atomic(q0, q1):
        lab = "at%d" % len(a.w)
        a.label(lab)
        getllar()
        a.lqd(51, 90, 0); a.a(51, 51, q0); a.stqd(51, 90, 0)
        a.lqd(52, 90, 16); a.a(52, 52, q1); a.stqd(52, 90, 16)
        a.wrch(S.MFC_LSA, 90); a.wrch(S.MFC_EAH, 127); a.wrch(S.MFC_EAL, 91)
        a.il(50, S.PUTLLC); a.wrch(S.MFC_Cmd, 50); a.rdch(53, S.MFC_RdAtomicStat)
        a.brnz(53, lab)

    def put(r_ls, off, size):
        a.ai(13, 91, off); a.il(14, size)
        a.mfc(S.PUT, r_ls, 13, 14, 3, 15); a.wait_tag(3, 15)

    atomic(34, 30)
    a.stqd(4, 89, 0)
    for k, ls in enumerate((0x1C0, 0x1D0, 0x1E0, 0x220)):
        a.lqa(40, ls); a.stqd(40, 89, 16 * (k + 1))
    a.stqd(81, 89, 80)
    put(89, L["snap"], SNAP_SIZE)
    getllar()
    a.lqd(41, 90, 0); a.rotqbyi(92, 41, 12)                        # spin count
    a.lqd(42, 90, 32); a.ori(94, 42, 0)                            # mode
    a.lqa(95, 0x1D0); a.rotqbyi(95, 95, 12)                        # own wid (LS 0x1DC)
    a.label("spin")
    a.brz(92, "done")
    getllar()
    a.lqd(41, 90, 0); a.rotqbyi(43, 41, 4)
    a.rotqbyi(49, 41, 12); a.brz(49, "done")                       # the PPU cancelled the spin
    a.cgti(44, 43, 1); a.brnz(44, "saw")
    a.andi(45, 94, MODE_POLL); a.brz(45, "next")
    a.il(3, 1)                                                     # selectWorkload(isPoll = 1)
    a.lqa(46, 0x1E0); a.rotqbyi(46, 46, 4)
    a.bisl(0, 46)
    a.ceq(47, 3, 95); a.brz(47, "other")
    a.label("next")
    a.ai(92, 92, -1); a.br("spin")
    a.label("other")                                               # poll named another workload
    a.ila(48, 0x30200); a.stqd(3, 48, 0)
    put(48, L["poll"], 16)
    a.br("done")
    a.label("saw")
    a.il(45, 1); a.fsmbi(62, 0x00F0); a.and_(45, 45, 62)
    atomic(45, 127)
    a.label("done")
    a.il(46, -1); a.fsmbi(63, 0x0F00); a.and_(46, 46, 63)
    atomic(46, 127)
    a.ori(0, 93, 0)
    a.bi(0)
    return a.bytes()


def body(T):
    pm_b = pm_code()
    pm, npm = T.spu_raw("pm", pm_b), len(pm_b)
    pm2 = T.spu_raw("pm2", pm_b)                # the same code at another address
    out = T.alloc("out", 16, 8)
    lines = {}

    def line(name):
        lines[name] = T.alloc("line_" + name, 384, 128)
        return lines[name]

    def prio(name, v):
        return T.alloc("prio_" + name, 16, 16, bytes(v))

    def add(spurs, name, pr, minc=1, maxc=1, module=None):
        widp = T.alloc("wid_" + name, 16)
        T.call("cellSpursAddWorkload", spurs, widp, module or pm, npm, line(name), pr, minc, maxc,
               rc="AddWorkload " + name)
        return ("mem", widp)

    def set_line(name, spin=0, mode=0):
        ln = lines[name]
        for k in range(0, 48, 4):
            T.store_word(ln + k, spin if k == L["spin"] else mode if k == L["mode"] else 0)

    def quiesce(*names):
        for _ in range(2):
            T.sleep_us(20000)
            for n in names:
                T.wait_word(lines[n] + L["inside"], 0)

    def ready(spurs, w, n):
        T.call("cellSpursReadyCountStore", spurs, w, n, rc=False)

    def remove_all(spurs, ws, label):
        for n, w in ws:
            T.call("cellSpursShutdownWorkload", spurs, w, rc="%s: ShutdownWorkload %s" % (label, n))
            T.call("cellSpursWaitForWorkloadShutdown", spurs, w, rc="%s: Wait %s" % (label, n))
            T.call("cellSpursRemoveWorkload", spurs, w, rc="%s: RemoveWorkload %s" % (label, n))
        T.call("cellSpursFinalize", spurs, rc="%s: Finalize" % label)

    # ---- one SPU: priorities, polling, local store ---------------------------------
    s1 = T.spurs_init(nspus=1, name="s1")
    wa = add(s1, "A", prio("hi", [1] * 8))
    wb = add(s1, "B", prio("mid", [8] * 8))
    wc = add(s1, "C", prio("never", [0] * 8))
    wd = add(s1, "D", prio("mid2", [8] * 8), module=pm2)
    T.settle(s1, 1)
    T.record_mem("s1 instance 0x00-0xFF after adds", s1, 0x100)
    T.record_mem("s1 wklInfo1[0..3]", s1 + 0xB00, 128)

    # local store across workloads: exactly one dispatch each, by signal
    for n, w in (("A", wa), ("B", wb), ("D", wd)):
        T.call("cellSpursSendWorkloadSignal", s1, w, rc="signal " + n)
        T.wait_word(lines[n] + L["count"], 1, cond="geu")
        quiesce(n)
        T.record_mem("%s snapshot (r4, LS 0x1C0..0x1EF, LS 0x220, LS counter)" % n,
                     lines[n] + L["snap"], SNAP_SIZE)

    # priority: A (1) always beats B (8) on the one SPU while both are ready
    for n in "ABCD":
        set_line(n, spin=2000)
    ready(s1, wa, 1); ready(s1, wb, 1)
    T.sleep_us(200000)
    T.record_nonzero("A and B ready: A ran", lines["A"] + L["count"])
    T.record_nonzero("A and B ready: B ran", lines["B"] + L["count"])
    ready(s1, wa, 0)
    T.wait_word(lines["B"] + L["count"], 1, cond="geu")
    ready(s1, wb, 0)
    quiesce("A", "B")
    T.record_nonzero("A done: B ran", lines["B"] + L["count"])
    # priority 0 on the SPU: never, until SetPriorities gives it one
    ready(s1, wc, 1)
    T.sleep_us(200000)
    T.record_nonzero("priority 0: C ran", lines["C"] + L["count"])
    pr1 = prio("one", [1] * 8)
    T.call("cellSpursSetPriorities", s1, wc, pr1, rc="SetPriorities C")
    T.wait_word(lines["C"] + L["count"], 1, cond="geu")
    ready(s1, wc, 0)
    quiesce("C")
    T.record_nonzero("after SetPriorities: C ran", lines["C"] + L["count"])

    # a running module's poll: B spins polling; A becomes ready, then signalled
    for why, kick in (("readyCount", lambda: ready(s1, wa, 1)),
                      ("signal", lambda: T.call("cellSpursSendWorkloadSignal", s1, wa, rc=False))):
        set_line("A"); set_line("B", spin=0x7FFFFFFF, mode=MODE_POLL)
        for k in range(0, 16, 4):
            T.store_word(lines["B"] + L["poll"] + k, 0xEEEEEEEE)
        ready(s1, wb, 1)
        T.wait_word(lines["B"] + L["inside"], 1)
        T.sleep_us(50000)                       # B holds the only SPU: nothing idles
        T.record_mem("s1 instance 0x00-0xFF while B runs (%s)" % why, s1, 0x100)
        kick()
        T.wait_word(lines["A"] + L["count"], 1, cond="geu")
        # B yielded to A, and cannot run again while A (higher priority) is
        # ready: take its answer now. Only selectWorkload's result -- the
        # preferred doubleword {wid, pollStatus} -- is compared; r3's other
        # two words are whatever the kernel's select code left there.
        T.record_mem("B's poll answer {wid, pollStatus} when A wanted the SPU (%s)" % why,
                     lines["B"] + L["poll"], 8)
        set_line("B")
        ready(s1, wa, 0); ready(s1, wb, 0)
        quiesce("A", "B")
        T.record_nonzero("A ran after B's poll (%s)" % why, lines["A"] + L["count"])
    T.settle(s1, 1)
    T.record_mem("s1 instance 0x00-0xFF after polls", s1, 0x100)
    remove_all(s1, (("A", wa), ("B", wb), ("C", wc), ("D", wd)), "s1")

    # ---- two SPUs: idle-SPU requests ------------------------------------------------
    s2 = T.spurs_init(nspus=2, name="s2")
    we = add(s2, "E", prio("e", [8] * 8), minc=1, maxc=2)
    set_line("E", spin=20000)
    ready(s2, we, 1)
    T.call("cellSpursRequestIdleSpu", s2, we, 1, rc="RequestIdleSpu 1")
    T.wait_word(lines["E"] + L["saw2"], 0, cond="ne")
    ready(s2, we, 0)
    quiesce("E")
    T.record_nonzero("readyCount 1 + idle request 1: two SPUs at once", lines["E"] + L["saw2"])
    T.call("cellSpursRequestIdleSpu", s2, we, 0, rc="RequestIdleSpu 0")
    set_line("E", spin=20000)
    ready(s2, we, 1)
    T.sleep_us(300000)
    ready(s2, we, 0)
    quiesce("E")
    T.record_nonzero("readyCount 1, no idle request: ran", lines["E"] + L["count"])
    T.record_nonzero("readyCount 1, no idle request: two SPUs at once", lines["E"] + L["saw2"])
    T.settle(s2, 2)
    T.record_mem("s2 instance 0x00-0xFF", s2, 0x100)
    remove_all(s2, (("E", we),), "s2")

    # ---- exitIfNoWork: the group leaves when idle; WakeUp restarts it ------------------
    s3 = T.spurs_init(nspus=1, name="s3", exit_if_no_work=1)
    T.sleep_us(100000)
    T.record_mem("s3 instance 0x00-0xFF, no workloads", s3, 0x100)
    T.record_mem("s3 handler bytes 0xD64..0xD67", s3 + 0xD64, 4)
    wf = add(s3, "F", prio("f", [8] * 8))
    ready(s3, wf, 1)
    T.sleep_us(200000)
    T.record_nonzero("exitIfNoWork: ran on readyCount alone", lines["F"] + L["count"])
    T.call("cellSpursWakeUp", s3, rc="WakeUp")
    T.wait_word(lines["F"] + L["count"], 1, cond="geu", tries=1000)
    ready(s3, wf, 0)
    quiesce("F")
    T.record_nonzero("exitIfNoWork: ran after WakeUp", lines["F"] + L["count"])
    T.sleep_us(200000)
    T.record_mem("s3 instance 0x00-0xFF, idle again", s3, 0x100)
    T.record_mem("s3 handler bytes 0xD64..0xD67, idle again", s3 + 0xD64, 4)
    set_line("F")
    T.call("cellSpursSendWorkloadSignal", s3, wf, rc="signal F")
    T.sleep_us(200000)
    T.record_mem("exitIfNoWork: dispatches for a signal without WakeUp", lines["F"] + L["count"], 4)
    T.call("cellSpursWakeUp", s3, rc="WakeUp again")
    T.wait_word(lines["F"] + L["count"], 1, cond="geu", tries=1000)
    quiesce("F")
    T.record_mem("exitIfNoWork: dispatches for a signal after WakeUp", lines["F"] + L["count"], 4)
    remove_all(s3, (("F", wf),), "s3")
