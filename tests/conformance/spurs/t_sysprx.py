"""liblv2 (sysPrxForUser) functions firmware libsre depends on, against the
firmware's own liblv2 (RPCS3 runs it LLE): SPU image import / close (both
image types, and what the descriptor and its segment list hold), running an
imported image, _sys_vsnprintf / _sys_snprintf / _sys_vprintf formatting, the
SPU printf attach/detach callbacks, and libprof's user-trace registration."""
import struct
from spurs_lib import P, S, SYS, SpuTask

SPU_CODE_LS, SPU_DATA_LS = 0x1000, 0x2000
DATA_PATTERN = bytes(range(0x40, 0x50))              # 16 file bytes; memsz 64 (48 zero-filled)
EXIT_WORD = 0xC0DE1234


def spu_program():
    """r3 = {EA hi, EA lo}: PUT the 64-byte data segment to EA, exit with the
    data segment's first word."""
    t = SpuTask(SPU_CODE_LS)
    a = t.a
    a.rotqbyi(10, 3, 4)                                  # EA lo -> preferred
    a.ila(11, SPU_DATA_LS)
    t.dma(S.PUT, 11, 10, 64)
    a.lqa(12, SPU_DATA_LS + 0x30)                        # word 0x30 of the data segment (zero-filled)
    a.lqa(13, SPU_DATA_LS)
    a.or_(12, 12, 13)
    a.wrch(S.SPU_WrOutMbox, 12)
    a.stop(0x102)
    return t.bytes()


def spu_elf(code, data_file, data_mem, machine=23):
    """A 32-bit big-endian SPU ELF with two PT_LOAD segments: code (filesz ==
    memsz) and data (memsz > filesz: the rest is zero-filled)."""
    eh, ph = 0x34, 0x20
    off_code = 0x80
    off_data = (off_code + len(code) + 15) & ~15
    phdrs = [(1, off_code, SPU_CODE_LS, SPU_CODE_LS, len(code), len(code), 5, 0x10),
             (1, off_data, SPU_DATA_LS, SPU_DATA_LS, len(data_file), data_mem, 6, 0x10)]
    hdr = b"\x7fELF" + bytes([1, 2, 1, 0]) + bytes(8)
    hdr += struct.pack(">HHIIIIIHHHHHH", 2, machine, 1, SPU_CODE_LS, eh, 0, 0, eh, ph, len(phdrs), 0, 0, 0)
    img = bytearray(hdr)
    for p in phdrs:
        img += struct.pack(">IIIIIIII", *p)
    img += bytes(off_code - len(img)) + code
    img += bytes(off_data - len(img)) + data_file
    return bytes(img)


