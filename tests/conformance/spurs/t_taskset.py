"""SPURS tasksets against the firmware's libsre: taskset and task attributes,
creating tasksets (legacy and taskset2) and tasks, the LS-pattern helpers,
the taskset structure as libsre maintains it, and tasks running under the
real taskset policy module -- task exit and exit codes, signals, yield --
with shutdown and join.

Task programs are hand-written SPU code loaded at LS 0x3000 (above the
taskset policy module). A task's argument quadword arrives in r3; the
taskset syscalls go through the policy module's entry at LS 0x27C4
(r3 = syscall number, r4 = argument): 0 EXIT, 1 YIELD, 2 WAIT_SIGNAL. The
exit code is read from word 0 of LS 0x2FD0, not from r4. The syscall is an
ordinary call, so r3-r79 do not survive it; a task keeps its state in r80
and up. A task enters on the kernel's stack and, like a task's crt0, moves
to the top of LS. A task that waits or yields needs a context save area and
an LS pattern covering its code and its stack."""
import struct
from spurs_lib import P, S, SDK_VERSION, SYS

TASK_BASE = 0x3000
SC_EXIT, SC_YIELD, SC_WAIT_SIGNAL = 0, 1, 2


def task_asm():
    a = S.SpuAsm(TASK_BASE)
    # A task enters on the kernel's stack (r1 = 0x2C30); like a task's crt0, move to
    # the top of LS, so the stack lies in the blocks its LS pattern saves.
    a.ila(1, 0x3FFF0); a.il(2, 0); a.stqd(2, 1, 0)
    a.il(80, 0)                                    # EAH = 0
    a.ila(81, 0x30000)                             # LS scratch line
    return a


def atomic_add(a, r_ea, r_val, lab):
    """Add the quadword r_val to the first quadword of the line at EA r_ea."""
    a.label(lab)
    a.wrch(S.MFC_LSA, 81); a.wrch(S.MFC_EAH, 80); a.wrch(S.MFC_EAL, r_ea)
    a.il(12, S.GETLLAR); a.wrch(S.MFC_Cmd, 12); a.rdch(12, S.MFC_RdAtomicStat)
    a.lqd(21, 81, 0); a.a(21, 21, r_val); a.stqd(21, 81, 0)
    a.wrch(S.MFC_LSA, 81); a.wrch(S.MFC_EAH, 80); a.wrch(S.MFC_EAL, r_ea)
    a.il(12, S.PUTLLC); a.wrch(S.MFC_Cmd, 12); a.rdch(13, S.MFC_RdAtomicStat)
    a.brnz(13, lab)


def word_vec(a, rt, slot, value, scratch=8):
    """rt = a quadword with `value` in word `slot` and zero elsewhere."""
    a.il(rt, value)                                # value in every word
    a.fsmbi(scratch, 0xF000 >> (4 * slot))         # keep word `slot`
    a.and_(rt, rt, scratch)


def task_exit(a, r_code):
    """Exit with the preferred word of r_code: the policy module takes a task's exit
    code from word 0 of LS 0x2FD0 (in the taskset kernel context), not from r4."""
    a.lqa(15, 0x2FD0); a.fsmbi(16, 0xF000); a.selb(15, 15, r_code, 16); a.stqa(15, 0x2FD0)
    syscall(a, SC_EXIT, r4=r_code)


def syscall(a, num, r4=None, imm=0):
    a.lqa(5, 0x27C0); a.rotqbyi(5, 5, 4)           # syscall entry (LS 0x27C4)
    if r4 is None:
        a.il(4, imm)
    else:
        a.ori(4, r4, 0)
    a.il(3, num)
    a.bisl(0, 5)


def adder_task():
    """arg {line EA, index}: line += {index + 1, 1, 0, 0}; exit(0x100 + index)."""
    a = task_asm()
    a.ori(82, 3, 0)                                # keep the argument
    a.rotqbyi(6, 3, 4); a.ai(6, 6, 1)              # index + 1 -> preferred
    a.fsmbi(8, 0xF000); a.and_(6, 6, 8)
    word_vec(a, 7, 1, 1); a.or_(6, 6, 7)
    atomic_add(a, 82, 6, "add")
    a.rotqbyi(14, 82, 4); a.ai(14, 14, 0x100)      # exit code
    task_exit(a, 14)
    a.stop(0x3FF)
    return a.bytes()


def sigwait_task():
    """arg {line EA}: line += {0, 0, 1, 0}; wait for a signal;
    line += {0, 0, 0, 1}; exit(0x55)."""
    a = task_asm()
    a.ori(82, 3, 0)
    word_vec(a, 6, 2, 1)
    atomic_add(a, 82, 6, "started")
    syscall(a, SC_WAIT_SIGNAL)
    a.fsmbi(8, 0xF000); a.and_(6, 3, 8)            # {rc, 0, 0, 0}
    word_vec(a, 7, 3, 1); a.or_(6, 6, 7)           # + {0, 0, 0, 1}
    atomic_add(a, 82, 6, "signalled")
    a.il(14, 0x55); task_exit(a, 14)
    a.stop(0x3FF)
    return a.bytes()


