"""SPURS workloads: AddWorkload / workload-attribute validation and layout,
the instance bytes a workload occupies, readyCount arithmetic, GetWorkloadInfo
and GetWorkloadData, the workload flag, the shutdown -> wait -> remove life
cycle (with its completion hook), wid allocation per instance, and what the
kernel hands a policy module: its context fields, poll status per wake-up
reason (readyCount / signal / flag), and how many SPUs run it at once."""
from spurs_lib import SDK_VERSION, P, S

PM_BASE = 0xA00
WL = dict(count=0, inside=4, saw2=8, spin=12, st_ready=16, st_signal=20, st_flag=24, snap=128)
SNAP_SPUNUM = 16 + 8          # snapshot = r4 quad, then LS 0x1C0..0x1EF; spuNum is LS 0x1C8
# Instance snapshots are taken once the SPUs have settled (each has serviced
# the system-service messages the last change raised and gone idle:
# T.settle), so the SPU kernel's own bytes -- sysSrvMessage, spuIdling,
# wklStatus1, sysSrvMsgUpdateWorkload -- are compared like the rest.


def pm_code():
    """Entered at LS 0xA00: r0 = exitToKernel, r3 = kernel context, r4 = the
    u64 workload data (the WL line's EA), r5 = poll status. Per dispatch it
    atomically adds {count+1, inside+1} and one count per poll-status bit,
    PUTs a snapshot of r4 and the kernel context (LS 0x1C0..0x1EF), then spins
    `spin` times watching `inside`: seeing 2 means another SPU runs this
    workload concurrently (saw2 += 1). Leaves with inside-1."""
    a = S.SpuAsm(PM_BASE)
    a.il(127, 0)
    a.rotqbyi(11, 4, 4)                         # EA (low word of the u64) -> preferred slot
    a.ila(10, 0x30000)                          # 128-byte line buffer
    a.ila(12, 0x30100)                          # snapshot buffer
    a.fsmbi(60, 0xF000)                         # word-0 mask

    def word(rt, k):                            # keep word 0 of rt, move it to word k
        a.and_(rt, rt, 60)
        if k:
            a.rotqbyi(rt, rt, 16 - 4 * k)

    # q1 = {status&1, (status>>1)&1, (status>>2)&1, 1}: poll-status bits seen
    a.andi(30, 5, 1); word(30, 0)
    a.rotmi(31, 5, 1); a.andi(31, 31, 1); word(31, 1)
    a.rotmi(32, 5, 2); a.andi(32, 32, 1); word(32, 2)
    a.il(33, 1); word(33, 3)
    a.or_(30, 30, 31); a.or_(30, 30, 32); a.or_(30, 30, 33)
    a.il(34, 1); a.fsmbi(61, 0xFF00); a.and_(34, 34, 61)     # q0 = {1, 1, 0, 0}

    def atomic(q0, q1):
        lab = "at%d" % len(a.w)
        a.label(lab)
        a.wrch(S.MFC_LSA, 10); a.wrch(S.MFC_EAH, 127); a.wrch(S.MFC_EAL, 11)
        a.il(50, S.GETLLAR); a.wrch(S.MFC_Cmd, 50); a.rdch(50, S.MFC_RdAtomicStat)
        a.lqd(51, 10, 0); a.a(51, 51, q0); a.stqd(51, 10, 0)
        a.lqd(52, 10, 16); a.a(52, 52, q1); a.stqd(52, 10, 16)
        a.wrch(S.MFC_LSA, 10); a.wrch(S.MFC_EAH, 127); a.wrch(S.MFC_EAL, 11)
        a.il(50, S.PUTLLC); a.wrch(S.MFC_Cmd, 50); a.rdch(53, S.MFC_RdAtomicStat)
        a.brnz(53, lab)

    atomic(34, 30)
    # snapshot: r4, then the kernel context words at LS 0x1C0..0x1EF
    a.stqd(4, 12, 0)
    for k, ls in enumerate((0x1C0, 0x1D0, 0x1E0)):
        a.lqa(40, ls); a.stqd(40, 12, 16 * (k + 1))
    a.ai(13, 11, WL["snap"])
    a.il(14, 64)
    a.mfc(S.PUT, 12, 13, 14, 3, 15)
    a.wait_tag(3, 15)
    # spin: n = line word 3, re-read each pass through GETLLAR
    a.wrch(S.MFC_LSA, 10); a.wrch(S.MFC_EAH, 127); a.wrch(S.MFC_EAL, 11)
    a.il(50, S.GETLLAR); a.wrch(S.MFC_Cmd, 50); a.rdch(50, S.MFC_RdAtomicStat)
    a.lqd(41, 10, 0); a.rotqbyi(42, 41, 12)
    a.label("spin")
    a.brz(42, "done")
    a.wrch(S.MFC_LSA, 10); a.wrch(S.MFC_EAH, 127); a.wrch(S.MFC_EAL, 11)
    a.il(50, S.GETLLAR); a.wrch(S.MFC_Cmd, 50); a.rdch(50, S.MFC_RdAtomicStat)
    a.lqd(41, 10, 0); a.rotqbyi(43, 41, 4)
    a.rotqbyi(49, 41, 12); a.brz(49, "done")                   # the PPU released it
    a.cgti(44, 43, 1); a.brnz(44, "saw")
    a.ai(42, 42, -1); a.br("spin")
    a.label("saw")
    a.il(45, 1); a.fsmbi(62, 0x00F0); a.and_(45, 45, 62)     # {0, 0, 1, 0}
    atomic(45, 127)
    a.label("done")
    a.il(46, -1); a.fsmbi(63, 0x0F00); a.and_(46, 46, 63)    # {0, -1, 0, 0}
    atomic(46, 127)
    a.bi(0)
    return a.bytes()