def body(T):
    code = spu_program()
    code += bytes(-len(code) % 16)                       # lv2 wants 16-byte segment sizes
    data = struct.pack(">I", EXIT_WORD) + DATA_PATTERN[4:]
    elf = spu_elf(code, data, 64)
    src = T.alloc("spu_elf", len(elf), 128, elf)
    odd = T.alloc("spu_elf_odd", len(elf) + 16, 128, spu_elf(code + bytes(4), data, 64))
    ppu_elf = T.alloc("ppu_elf", len(elf), 128, spu_elf(code, data, 64, machine=21))
    sce = T.alloc("sce_hdr", 64, 128, b"SCE\0" + bytes(60))
    junk = T.alloc("junk", 64, 128, bytes(64))
    img = T.alloc("img", 16, 16)
    img2 = T.alloc("img2", 16, 16)

    # ---- import: direct, protected, rejects -------------------------------------
    T.call("sys_spu_image_import", img, src, 1, rc="import DIRECT")
    T.record_mem("DIRECT descriptor {type, entry, segs, nsegs}", img, 16, ignore=((8, 4),))
    T.record_mem("DIRECT segment list (3 entries)", img + 8, 0x48, deref=True)
    T.call("sys_spu_image_import", img2, src, 0, rc="import PROTECT")
    T.record_mem("PROTECT descriptor {type, kernel id, segs, nsegs}", img2, 16, ignore=((4, 4),))
    img3 = T.alloc("img3", 16, 16)
    T.call("sys_spu_image_import", img3, odd, 1, rc="import DIRECT, code size not a multiple of 16")
    for name, a, typ in (("type 2", src, 2), ("not an ELF", junk, 1), ("SCE header", sce, 1),
                         ("PPU ELF", ppu_elf, 1), ("SCE header, PROTECT", sce, 0),
                         ("PPU ELF, PROTECT", ppu_elf, 0)):
        T.call("sys_spu_image_import", T.alloc(T.uniq("imgx"), 16, 16), a, typ, rc="import " + name)

    # ---- run the DIRECT image -----------------------------------------------------
    out = T.alloc("spu_out", 64, 128, b"\xee" * 64)
    gattr = T.alloc("gattr", 16, 16)
    gname = T.alloc("gname", 16, 16, b"sysprx\0")
    T.emit(P.li32(3, gattr), P.li32(4, 7), P.stw(4, 3, 0), P.li32(4, gname), P.stw(4, 3, 4))
    tattr = T.alloc("tattr", 16, 16)
    T.emit(P.li32(3, tattr), P.li32(4, gname), P.stw(4, 3, 0), P.li32(4, 7), P.stw(4, 3, 4))
    targ = T.alloc("targ", 32, 16)
    T.store_word(targ + 4, out)
    ids = T.alloc("ids", 16, 16)
    T.syscall(SYS["spu_group_create"], ids, 1, 100, gattr, rc="group_create")
    T.syscall(SYS["spu_thread_initialize"], ids + 8, ("mem", ids), 0, img3, tattr, targ,
              rc="thread_initialize, segment size not a multiple of 16")
    T.syscall(SYS["spu_thread_initialize"], ids + 4, ("mem", ids), 0, img, tattr, targ, rc="thread_initialize")
    T.syscall(SYS["spu_group_start"], ("mem", ids), rc="group_start")
    join = T.alloc("join", 16, 16)
    T.syscall(SYS["spu_group_join"], ("mem", ids), join, join + 4, rc="group_join")
    T.record_mem("group_join {cause, status}", join, 8)
    st = T.alloc("exit_status", 16, 16)
    T.syscall(SYS["spu_get_exit_status"], ("mem", ids + 4), st, rc="get_exit_status")
    T.record_mem("thread exit status (data word 0 | zero-filled word 12)", st, 4)
    T.record_mem("data segment as loaded (64 bytes)", out, 64)
    T.syscall(SYS["spu_group_destroy"], ("mem", ids), rc="group_destroy")
    T.syscall(SYS["spu_group_join"], ("mem", ids), join, join + 4, rc="group_join after destroy")

    # ---- a group whose one thread was never initialized ----------------------------
    ids2 = T.alloc("ids2", 16, 16)
    T.syscall(SYS["spu_group_create"], ids2, 1, 100, gattr, rc="group_create (thread not initialized)")
    T.syscall(SYS["spu_group_start"], ("mem", ids2), rc="group_start, thread not initialized")
    T.syscall(SYS["spu_group_join"], ("mem", ids2), join, join + 4, rc="group_join, never started")
    T.syscall(SYS["spu_group_destroy"], ("mem", ids2), rc="group_destroy, never started")

    # ---- close --------------------------------------------------------------------
    T.call("sys_spu_image_close", img, rc="close DIRECT")
    T.record_mem("DIRECT descriptor after close", img, 16, ignore=((8, 4),))
    T.call("sys_spu_image_close", img2, rc="close PROTECT")
    T.record_mem("PROTECT descriptor after close", img2, 16, ignore=((4, 4),))
    T.call("sys_spu_image_close", img2, rc="close PROTECT again")
    T.call("sys_spu_image_close", img3, rc="close DIRECT (unaligned)")

    # ---- formatting ---------------------------------------------------------------
    fmts = [
        (b"%d|%i|%u|%x|%X|%o|%c|%%", [-42, 7, 0xFFFFFFFE, 0xBEEF, 0xBEEF, 8, 0x41]),
        (b"[%5d][%-5d][%05d][%+d][% d][%#x][%#o]", [42, 42, 42, 42, 42, 255, 8]),
        (b"%s|%.3s|%8s|%-8s|", None),        # strings filled below
        (b"%lld|%llx|%ld|%hd|%hhx|%zu", [-(1 << 40), 0x123456789A, -5, 0x12345, 0x1FF, 99]),
        (b"%p|%n?", [0x10000, 0]),
        (b"%d|%u|%x|%ld|%lx|%c", [0x100000005, 0x1FFFFFFFF, 0x10000000F, 0x100000005, 0x10000000F, 0x141]),
        (b"%zu|%d|%p|%5p|%s", [99, 5, 0, 0x20, 0]),
        (b"[%*d][%-*d][%.*s]", [6, 42, 6, 42, 2, "S"]),
        (b"%f|%.2f|%e|%g|%5.1f", ["D1.5", "D-2.125", "D1234.5", "D0.0001", "D3.25"]),
        (b"%n|%d", ["N", 77]),
        (b"%lld|%5s|%-3c|%f|%d", [-3, "S", 0x42, 9, 10]),  # %f: does it consume one?
    ]
    sarg = T.alloc("sarg", 16, 16, b"hello\0")
    nout = T.alloc("nout", 16, 16, b"\xee" * 16)

    def slot(a):
        if a == "S":
            return sarg
        if a == "N":
            return nout
        if isinstance(a, str) and a[0] == "D":
            return struct.unpack(">Q", struct.pack(">d", float(a[1:])))[0]
        return a & 0xFFFFFFFFFFFFFFFF

    for k, (f, args) in enumerate(fmts):
        if args is None:
            args = [sarg, sarg, sarg, sarg]
        args = [slot(a) for a in args]
        fe = T.alloc("fmt%d" % k, len(f) + 1, 16, f + b"\0")
        va = T.alloc("va%d" % k, 8 * len(args), 16,
                     b"".join(struct.pack(">Q", a) for a in args))
        buf = T.alloc("buf%d" % k, 96, 16, b"\xee" * 96)
        T.call("_sys_vsnprintf", buf, 96, fe, va, rc="vsnprintf #%d" % k)
        T.record_mem("vsnprintf #%d output" % k, buf, 96)
    T.record_mem("%n target", nout, 16)
    f0 = T["fmt0"]
    for n in (0, 1, 5, 33, 34):
        buf = T.alloc(T.uniq("tbuf"), 32, 16, b"\xee" * 32)
        T.call("_sys_vsnprintf", buf, n, f0, T["va0"], rc="vsnprintf size %d" % n)
        T.record_mem("vsnprintf size %d output" % n, buf, 32)
    buf = T.alloc("snbuf", 96, 16, b"\xee" * 96)
    f1 = T["fmt1"]
    T.call("_sys_snprintf", buf, 96, f1, 42, 42, 42, 42, 42, stack=(255, 8),
           rc="snprintf (5 register args, 2 on the stack)")
    T.record_mem("snprintf output", buf, 96)
    buf = T.alloc("spbuf", 64, 16, b"\xee" * 64)
    T.call("_sys_sprintf", buf, T["fmt2"], sarg, sarg, sarg, sarg, rc="sprintf")
    T.record_mem("sprintf output", buf, 64)
    pf = T.alloc("pfmt", 32, 16, b"vprintf %d %s\n\0")
    pva = T.alloc("pva", 16, 16, struct.pack(">QQ", 7, sarg))
    T.call("_sys_vprintf", pf, pva, rc="vprintf")

    # ---- SPU printf callbacks -----------------------------------------------------
    cb = T.alloc("cbrec", 64, 16)
    for i, name in enumerate(("ag", "dg", "at", "dt")):
        def body_cb(T, i=i):
            T.emit(P.li32(9, cb + 16 * i), P.stw(3, 9, 0), P.stw(4, 9, 4),
                   P.lwz(10, 9, 12), P.addi(10, 10, 1), P.stw(10, 9, 12))
        T.func("spucb_" + name, body_cb, ret=0x100 + i)
    T.call("_sys_spu_printf_attach_group", 7, rc="attach_group before initialize")
    T.call("_sys_spu_printf_detach_group", 7, rc="detach_group before initialize")
    T.call("_sys_spu_printf_initialize", T.opd_of("spucb_ag"), T.opd_of("spucb_dg"),
           T.opd_of("spucb_at"), T.opd_of("spucb_dt"), rc="spu_printf initialize")
    T.call("_sys_spu_printf_attach_group", 7, rc="attach_group 7")
    T.call("_sys_spu_printf_detach_group", 7, rc="detach_group 7")
    T.call("_sys_spu_printf_attach_thread", 9, rc="attach_thread 9")
    T.call("_sys_spu_printf_detach_thread", 9, rc="detach_thread 9")
    T.record_mem("callbacks {arg0, -, -, calls} x {ag, dg, at, dt}", cb, 64,
                 ignore=((4, 4), (20, 4), (36, 4), (52, 4)))
    T.call("_sys_spu_printf_finalize", rc="spu_printf finalize")
    T.call("_sys_spu_printf_attach_group", 7, rc="attach_group after finalize")
    T.call("_sys_spu_printf_detach_thread", 9, rc="detach_thread after finalize")
    T.call("_sys_spu_printf_finalize", rc="spu_printf finalize again")

    # ---- libprof user trace -------------------------------------------------------
    T.call("cellUserTraceRegister", 0, 0, rc="cellUserTraceRegister")
    T.call("cellUserTraceUnregister", 0, rc="cellUserTraceUnregister")