def yield_task():
    """arg {line EA}: three times: line += {0, 1, 0, 0}, yield; exit(3)."""
    a = task_asm()
    a.ori(82, 3, 0)
    a.il(83, 3)
    a.label("loop")
    word_vec(a, 6, 1, 1)
    atomic_add(a, 82, 6, "bump")
    syscall(a, SC_YIELD)
    a.ai(83, 83, -1)
    a.brnz(83, "loop")
    a.il(14, 3); task_exit(a, 14)
    a.stop(0x3FF)
    return a.bytes()


def body(T):
    spurs = T.spurs_init(nspus=2, spu_prio=100, ppu_prio=1000)
    adder = T.spu_elf("adder", adder_task(), TASK_BASE)
    sigw = T.spu_elf("sigwait", sigwait_task(), TASK_BASE)
    yld = T.spu_elf("yield", yield_task(), TASK_BASE)
    prio = T.alloc("prio", 16, 16, bytes([1, 1, 1, 1, 1, 1, 1, 1]))
    badprio = T.alloc("badprio", 16, 16, bytes([1, 16, 1, 1, 1, 1, 1, 1]))
    name = T.alloc("tsname", 16, 16, b"TasksetT\0")

    # ---- taskset attribute ------------------------------------------------------------
    attr = T.alloc("tsattr", 512, 8)
    T.call("_cellSpursTasksetAttributeInitialize", attr, 1, SDK_VERSION, 0x1122334455667788,
           prio, 2, rc="TasksetAttributeInitialize")
    T.record_mem("taskset attribute", attr, 128)
    T.call("_cellSpursTasksetAttributeInitialize", 0, 1, SDK_VERSION, 0, prio, 2,
           rc="TasksetAttributeInitialize null")
    T.call("_cellSpursTasksetAttributeInitialize", attr + 4, 1, SDK_VERSION, 0, prio, 2,
           rc="TasksetAttributeInitialize misaligned")
    T.call("_cellSpursTasksetAttributeInitialize", attr, 1, SDK_VERSION, 0, badprio, 2,
           rc="TasksetAttributeInitialize priority 16")
    T.call("_cellSpursTasksetAttributeInitialize", attr, 1, SDK_VERSION, 0, prio, 0,
           rc="TasksetAttributeInitialize maxContention 0")
    T.call("_cellSpursTasksetAttributeInitialize", attr, 1, SDK_VERSION, 0x1122334455667788,
           prio, 2, rc=False)
    T.call("cellSpursTasksetAttributeSetName", attr, name, rc="TasksetAttributeSetName")
    T.call("cellSpursTasksetAttributeSetName", attr, 0, rc="TasksetAttributeSetName null")
    for size in (6400, 10496, 4096):
        T.call("cellSpursTasksetAttributeSetTasksetSize", attr, size,
               rc="TasksetAttributeSetTasksetSize %d" % size)
    T.call("cellSpursTasksetAttributeSetTasksetSize", attr, 6400, rc=False)
    for en in (0, 1, 2):
        T.call("cellSpursTasksetAttributeEnableClearLS", attr, en,
               rc="TasksetAttributeEnableClearLS %d" % en)
    T.call("cellSpursTasksetAttributeEnableClearLS", attr, 0, rc=False)
    T.record_mem("taskset attribute after setters", attr, 128)

    # ---- create ---------------------------------------------------------------------------
    ts = T.alloc("taskset", 6400, 128)
    T.call("cellSpursCreateTasksetWithAttribute", 0, ts, attr, rc="CreateTasksetWithAttribute null spurs")
    T.call("cellSpursCreateTasksetWithAttribute", spurs, 0, attr, rc="CreateTasksetWithAttribute null taskset")
    T.call("cellSpursCreateTasksetWithAttribute", spurs, ts + 64, attr,
           rc="CreateTasksetWithAttribute misaligned taskset")
    T.call("cellSpursCreateTasksetWithAttribute", spurs, ts, attr, rc="CreateTasksetWithAttribute")
    T.record_mem("taskset 0x00-0x7F after create", ts, 0x80)
    T.record_mem("taskset 0x1880-0x18FF after create", ts + 0x1880, 0x80)
    wid = T.alloc("wid", 16, 16, b"\xee" * 16)
    T.call("cellSpursGetTasksetId", ts, wid, rc="GetTasksetId")
    T.record_mem("taskset wid", wid, 4)
    out = T.alloc("out", 16, 16, b"\xee" * 16)
    T.call("cellSpursTasksetGetSpursAddress", ts, out, rc="TasksetGetSpursAddress")
    T.record_mem("taskset spurs address", out, 4)
    T.call("cellSpursLookUpTasksetAddress", spurs, out, ("mem", wid), rc="LookUpTasksetAddress")
    T.record_mem("looked-up taskset address", out, 4)
    T.call("cellSpursLookUpTasksetAddress", spurs, out, 31, rc="LookUpTasksetAddress bad wid")

    # ---- LS patterns --------------------------------------------------------------------
    pat = T.alloc("pattern", 16, 16, b"\xee" * 16)
    T.call("cellSpursTaskGetLoadableSegmentPattern", pat, adder, rc="TaskGetLoadableSegmentPattern")
    T.record_mem("loadable segment pattern", pat, 16)
    T.call("cellSpursTaskGetReadOnlyAreaPattern", pat, adder, rc="TaskGetReadOnlyAreaPattern")
    T.record_mem("read-only area pattern", pat, 16)
    T.call("cellSpursTaskGenerateLsPattern", pat, 0x3000, 0x2000, rc="TaskGenerateLsPattern")
    T.record_mem("generated LS pattern 0x3000+0x2000", pat, 16)
    csz = T.alloc("ctxsize", 16, 16, b"\xee" * 16)
    T.call("cellSpursTaskGetContextSaveAreaSize", csz, pat, rc="TaskGetContextSaveAreaSize")
    T.record_mem("context save area size", csz, 4)

    # ---- tasks: validation ----------------------------------------------------------------
    line = T.alloc("addline", 128, 128)
    tids = T.alloc("tids", 64, 16, b"\xee" * 64)
    targs = T.alloc("targs", 16 * 8, 16)
    for i in range(8):
        T.store_word(targs + 16 * i, line)
        T.store_word(targs + 16 * i + 4, i)
    T.call("cellSpursCreateTask", ts, 0, adder, 0, 0, 0, targs, rc="CreateTask null id")
    T.call("cellSpursCreateTask", ts, tids, 0, 0, 0, 0, targs, rc="CreateTask null elf")
    T.call("cellSpursCreateTask", ts, tids, adder + 4, 0, 0, 0, targs, rc="CreateTask misaligned elf")
    T.call("cellSpursCreateTask", 0, tids, adder, 0, 0, 0, targs, rc="CreateTask null taskset")
    T.call("cellSpursCreateTask", ts, tids, adder, line, 0, 0, targs,
           rc="CreateTask context without size")

    # ---- tasks: four adders run to exit ---------------------------------------------------
    for i in range(4):
        T.call("cellSpursCreateTask", ts, tids + 4 * i, adder, 0, 0, 0, targs + 16 * i,
               rc="CreateTask adder %d" % i)
    T.record_mem("task ids", tids, 16)
    T.wait_word(line + 4, 4)
    T.record_mem("line after four adders {sum, exits}", line, 16)
    T.settle(spurs, 2)
    T.record_mem("taskset bitsets after the adders", ts, 0x60)
    T.record_mem("task info 0 after exit", ts + 0x80, 48)

    # ---- a task waiting for a signal --------------------------------------------------
    line2 = T.alloc("line2", 128, 128)
    sarg = T.alloc("sarg", 16, 16)
    T.store_word(sarg, line2)
    # A task's context is saved only if its LS pattern covers every 2 KB block from
    # its stack pointer to the top of LS: the code block (0x3000) and the top 8 KB.
    blocks = [TASK_BASE // 0x800] + list(range(124, 128))
    pbits = bytearray(16)
    for blk in blocks:
        pbits[blk // 8] |= 0x80 >> (blk % 8)
    spat = T.alloc("savepattern", 16, 16, bytes(pbits))
    T.call("cellSpursTaskGetContextSaveAreaSize", csz, spat, rc="TaskGetContextSaveAreaSize (save pattern)")
    T.record_mem("context save area size (save pattern)", csz, 4)
    CTX = 0x400 + 0x800 * len(blocks)            # what libsre reports for the pattern (0x2C00)
    sctx = T.alloc("sctx", CTX, 128)
    T.call("cellSpursCreateTask", ts, tids + 0x10, sigw, sctx, CTX, spat, sarg,
           rc="CreateTask signal waiter")
    T.wait_word(line2 + 8, 1)
    T.settle(spurs, 2)
    T.record_mem("signal waiter started, not signalled", line2, 16)
    T.record_mem("taskset bitsets, waiter waiting", ts, 0x60)
    T.call("_cellSpursSendSignal", ts, ("mem", tids + 0x10), rc="SendSignal")
    T.wait_word(line2 + 12, 1)
    T.record_mem("signal waiter signalled", line2, 16)
    T.call("_cellSpursSendSignal", ts, 127, rc="SendSignal to an absent task")

    # ---- a task that yields -------------------------------------------------------------
    line3 = T.alloc("line3", 128, 128)
    yarg = T.alloc("yarg", 16, 16)
    T.store_word(yarg, line3)
    yctx = T.alloc("yctx", CTX, 128)
    T.call("cellSpursCreateTask", ts, tids + 0x14, yld, yctx, CTX, spat, yarg, rc="CreateTask yielder")
    T.wait_word(line3 + 4, 3)
    T.settle(spurs, 2)
    T.record_mem("yielder ran three rounds", line3, 16)

    T.call("cellSpursShutdownTaskset", ts, rc="ShutdownTaskset")
    T.call("cellSpursJoinTaskset", ts, rc="JoinTaskset")
    T.call("cellSpursShutdownTaskset", ts, rc="ShutdownTaskset again")
    T.call("cellSpursJoinTaskset", ts, rc="JoinTaskset again")
    T.record_mem("taskset bitsets after join", ts, 0x60)

    # ---- taskset2: exit codes and joining tasks -------------------------------------------
    attr2 = T.alloc("tsattr2", 512, 8)
    T.call("_cellSpursTasksetAttribute2Initialize", attr2, 1, rc=False)     # void
    T.record_mem("taskset2 attribute", attr2, 128)
    ts2 = T.alloc("taskset2", 10496, 128)
    T.call("cellSpursCreateTaskset2", spurs, ts2, attr2, rc="CreateTaskset2")
    # x78 is an address inside libsre's data segment: the same offset on both, but the
    # module sits elsewhere (docs/KNOWN_DIFFERENCES.md, "one fixed load address")
    T.record_mem("taskset2 0x00-0x7F after create", ts2, 0x80, ignore=((0x78, 8),))
    tattr2 = T.alloc("tattr2", 512, 8)
    # CELL_SPURS_TASK2_REVISION is 0; CreateTask2 refuses any other revision
    T.call("_cellSpursTaskAttribute2Initialize", tattr2, 1, rc=False)        # void
    T.record_mem("task attribute2", tattr2, 64)
    line4 = T.alloc("line4", 128, 128)
    t2args = T.alloc("t2args", 32, 16)
    T.store_word(t2args, line4); T.store_word(t2args + 4, 5)
    T.store_word(t2args + 16, line4)
    tid2 = T.alloc("tid2", 16, 16, b"\xee" * 16)
    T.call("cellSpursCreateTask2", ts2, tid2, adder, t2args, tattr2, rc="CreateTask2 revision 1")
    T.call("_cellSpursTaskAttribute2Initialize", tattr2, 0, rc=False)
    T.record_mem("task attribute2 revision 0", tattr2, 64)
    T.call("cellSpursCreateTask2", ts2, tid2, adder, t2args, tattr2, rc="CreateTask2 adder")
    # the waiter's attribute: {revision, sizeContext, eaContext, lsPattern, name}
    tattr2c = T.alloc("tattr2c", 512, 8)
    T.call("_cellSpursTaskAttribute2Initialize", tattr2c, 0, rc=False)
    s2ctx = T.alloc("s2ctx", CTX, 128)
    T.store_word(tattr2c + 4, CTX)
    T.store_word(tattr2c + 12, s2ctx)
    T.emit(P.li32(3, tattr2c + 16), P.li32(4, spat), *[x for k in range(4)
           for x in (P.lwz(5, 4, 4 * k), P.stw(5, 3, 4 * k))])
    T.call("cellSpursCreateTask2", ts2, tid2 + 4, sigw, t2args + 16, tattr2c,
           rc="CreateTask2 signal waiter")
    T.record_mem("task2 ids", tid2, 8)
    code = T.alloc("exitcode", 16, 16, b"\xee" * 16)
    T.call("cellSpursJoinTask2", ts2, ("mem", tid2), code, rc="JoinTask2 adder")
    T.record_mem("adder exit code", code, 4)
    T.wait_word(line4 + 8, 1)
    T.call("cellSpursTryJoinTask2", ts2, ("mem", tid2 + 4), code + 4, rc="TryJoinTask2 waiting task")
    T.call("_cellSpursSendSignal", ts2, ("mem", tid2 + 4), rc="SendSignal (taskset2)")
    T.call("cellSpursJoinTask2", ts2, ("mem", tid2 + 4), code + 8, rc="JoinTask2 signal waiter")
    T.record_mem("exit codes {adder, -, signal waiter}", code, 12, ignore=((4, 4),))
    T.call("cellSpursJoinTask2", ts2, ("mem", tid2), code, rc="JoinTask2 already joined")
    T.call("cellSpursShutdownTaskset", ts2, rc="ShutdownTaskset2")
    T.call("cellSpursJoinTaskset", ts2, rc="JoinTaskset2")

    T.call("cellSpursFinalize", spurs, rc="Finalize")
