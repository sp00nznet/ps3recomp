"""SPURS event flags against the firmware's libsre, from the PPU side:
initialization (taskset-bound and IWL) and its validation, the getters,
Set/Clear/TryWait under each wait mode, clear mode and direction, attaching
and detaching the lv2 event queue a PPU waiter blocks on, and a PPU thread
blocking in Wait until another thread sets the bits it waits for.

The SPU side (a task waiting on or setting a flag) needs the SPU SPURS
library, which a hand-written task does not have; it is not covered.

Records made inside the waiter thread are stored to memory and recorded by
the main thread."""
from spurs_lib import P, SYS

OR, AND = 0, 1
CLEAR_AUTO, CLEAR_MANUAL = 0, 1
SPU2SPU, SPU2PPU, PPU2SPU, ANY2ANY = 0, 1, 2, 3


def body(T):
    spurs = T.spurs_init(nspus=2, spu_prio=100, ppu_prio=1000)
    prio = T.alloc("prio", 16, 16, bytes([1, 1, 1, 1, 1, 1, 1, 1]))
    ts = T.alloc("taskset", 6400, 128)
    T.call("cellSpursCreateTaskset", spurs, ts, 0, prio, 1, rc="CreateTaskset")

    def init(ef, clear, direction, label, iwl=False):
        T.call("_cellSpursEventFlagInitialize", spurs if iwl else 0, 0 if iwl else ts, ef,
               clear, direction, rc=label)

    # ---- initialize: validation -----------------------------------------------------------
    ef = T.alloc("ef", 128, 128)
    T.call("_cellSpursEventFlagInitialize", 0, 0, ef, 0, 0, rc="Initialize no taskset, no spurs")
    T.call("_cellSpursEventFlagInitialize", 0, ts, 0, 0, 0, rc="Initialize null flag")
    T.call("_cellSpursEventFlagInitialize", 0, ts, ef + 16, 0, 0, rc="Initialize misaligned flag")
    T.call("_cellSpursEventFlagInitialize", 0, ts + 16, ef, 0, 0, rc="Initialize misaligned taskset")
    T.call("_cellSpursEventFlagInitialize", spurs + 16, 0, ef, 0, 0, rc="Initialize misaligned spurs")
    T.call("_cellSpursEventFlagInitialize", 0, ts, ef, 2, 0, rc="Initialize clear mode 2")
    T.call("_cellSpursEventFlagInitialize", 0, ts, ef, 0, 4, rc="Initialize direction 4")
    T.call("_cellSpursEventFlagInitialize", spurs, ts, ef, 0, 0, rc="Initialize spurs and taskset")

    # ---- initialize: every direction and clear mode, the structure and the getters ---------
    out = T.alloc("out", 16, 16)
    for iwl in (False, True):
        for clear in (CLEAR_AUTO, CLEAR_MANUAL):
            for d in (SPU2SPU, SPU2PPU, PPU2SPU, ANY2ANY):
                tag = "%s clear %d direction %d" % ("IWL" if iwl else "taskset", clear, d)
                init(ef, clear, d, "Initialize " + tag, iwl)
                T.record_mem("flag after Initialize " + tag, ef, 128)
    init(ef, CLEAR_MANUAL, SPU2PPU, "Initialize for the getters")
    T.store_word(out, 0xEEEEEEEE)
    T.call("cellSpursEventFlagGetDirection", ef, out, rc="GetDirection")
    T.record_mem("direction", out, 4)
    T.call("cellSpursEventFlagGetClearMode", ef, out, rc="GetClearMode")
    T.record_mem("clear mode", out, 4)
    T.call("cellSpursEventFlagGetTasksetAddress", ef, out, rc="GetTasksetAddress")
    T.record_mem("taskset address", out, 4)
    init(ef, CLEAR_MANUAL, SPU2PPU, "Initialize IWL for the getters", iwl=True)
    T.store_word(out, 0xEEEEEEEE)
    T.call("cellSpursEventFlagGetTasksetAddress", ef, out, rc="GetTasksetAddress IWL")
    T.record_mem("taskset address IWL", out, 4)
    for fn in ("cellSpursEventFlagGetDirection", "cellSpursEventFlagGetClearMode",
               "cellSpursEventFlagGetTasksetAddress"):
        short = fn[len("cellSpursEventFlag"):]
        T.call(fn, 0, out, rc=short + " null flag")
        T.call(fn, ef, 0, rc=short + " null out")
        T.call(fn, ef + 16, out, rc=short + " misaligned flag")

    # ---- set / clear / try-wait --------------------------------------------------------------
    mask = T.alloc("mask", 16, 16)

    def trywait(bits, mode, label):
        T.emit(P.li32(3, mask), P.li32(4, bits), P.sth(4, 3, 0))
        T.call("cellSpursEventFlagTryWait", ef, mask, mode, rc="TryWait " + label)
        T.record_mem("TryWait %s: mask" % label, mask, 4)
        T.record_mem("TryWait %s: flag" % label, ef, 16)

    init(ef, CLEAR_AUTO, ANY2ANY, "Initialize ANY2ANY auto")
    T.call("cellSpursEventFlagSet", ef, 0x0005, rc="Set 0x0005")
    T.record_mem("flag after Set 0x0005", ef, 16)
    trywait(0x0003, AND, "AND 0x0003 (not all set)")
    trywait(0x0006, OR, "OR 0x0006 (auto clear)")
    trywait(0x0004, OR, "OR 0x0004 (cleared)")
    trywait(0x0001, AND, "AND 0x0001")
    T.call("cellSpursEventFlagSet", ef, 0xFFFF, rc="Set 0xFFFF")
    trywait(0x0000, OR, "OR mask 0")
    trywait(0x8001, 2, "mode 2")
    T.call("cellSpursEventFlagTryWait", ef, 0, OR, rc="TryWait null mask")
    T.call("cellSpursEventFlagTryWait", 0, mask, OR, rc="TryWait null flag")
    T.call("cellSpursEventFlagClear", ef, 0x00F0, rc="Clear 0x00F0")
    T.record_mem("flag after Clear 0x00F0", ef, 16)
    T.call("cellSpursEventFlagClear", 0, 1, rc="Clear null flag")
    T.call("cellSpursEventFlagSet", 0, 1, rc="Set null flag")
    T.call("cellSpursEventFlagSet", ef + 16, 1, rc="Set misaligned flag")

    init(ef, CLEAR_MANUAL, SPU2PPU, "Initialize SPU2PPU manual")
    T.call("cellSpursEventFlagSet", ef, 0x0030, rc="Set 0x0030 (SPU2PPU)")
    trywait(0x0010, OR, "OR 0x0010 (manual clear)")
    trywait(0x0030, AND, "AND 0x0030 (manual clear)")
    T.call("cellSpursEventFlagClear", ef, 0x0010, rc="Clear 0x0010")
    trywait(0x0030, AND, "AND 0x0030 after clear")

    init(ef, CLEAR_AUTO, PPU2SPU, "Initialize PPU2SPU")
    T.call("cellSpursEventFlagSet", ef, 0x0001, rc="Set (PPU2SPU)")
    trywait(0x0001, OR, "OR (PPU2SPU)")
    init(ef, CLEAR_AUTO, SPU2SPU, "Initialize SPU2SPU")
    T.call("cellSpursEventFlagSet", ef, 0x0001, rc="Set (SPU2SPU)")
    trywait(0x0001, OR, "OR (SPU2SPU)")

    # ---- wait without and with the lv2 event queue -----------------------------------------
    init(ef, CLEAR_AUTO, ANY2ANY, "Initialize ANY2ANY for Wait")
    T.emit(P.li32(3, mask), P.li32(4, 1), P.sth(4, 3, 0))
    T.call("cellSpursEventFlagWait", ef, mask, OR, rc="Wait before attach")
    T.call("cellSpursEventFlagDetachLv2EventQueue", ef, rc="Detach before attach")
    T.call("cellSpursEventFlagAttachLv2EventQueue", ef, rc="AttachLv2EventQueue")
    # 0x78/0x7C are lv2 ids (port, queue): nonzero, but numbered by each kernel its own way
    T.record_mem("flag after attach", ef, 128, ignore=((0x78, 8),))
    T.record_nonzero("event queue id after attach", ef + 0x7C)
    T.call("cellSpursEventFlagAttachLv2EventQueue", ef, rc="AttachLv2EventQueue again")
    T.call("cellSpursEventFlagAttachLv2EventQueue", 0, rc="AttachLv2EventQueue null")

    init_ppu = T.alloc("ef_ppu2spu", 128, 128)
    T.call("_cellSpursEventFlagInitialize", 0, ts, init_ppu, CLEAR_AUTO, PPU2SPU,
           rc="Initialize PPU2SPU for attach")
    T.call("cellSpursEventFlagAttachLv2EventQueue", init_ppu, rc="AttachLv2EventQueue PPU2SPU")
    T.call("cellSpursEventFlagDetachLv2EventQueue", init_ppu, rc="DetachLv2EventQueue PPU2SPU")

    # ---- a PPU thread blocks in Wait(AND 0x0300) until both bits are set ------------------
    res = T.alloc("wres", 16, 16, b"\xee" * 16)    # the waiter's Wait rc
    wmask = T.alloc("wmask", 16, 16)
    done = T.alloc("done", 16, 16)

    def waiter(T):
        T.emit(P.li32(3, wmask), P.li32(4, 0x0300), P.sth(4, 3, 0))
        T.call("cellSpursEventFlagWait", ef, wmask, AND, rc=False)
        T.emit(P.li32(9, res), P.stw(3, 9, 0))
        T.store_word(done, 1)
    T.thread("waiter", waiter)
    tid = T.alloc("tid", 16, 16)
    tret = T.alloc("tret", 16, 16)
    T.start_thread("waiter", 0, tid)
    T.sleep_us(50000)
    T.record_mem("waiter returned before any Set (done)", done, 4)
    T.record_mem("flag with the PPU waiter blocked", ef, 16)
    T.emit(P.li32(3, mask), P.li32(4, 1), P.sth(4, 3, 0))
    T.call("cellSpursEventFlagTryWait", ef, mask, OR, rc="TryWait while a PPU thread waits")
    T.call("cellSpursEventFlagSet", ef, 0x0100, rc="Set 0x0100 (half the AND)")
    T.sleep_us(50000)
    T.record_mem("waiter returned after half the AND (done)", done, 4)
    T.call("cellSpursEventFlagSet", ef, 0x0201, rc="Set 0x0201 (completes it)")
    T.wait_word(done, 1)
    T.record_mem("waiter's Wait rc", res, 4)
    T.record_mem("waiter's mask", wmask, 4)
    T.record_mem("flag after the waiter woke", ef, 16)
    T.join_thread(tid, tret)

    T.call("cellSpursEventFlagDetachLv2EventQueue", ef, rc="DetachLv2EventQueue")
    T.record_mem("flag after detach", ef, 128, ignore=((0x78, 8),))
    T.call("cellSpursEventFlagDetachLv2EventQueue", ef, rc="DetachLv2EventQueue again")
    T.call("cellSpursEventFlagWait", ef, mask, OR, rc="Wait after detach")

    T.call("cellSpursShutdownTaskset", ts, rc="ShutdownTaskset")
    T.call("cellSpursJoinTaskset", ts, rc="JoinTaskset")
    T.call("cellSpursFinalize", spurs, rc="Finalize")