def body(T):
    spurs = T.spurs_init(nspus=2)
    pm_b = pm_code()
    pm, npm = T.spu_raw("pm", pm_b), len(pm_b)
    wl = T.alloc("wl", 256, 128)
    wid, wid1, wid2 = T.alloc("wid", 16), T.alloc("wid1", 16), T.alloc("wid2", 16)
    prio = T.alloc("prio", 16, 16, bytes([8] * 8))
    prio_bad = T.alloc("prio_bad", 16, 16, bytes([16] * 8))
    out = T.alloc("out", 16, 8)
    hookrec = T.alloc("hookrec", 16, 16)

    def hook(T):
        T.emit(P.li32(9, hookrec), P.stw(3, 9, 0), P.stw(4, 9, 4), P.stw(5, 9, 8),
               P.lwz(10, 9, 12), P.addi(10, 10, 1), P.stw(10, 9, 12))
    T.func("shutdown_hook", hook)

    def info(label, w, ignore=()):
        buf = T.alloc(T.uniq("winfo"), 64, 16, b"\xee" * 64)
        T.call("cellSpursGetWorkloadInfo", spurs, w, buf, rc="GetWorkloadInfo rc: " + label)
        T.record_mem("GetWorkloadInfo: " + label, buf, 64, ignore=ignore)

    def quiesce():
        for _ in range(2):
            T.sleep_us(20000)
            T.wait_word(wl + WL["inside"], 0)

    def reset(spin=0):
        for k in range(0, 32, 4):
            T.store_word(wl + k, spin if k == WL["spin"] else 0)

    def statuses(label):
        T.record_nonzero(label + ": dispatched", wl + WL["count"])
        T.record_nonzero(label + ": saw READYCOUNT", wl + WL["st_ready"])
        T.record_nonzero(label + ": saw SIGNAL", wl + WL["st_signal"])
        T.record_nonzero(label + ": saw FLAG", wl + WL["st_flag"])

    # ---- AddWorkload argument validation --------------------------------------
    A = "cellSpursAddWorkload"
    T.call(A, 0, wid, pm, npm, wl, prio, 1, 1, rc="AddWorkload null spurs")
    T.call(A, spurs + 8, wid, pm, npm, wl, prio, 1, 1, rc="AddWorkload misaligned spurs")
    T.call(A, spurs, 0, pm, npm, wl, prio, 1, 1, rc="AddWorkload null wid")
    T.call(A, spurs, wid, 0, npm, wl, prio, 1, 1, rc="AddWorkload null pm")
    T.call(A, spurs, wid, pm + 8, npm, wl, prio, 1, 1, rc="AddWorkload misaligned pm")
    T.call(A, spurs, wid, pm, npm, wl, 0, 1, 1, rc="AddWorkload null priority")
    T.call(A, spurs, wid, pm, npm, wl, prio, 0, 1, rc="AddWorkload minContention 0")
    T.call(A, spurs, wid, pm, npm, wl, prio, 1, 0, rc="AddWorkload maxContention 0")
    T.call(A, spurs, wid, pm, npm, wl, prio, 2, 1, rc="AddWorkload min > max")
    T.call(A, spurs, wid, pm, npm, wl, prio, 9, 9, rc="AddWorkload min 9")
    T.call(A, spurs, wid, pm, npm, wl, prio_bad, 1, 1, rc="AddWorkload priority 16")
    T.settle(spurs, 2)
    T.record_mem("instance 0x00-0xFF after rejected adds", spurs, 0x100)

    # ---- a real workload ---------------------------------------------------------
    T.call(A, spurs, wid, pm, npm, wl, prio, 1, 2, rc="AddWorkload")
    T.record_mem("wid", wid, 4)
    T.settle(spurs, 2)
    T.record_mem("instance 0x00-0xFF after add", spurs, 0x100)
    T.record_mem("wklInfo1[0..7]", spurs + 0xB00, 256)
    info("fresh wid 0", ("mem", wid))
    T.call("cellSpursGetWorkloadData", spurs, out, ("mem", wid), rc="GetWorkloadData")
    T.record_mem("GetWorkloadData out", out, 8)
    T.call("cellSpursSetMaxContention", spurs, ("mem", wid), 8, rc="SetMaxContention 8")
    T.record_mem("maxContention bytes after SetMaxContention 8", spurs + 0x50, 16)
    T.call("cellSpursSetMaxContention", spurs, ("mem", wid), 2, rc="SetMaxContention 2")
    pr2 = T.alloc("prio2", 16, 16, bytes([1, 2, 3, 4, 5, 6, 7, 8]))
    T.call("cellSpursSetPriorities", spurs, ("mem", wid), pr2, rc="SetPriorities 1..8")
    T.record_mem("wklInfo1[0..7] after SetPriorities", spurs + 0xB00, 256)
    T.call("cellSpursSetPriorities", spurs, ("mem", wid), prio, rc="SetPriorities back to 8")

    # ---- workload attribute ---------------------------------------------------------
    wattr = T.alloc("wattr", 512, 8, b"\xee" * 512)
    W = "_cellSpursWorkloadAttributeInitialize"
    T.call(W, wattr, 1, SDK_VERSION, pm, npm, wl, prio, 1, stack=(1,), rc="WorkloadAttributeInitialize")
    T.record_mem("workload attribute after init", wattr, 128)
    cls = T.alloc("cls", 16, 16, b"ConfClass\0")
    ins = T.alloc("ins", 16, 16, b"ConfInst\0")
    T.call("cellSpursWorkloadAttributeSetName", wattr, cls, ins, rc="WorkloadAttributeSetName")
    hookarg = 0x00C0FFEE
    T.call("cellSpursWorkloadAttributeSetShutdownCompletionEventHook", wattr,
           T.opd_of("shutdown_hook"), hookarg, rc="SetShutdownCompletionEventHook")
    T.record_mem("workload attribute after SetName + hook", wattr, 128)
    T.call("cellSpursAddWorkloadWithAttribute", spurs, wid1, wattr, rc="AddWorkloadWithAttribute")
    T.record_mem("wid (with attribute)", wid1, 4)
    info("attribute workload", ("mem", wid1))
    wa2 = T.alloc("wattr2", 512, 8)
    T.call(W, 0, 1, SDK_VERSION, pm, npm, wl, prio, 1, stack=(1,), rc="WorkloadAttributeInitialize null")
    T.call(W, wa2 + 4, 1, SDK_VERSION, pm, npm, wl, prio, 1, stack=(1,), rc="WorkloadAttributeInitialize misaligned")
    T.call(W, wa2, 1, SDK_VERSION, 0, npm, wl, prio, 1, stack=(1,), rc="WorkloadAttributeInitialize null pm")
    T.call(W, wa2, 1, SDK_VERSION, pm + 8, npm, wl, prio, 1, stack=(1,), rc="WorkloadAttributeInitialize misaligned pm")
    T.call(W, wa2, 1, SDK_VERSION, pm, npm, wl, 0, 1, stack=(1,), rc="WorkloadAttributeInitialize null priority")
    T.call(W, wa2, 1, SDK_VERSION, pm, npm, wl, prio, 0, stack=(1,), rc="WorkloadAttributeInitialize min 0")
    T.call(W, wa2, 1, SDK_VERSION, pm, npm, wl, prio, 1, stack=(0,), rc="WorkloadAttributeInitialize max 0")
    T.call(W, wa2, 1, SDK_VERSION, pm, npm, wl, prio_bad, 1, stack=(1,), rc="WorkloadAttributeInitialize priority 16")
    T.call("cellSpursAddWorkloadWithAttribute", spurs, wid2, 0, rc="AddWorkloadWithAttribute null attr")
    T.call("cellSpursAddWorkloadWithAttribute", spurs, wid2, wattr + 4, rc="AddWorkloadWithAttribute misaligned attr")
    T.call("cellSpursAddWorkloadWithAttribute", spurs, 0, wattr, rc="AddWorkloadWithAttribute null wid")

    # ---- readyCount arithmetic (the PM runs meanwhile; it returns at once) ----------------
    w = ("mem", wid)
    T.call("cellSpursReadyCountStore", spurs, w, 3, rc="ReadyCountStore 3")
    T.record_mem("readyCount bytes after store 3", spurs, 16)
    T.call("cellSpursReadyCountAdd", spurs, w, out, 2, rc="ReadyCountAdd +2")
    T.record_mem("ReadyCountAdd +2 old", out, 4)
    T.call("cellSpursReadyCountAdd", spurs, w, out, -10, rc="ReadyCountAdd -10")
    T.record_mem("ReadyCountAdd -10 old", out, 4)
    T.record_mem("readyCount bytes after -10", spurs, 16)
    T.call("cellSpursReadyCountAdd", spurs, w, out, 300, rc="ReadyCountAdd +300")
    T.record_mem("readyCount bytes after +300", spurs, 16)
    T.call("cellSpursReadyCountSwap", spurs, w, out, 7, rc="ReadyCountSwap 7")
    T.record_mem("ReadyCountSwap old", out, 4)
    T.call("cellSpursReadyCountSwap", spurs, w, out, 256, rc="ReadyCountSwap 256")
    T.call("cellSpursReadyCountCompareAndSwap", spurs, w, out, 7, 9, rc="ReadyCountCAS 7->9")
    T.record_mem("ReadyCountCAS 7->9 old", out, 4)
    T.call("cellSpursReadyCountCompareAndSwap", spurs, w, out, 1, 4, rc="ReadyCountCAS 1->4 (fails)")
    T.record_mem("ReadyCountCAS 1->4 old", out, 4)
    T.call("cellSpursReadyCountCompareAndSwap", spurs, w, out, 9, 256, rc="ReadyCountCAS 9->256")
    T.record_mem("readyCount bytes after CAS", spurs, 16)
    T.call("cellSpursReadyCountAdd", spurs, w, out, 0, rc="ReadyCountAdd 0")
    T.call("cellSpursReadyCountStore", spurs, w, 0, rc="ReadyCountStore 0")
    quiesce()
    # held readyCount: the PM must be dispatched with READYCOUNT status
    reset()
    T.call("cellSpursReadyCountStore", spurs, w, 1, rc=False)
    T.wait_word(wl + WL["count"], 1, cond="geu")
    T.call("cellSpursReadyCountStore", spurs, w, 0, rc=False)
    quiesce()
    statuses("readyCount phase")
    T.record_mem("PM snapshot (r4, LS 0x1C0..0x1EF)", wl + WL["snap"], 64,
                 ignore=((SNAP_SPUNUM, 4),))   # which SPU took it varies
    T.settle(spurs, 2)
    T.record_mem("instance 0x00-0xFF after readyCount phase", spurs, 0x100)

    # ---- signal: one wake-up per signal ---------------------------------------------------
    reset()
    T.call("cellSpursSendWorkloadSignal", spurs, w, rc="SendWorkloadSignal")
    T.wait_word(wl + WL["count"], 1, cond="geu")
    T.sleep_us(50000)
    quiesce()
    T.record_mem("dispatches for one signal", wl + WL["count"], 4)
    statuses("signal phase")
    T.settle(spurs, 2)
    T.record_mem("wklSignal after dispatch", spurs + 0x70, 4)
    info("after signal", w)

    # ---- workload flag ------------------------------------------------------------------
    reset()
    F = "_cellSpursWorkloadFlagReceiver"
    T.call(F, spurs, w, 1, rc="FlagReceiver set")
    T.call(F, spurs, w, 1, rc="FlagReceiver set again")
    T.call(F, spurs, ("mem", wid1), 1, rc="FlagReceiver set by another wid")
    T.call(F, spurs, ("mem", wid1), 0, rc="FlagReceiver clear by non-owner")
    T.settle(spurs, 2)
    T.record_mem("wklFlag area after receiver set", spurs + 0x60, 32)
    T.store_word(spurs + 0x6C, 0)                          # raise the flag
    T.wait_word(wl + WL["count"], 1, cond="geu", tries=1000)
    T.sleep_us(50000)
    quiesce()
    T.record_mem("dispatches for the flag", wl + WL["count"], 4)
    statuses("flag phase")
    T.settle(spurs, 2)
    T.record_mem("wklFlag area after the flag", spurs + 0x60, 32)
    T.call(F, spurs, w, 0, rc="FlagReceiver clear")
    T.settle(spurs, 2)
    T.record_mem("wklFlag area after receiver clear", spurs + 0x60, 32)

    # ---- concurrency: how many SPUs run the workload at once ------------------------------
    reset(spin=20000)
    T.call("cellSpursReadyCountStore", spurs, w, 1, rc=False)      # maxContention 2, readyCount 1
    T.sleep_us(300000)
    T.call("cellSpursReadyCountStore", spurs, w, 0, rc=False)
    quiesce()
    T.record_nonzero("readyCount 1, maxContention 2: ran", wl + WL["count"])
    T.record_nonzero("readyCount 1, maxContention 2: two SPUs at once", wl + WL["saw2"])
    reset(spin=20000)
    T.call("cellSpursReadyCountStore", spurs, w, 2, rc=False)      # maxContention 2, readyCount 2
    T.wait_word(wl + WL["saw2"], 0, cond="ne")
    T.call("cellSpursReadyCountStore", spurs, w, 0, rc=False)
    quiesce()
    T.record_nonzero("readyCount 2, maxContention 2: two SPUs at once", wl + WL["saw2"])
    reset(spin=20000)
    T.call("cellSpursSetMaxContention", spurs, w, 1, rc=False)
    T.call("cellSpursReadyCountStore", spurs, w, 2, rc=False)      # maxContention 1, readyCount 2
    T.sleep_us(300000)
    T.call("cellSpursReadyCountStore", spurs, w, 0, rc=False)
    quiesce()
    T.record_nonzero("readyCount 2, maxContention 1: ran", wl + WL["count"])
    T.record_nonzero("readyCount 2, maxContention 1: two SPUs at once", wl + WL["saw2"])
    reset()

    # ---- life cycle ------------------------------------------------------------------------
    # The workload is kept on an SPU (its module spins) while it is shut down,
    # so the shutdown cannot complete until the module is released: a waiter
    # on another thread arrives before completion, deterministically.
    wrc, tid, tret = T.alloc("wait_rc", 16), T.alloc("waiter_tid", 16), T.alloc("waiter_ret", 16)

    def waiter(T):
        T.call("cellSpursWaitForWorkloadShutdown", spurs, ("mem", wid), rc=False)
        T.emit(P.li32(9, wrc), P.stw(3, 9, 0))
    T.thread("waiter", waiter)
    T.call("cellSpursRemoveWorkload", spurs, w, rc="RemoveWorkload while runnable")
    reset(spin=0x7FFFFFFF)
    T.call("cellSpursReadyCountStore", spurs, w, 1, rc=False)
    T.wait_word(wl + WL["inside"], 1)
    T.call("cellSpursShutdownWorkload", spurs, w, rc="ShutdownWorkload")
    T.call("cellSpursShutdownWorkload", spurs, w, rc="ShutdownWorkload again")
    T.call("cellSpursReadyCountStore", spurs, w, 1, rc="ReadyCountStore after shutdown")
    T.call("cellSpursSendWorkloadSignal", spurs, w, rc="SendWorkloadSignal after shutdown")
    T.call("cellSpursGetWorkloadData", spurs, out, w, rc="GetWorkloadData after shutdown")
    info("after shutdown, module still running", w)
    T.store_word(wrc, 0xEEEEEEEE)
    T.start_thread("waiter", 0, tid)
    T.sleep_us(50000)
    T.record_mem("wklState1 while shutting down, waiter blocked", spurs + 0x80, 16)
    T.record_mem("wklEvent1 while shutting down, waiter blocked", spurs + 0xA0, 16)
    T.record_mem("Wait still blocked", wrc, 4)
    T.store_word(wl + WL["spin"], 0)                 # release the module
    T.join_thread(tid, tret)
    T.record_mem("WaitForWorkloadShutdown (waited for completion)", wrc, 4)
    T.settle(spurs, 2)
    T.record_mem("instance 0x00-0xFF after shutdown", spurs, 0x100)
    T.call("cellSpursWaitForWorkloadShutdown", spurs, w, rc="WaitForWorkloadShutdown again")
    T.call("cellSpursRemoveWorkload", spurs, w, rc="RemoveWorkload")
    T.call("cellSpursRemoveWorkload", spurs, w, rc="RemoveWorkload again")
    T.call("cellSpursShutdownWorkload", spurs, w, rc="ShutdownWorkload after remove")
    T.call("cellSpursGetWorkloadInfo", spurs, w, out, rc="GetWorkloadInfo after remove")
    T.settle(spurs, 2)
    T.record_mem("instance 0x00-0xFF after remove", spurs, 0x100)
    T.record_mem("wklInfo1[0..7] after remove", spurs + 0xB00, 256)
    # the attribute workload: its completion hook
    w1 = ("mem", wid1)
    T.call("cellSpursShutdownWorkload", spurs, w1, rc="ShutdownWorkload (hooked)")
    T.call("cellSpursWaitForWorkloadShutdown", spurs, w1, rc="WaitForWorkloadShutdown (hooked)")
    T.sleep_us(50000)
    T.record_mem("shutdown hook {spurs, wid, arg, calls}", hookrec, 16)
    T.call("cellSpursRemoveWorkload", spurs, w1, rc="RemoveWorkload (hooked)")

    # ---- wid allocation: reuse, the 16-workload limit, per instance -----------------------
    T.call(A, spurs, wid, pm, npm, wl, prio, 1, 1, rc="AddWorkload after removes")
    T.record_mem("wid after removes", wid, 4)
    ids = T.alloc("ids", 64, 16)
    for k in range(1, 16):
        T.call(A, spurs, ids + 4 * k, pm, npm, wl, prio, 1, 1, rc="AddWorkload #%d" % k)
    T.record_mem("wids 1..15", ids + 4, 60)
    T.call(A, spurs, wid2, pm, npm, wl, prio, 1, 1, rc="AddWorkload #17 (full)")
    spurs2 = T.spurs_init(nspus=1, name="spurs2")
    w2b = T.alloc("w2b", 16)
    T.call(A, spurs2, w2b, pm, npm, wl, prio, 1, 1, rc="AddWorkload on a second instance")
    T.record_mem("second instance wid", w2b, 4)
    T.call("cellSpursShutdownWorkload", spurs2, ("mem", w2b), rc="spurs2 ShutdownWorkload")
    T.call("cellSpursWaitForWorkloadShutdown", spurs2, ("mem", w2b), rc="spurs2 WaitForWorkloadShutdown")
    T.call("cellSpursRemoveWorkload", spurs2, ("mem", w2b), rc="spurs2 RemoveWorkload")
    T.call("cellSpursFinalize", spurs2, rc="spurs2 Finalize")
    # tear down: every shutdown completes before its Wait (no waiter marks)
    for k in range(16):
        x = ("mem", wid) if k == 0 else ("mem", ids + 4 * k)
        T.call("cellSpursShutdownWorkload", spurs, x, rc=False)
    T.settle(spurs, 2)
    for k in range(16):
        x = ("mem", wid) if k == 0 else ("mem", ids + 4 * k)
        T.call("cellSpursWaitForWorkloadShutdown", spurs, x, rc=False)
        T.call("cellSpursRemoveWorkload", spurs, x, rc="RemoveWorkload #%d" % k)
    T.settle(spurs, 2)
    T.record_mem("instance 0x00-0xFF at the end", spurs, 0x100)
    T.call("cellSpursFinalize", spurs, rc="Finalize")
