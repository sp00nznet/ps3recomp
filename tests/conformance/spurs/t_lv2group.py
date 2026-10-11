"""lv2 SPU thread group join, when the joining thread is not the one that
starts the group (docs/KNOWN_DIFFERENCES.md, "Joining an SPU thread group that
is not running"):

  A. a waiter thread joins before the group's first start;
  B. a waiter joins after the previous run was already joined (a dedicated
     join thread: start/join alternate across threads);
  C. a second join while the waiter is joining;
  D. a one-thread group whose thread is at index 5 (lv2 takes any index
     0..7), started while another group's thread exists.

The SPU program ends the group with sys_spu_thread_group_exit, using a value
the PPU stores before each start as the status, so a join that reports the
previous run (or no run) is told apart from one that waited for this one.

Records made inside the waiter threads are stored to memory and recorded by
the main thread (thread records are not printed)."""
import struct
from spurs_lib import P, S, SYS, SpuTask

CODE_LS = 0x1000


def spu_group_exit_with_word():
    """r3 = {EA hi, EA lo}: GET the 16 bytes at EA, then
    sys_spu_thread_group_exit(word 0)."""
    t = SpuTask(CODE_LS)
    a = t.a
    a.rotqbyi(10, 3, 4)                      # EA lo -> preferred slot
    a.ila(11, 0x2000)
    t.dma(S.GET, 11, 10, 16)
    a.lqa(12, 0x2000)
    a.wrch(S.SPU_WrOutMbox, 12)
    a.stop(0x101)                            # sys_spu_thread_group_exit
    return t.bytes()


def body(T):
    elf = T.spu_elf("grpexit", spu_group_exit_with_word(), CODE_LS)
    img = T.alloc("img", 16, 16)
    T.call("sys_spu_image_import", img, elf, 1, rc="import")

    val = T.alloc("val", 16, 128)                # the status the next run exits with
    gname = T.alloc("gname", 16, 16, b"lv2grp\0")
    gattr = T.alloc("gattr", 16, 16)
    T.emit(P.li32(3, gattr), P.li32(4, 7), P.stw(4, 3, 0), P.li32(4, gname), P.stw(4, 3, 4))
    tattr = T.alloc("tattr", 16, 16)
    T.emit(P.li32(3, tattr), P.li32(4, gname), P.stw(4, 3, 0), P.li32(4, 7), P.stw(4, 3, 4))
    targ = T.alloc("targ", 32, 16)
    T.store_word(targ + 4, val)
    ids = T.alloc("ids", 16, 16)
    T.syscall(SYS["spu_group_create"], ids, 1, 100, gattr, rc="group_create")
    T.syscall(SYS["spu_thread_initialize"], ids + 4, ("mem", ids), 0, img, tattr, targ,
              rc="thread_initialize")

    # waiter k: join the group, store {rc, cause, status}, then flag = 1
    res = {k: T.alloc("res_" + k, 16, 16, b"\xee" * 16) for k in "AB"}
    flag = {k: T.alloc("flag_" + k, 16, 16) for k in "AB"}
    for k in "AB":
        def waiter(T, k=k):
            r = res[k]
            T.syscall(SYS["spu_group_join"], ("mem", ids), r + 4, r + 8)
            T.emit(P.li32(9, r), P.stw(3, 9, 0))
            T.store_word(flag[k], 1)
        T.thread("waiter_" + k, waiter)
    tid = T.alloc("tid", 16, 16)
    tret = T.alloc("tret", 16, 16)
    join = T.alloc("join", 16, 16, b"\xee" * 16)

    # ---- A: the waiter joins before the first start ---------------------------------
    T.store_word(val, 0x111)
    T.start_thread("waiter_A", 0, tid)
    T.sleep_us(50000)
    T.record_mem("A: waiter returned before the group started (flag)", flag["A"], 4)
    T.syscall(SYS["spu_group_join"], ("mem", ids), join, join + 4,
              rc="C: second join while the waiter is joining")
    T.syscall(SYS["spu_group_start"], ("mem", ids), rc="A: group_start")
    T.wait_word(flag["A"], 1)
    T.record_mem("A: waiter's join {rc, cause, status}", res["A"], 12)
    T.join_thread(tid, tret)

    # ---- B: a run is started and joined, then a waiter joins before the next ----
    T.store_word(val, 0x222)
    T.syscall(SYS["spu_group_start"], ("mem", ids), rc="B: group_start, run 2")
    T.syscall(SYS["spu_group_join"], ("mem", ids), join, join + 4, rc="B: main joins run 2")
    T.record_mem("B: main's join of run 2 {cause, status}", join, 8)
    T.store_word(val, 0x333)
    T.start_thread("waiter_B", 0, tid)
    T.sleep_us(50000)
    T.record_mem("B: waiter returned before run 3 started (flag)", flag["B"], 4)
    T.syscall(SYS["spu_group_start"], ("mem", ids), rc="B: group_start, run 3")
    T.wait_word(flag["B"], 1)
    T.record_mem("B: waiter's join {rc, cause, status}", res["B"], 12)
    T.join_thread(tid, tret)

    # ---- D: a one-thread group with its thread at index 5 -----------------------------
    ids5 = T.alloc("ids5", 16, 16)
    T.syscall(SYS["spu_group_create"], ids5, 1, 100, gattr, rc="D: group_create (1 thread)")
    T.syscall(SYS["spu_thread_initialize"], ids5 + 4, ("mem", ids5), 5, img, tattr, targ,
              rc="D: thread_initialize index 5")
    T.store_word(val, 0x555)
    T.syscall(SYS["spu_group_start"], ("mem", ids5), rc="D: group_start")
    T.store_word(join, 0xEEEEEEEE); T.store_word(join + 4, 0xEEEEEEEE)
    T.syscall(SYS["spu_group_join"], ("mem", ids5), join, join + 4, rc="D: group_join")
    T.record_mem("D: join {cause, status}", join, 8)
    T.syscall(SYS["spu_group_destroy"], ("mem", ids5), rc="D: group_destroy")

    T.syscall(SYS["spu_group_destroy"], ("mem", ids), rc="group_destroy")
