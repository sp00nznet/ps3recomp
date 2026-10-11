"""Lock-free queues against the firmware's libsre, from the PPU side: the
cellSync LFQueue and the SPURS LFQueue built on it.

cellSync: initialization and its validation (including re-initialization and
a queue whose memory is not zero), the structure for every direction, the
getters, a non-blocking FIFO round trip to full and back to empty, the
direction permissions, Clear, and blocking push/pop between PPU threads.

SPURS: _cellSpursLFQueueInitialize for a taskset and IWL, the taskset
address, attaching the lv2 event queue, a non-blocking round trip, and a PPU
thread blocking in pop until the main thread pushes.

The SPU side needs the SPU sync library and is not covered."""
from spurs_lib import P

SPU2SPU, SPU2PPU, PPU2SPU, ANY2ANY = 0, 1, 2, 3
ENTRY, DEPTH = 16, 4


def body(T):
    spurs = T.spurs_init(nspus=2, spu_prio=100, ppu_prio=1000)
    prio = T.alloc("prio", 16, 16, bytes([1, 1, 1, 1, 1, 1, 1, 1]))
    ts = T.alloc("taskset", 6400, 128)
    T.call("cellSpursCreateTaskset", spurs, ts, 0, prio, 1, rc="CreateTaskset")

    buf = T.alloc("qbuf", ENTRY * DEPTH, 128)
    src = T.alloc("src", ENTRY * 8, 16,
                  b"".join(bytes([0x10 * (k + 1) + j for j in range(ENTRY)]) for k in range(8)))
    dst = T.alloc("dst", ENTRY * 8, 16, b"\xee" * (ENTRY * 8))
    out = T.alloc("out", 16, 16)

    # ---- cellSync: initialize --------------------------------------------------------------
    q = T.alloc("q", 128, 128)
    init = "cellSyncLFQueueInitialize"
    T.call(init, 0, buf, ENTRY, DEPTH, ANY2ANY, 0, rc="Initialize null queue")
    T.call(init, q, 0, ENTRY, DEPTH, ANY2ANY, 0, rc="Initialize null buffer")
    T.call(init, q, buf, 0x4010, DEPTH, ANY2ANY, 0, rc="Initialize size 0x4010")
    T.call(init, q, buf, 24, DEPTH, ANY2ANY, 0, rc="Initialize size 24")
    T.call(init, q, buf, ENTRY, 0, ANY2ANY, 0, rc="Initialize depth 0")
    T.call(init, q, buf, ENTRY, 0x8000, ANY2ANY, 0, rc="Initialize depth 0x8000")
    T.call(init, q, buf, ENTRY, DEPTH, 4, 0, rc="Initialize direction 4")
    T.call(init, q + 16, buf, ENTRY, DEPTH, ANY2ANY, 0, rc="Initialize misaligned queue")
    T.call(init, q, buf + 8, ENTRY, DEPTH, ANY2ANY, 0, rc="Initialize misaligned buffer")
    dirty = T.alloc("qdirty", 128, 128, b"\x11" * 128)
    T.call(init, dirty, buf, ENTRY, DEPTH, ANY2ANY, 0, rc="Initialize memory not zero")
    T.record_mem("queue (memory not zero) after Initialize", dirty, 128)
    for d in (SPU2SPU, SPU2PPU, PPU2SPU, ANY2ANY):
        qd = T.alloc("qdir%d" % d, 128, 128)
        T.call(init, qd, buf, ENTRY, DEPTH, d, 0, rc="Initialize direction %d" % d)
        T.record_mem("queue after Initialize direction %d" % d, qd, 128)
    T.call(init, q, buf, ENTRY, DEPTH, ANY2ANY, 0, rc="Initialize ANY2ANY")
    T.call(init, q, buf, ENTRY, DEPTH, ANY2ANY, 0, rc="Initialize again, same parameters")
    T.call(init, q, buf, ENTRY, DEPTH - 1, ANY2ANY, 0, rc="Initialize again, other depth")
    T.call(init, q, buf, ENTRY, DEPTH, SPU2PPU, 0, rc="Initialize again, other direction")
    T.record_mem("queue after Initialize ANY2ANY", q, 128)

    def getters(qq, tag):
        for fn in ("cellSyncLFQueueSize", "cellSyncLFQueueDepth", "cellSyncLFQueueGetDirection",
                   "cellSyncLFQueueGetEntrySize", "_cellSyncLFQueueGetSignalAddress"):
            short = fn.split("LFQueue")[1]
            T.store_word(out, 0xEEEEEEEE)
            T.call(fn, qq, out, rc="%s %s" % (short, tag))
            T.record_mem("%s %s: value" % (short, tag), out, 4)

    getters(q, "(empty)")
    T.call("cellSyncLFQueueSize", 0, out, rc="Size null queue")
    T.call("cellSyncLFQueueSize", q, 0, rc="Size null out")
    T.call("cellSyncLFQueueSize", q + 16, out, rc="Size misaligned queue")

    # ---- cellSync: non-blocking round trip ---------------------------------------------------
    for k in range(DEPTH + 1):
        T.call("_cellSyncLFQueuePushBody", q, src + ENTRY * k, 0, rc="TryPush %d" % k)
    getters(q, "(full)")
    T.record_mem("queue full", q, 128)
    T.record_mem("buffer full", buf, ENTRY * DEPTH)
    for k in range(DEPTH + 1):
        T.call("_cellSyncLFQueuePopBody", q, dst + ENTRY * k, 0, rc="TryPop %d" % k)
    T.record_mem("popped entries", dst, ENTRY * (DEPTH + 1))
    T.record_mem("queue empty again", q, 128)
    T.call("_cellSyncLFQueuePushBody", q, src, 0, rc="TryPush before Clear")
    T.call("cellSyncLFQueueClear", q, rc="Clear")
    getters(q, "(after Clear)")
    T.record_mem("queue after Clear", q, 128)
    T.call("_cellSyncLFQueuePushBody", 0, src, 0, rc="TryPush null queue")
    T.call("_cellSyncLFQueuePushBody", q, 0, 0, rc="TryPush null buffer")
    T.call("_cellSyncLFQueuePushBody", q, src + 8, 0, rc="TryPush misaligned buffer")
    T.call("_cellSyncLFQueuePopBody", q, 0, 0, rc="TryPop null buffer")

    # the direction decides which side may push and pop
    for d in (SPU2SPU, SPU2PPU, PPU2SPU):
        qd = T["qdir%d" % d]
        T.call("_cellSyncLFQueuePushBody", qd, src, 0, rc="TryPush direction %d" % d)
        T.call("_cellSyncLFQueuePopBody", qd, dst, 0, rc="TryPop direction %d" % d)

    # ---- cellSync: blocking pop and push between PPU threads ---------------------------------
    T.call("cellSyncLFQueueClear", q, rc=False)
    res = T.alloc("bres", 16, 16, b"\xee" * 16)
    done = T.alloc("bdone", 16, 16)
    tid = T.alloc("tid", 16, 16)
    tret = T.alloc("tret", 16, 16)
    T.store_word(dst, 0xEEEEEEEE)

    def popper(T):
        T.call("_cellSyncLFQueuePopBody", q, dst, 1, rc=False)
        T.emit(P.li32(9, res), P.stw(3, 9, 0))
        T.store_word(done, 1)
    T.thread("popper", popper)
    T.start_thread("popper", 0, tid)
    T.sleep_us(50000)
    T.record_mem("blocking pop returned on an empty queue (done)", done, 4)
    T.call("_cellSyncLFQueuePushBody", q, src + ENTRY * 5, 0, rc="TryPush for the popper")
    T.wait_word(done, 1)
    T.record_mem("popper: rc", res, 4)
    T.record_mem("popper: entry", dst, ENTRY)
    T.join_thread(tid, tret)

    for k in range(DEPTH):
        T.call("_cellSyncLFQueuePushBody", q, src + ENTRY * k, 0, rc=False)
    T.store_word(done, 0)
    T.store_word(res, 0xEEEEEEEE)

    def pusher(T):
        T.call("_cellSyncLFQueuePushBody", q, src + ENTRY * 6, 1, rc=False)
        T.emit(P.li32(9, res), P.stw(3, 9, 0))
        T.store_word(done, 1)
    T.thread("pusher", pusher)
    T.start_thread("pusher", 0, tid)
    T.sleep_us(50000)
    T.record_mem("blocking push returned on a full queue (done)", done, 4)
    T.call("_cellSyncLFQueuePopBody", q, dst, 0, rc="TryPop for the pusher")
    T.wait_word(done, 1)
    T.record_mem("pusher: rc", res, 4)
    T.join_thread(tid, tret)
    for k in range(DEPTH):
        T.call("_cellSyncLFQueuePopBody", q, dst + ENTRY * k, 0, rc=False)
    T.record_mem("entries after the pusher", dst, ENTRY * DEPTH)

    # ---- SPURS LFQueue -------------------------------------------------------------------------
    # each case on fresh (zero) memory: an initialized queue refuses a second Initialize
    sinit = "_cellSpursLFQueueInitialize"
    cases = [("null taskset", 0, ENTRY, DEPTH, ANY2ANY), ("depth 0", ts, ENTRY, 0, ANY2ANY),
             ("direction 4", ts, ENTRY, DEPTH, 4), ("IWL (spurs)", spurs, ENTRY, DEPTH, ANY2ANY)]
    for k, (tag, owner, size, depth, d) in enumerate(cases):
        qk = T.alloc("sq_case%d" % k, 128, 128)
        T.call(sinit, owner, qk, buf, size, depth, d, rc="SPURS Initialize " + tag)
        T.record_mem("SPURS queue after Initialize " + tag, qk, 128)
    T.call(sinit, ts, 0, buf, ENTRY, DEPTH, ANY2ANY, rc="SPURS Initialize null queue")
    sq = T.alloc("sq", 128, 128)
    T.call(sinit, ts, sq, buf, ENTRY, DEPTH, ANY2ANY, rc="SPURS Initialize taskset")
    T.record_mem("SPURS queue after Initialize", sq, 128)
    sqi = T["sq_case3"]
    T.store_word(out, 0xEEEEEEEE)
    T.call("cellSpursLFQueueGetTasksetAddress", sq, out, rc="SPURS GetTasksetAddress")
    T.record_mem("SPURS taskset address", out, 4)
    T.store_word(out, 0xEEEEEEEE)
    T.call("cellSpursLFQueueGetTasksetAddress", sqi, out, rc="SPURS GetTasksetAddress IWL")
    T.record_mem("SPURS taskset address IWL", out, 4)

    for k in range(DEPTH + 1):
        T.call("_cellSpursLFQueuePushBody", sq, src + ENTRY * k, 0, rc="SPURS TryPush %d" % k)
    for k in range(DEPTH + 1):
        T.call("_cellSpursLFQueuePopBody", sq, dst + ENTRY * k, 0, rc="SPURS TryPop %d" % k)
    T.record_mem("SPURS popped entries", dst, ENTRY * (DEPTH + 1))

    T.call("cellSpursLFQueueDetachLv2EventQueue", sq, rc="SPURS Detach before attach")
    T.call("cellSpursLFQueueAttachLv2EventQueue", sq, rc="SPURS AttachLv2EventQueue")
    # lv2 ids libsre keeps in the queue (event port, queue, SPU port) are formatted by each
    # kernel its own way (docs/KNOWN_DIFFERENCES.md, "lv2 object ids")
    T.record_mem("SPURS queue after attach", sq, 128, ignore=((0x28, 12), (0x50, 4), (0x78, 8)))
    T.call("cellSpursLFQueueAttachLv2EventQueue", sq, rc="SPURS AttachLv2EventQueue again")

    T.store_word(done, 0)
    T.store_word(res, 0xEEEEEEEE)
    T.store_word(dst, 0xEEEEEEEE)

    def spopper(T):
        T.call("_cellSpursLFQueuePopBody", sq, dst, 1, rc=False)
        T.emit(P.li32(9, res), P.stw(3, 9, 0))
        T.store_word(done, 1)
    T.thread("spopper", spopper)
    T.start_thread("spopper", 0, tid)
    T.sleep_us(50000)
    T.record_mem("SPURS blocking pop returned on an empty queue (done)", done, 4)
    T.call("_cellSpursLFQueuePushBody", sq, src + ENTRY * 7, 0, rc="SPURS TryPush for the popper")
    T.wait_word(done, 1)
    T.record_mem("SPURS popper: rc", res, 4)
    T.record_mem("SPURS popper: entry", dst, ENTRY)
    T.join_thread(tid, tret)

    T.call("cellSpursLFQueueDetachLv2EventQueue", sq, rc="SPURS DetachLv2EventQueue")
    T.call("cellSpursLFQueueDetachLv2EventQueue", sq, rc="SPURS DetachLv2EventQueue again")

    T.call("cellSpursShutdownTaskset", ts, rc="ShutdownTaskset")
    T.call("cellSpursJoinTaskset", ts, rc="JoinTaskset")
    T.call("cellSpursFinalize", spurs, rc="Finalize")
