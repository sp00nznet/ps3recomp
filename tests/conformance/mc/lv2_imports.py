"""Firmware library imports for a hand-built lv2 executable.

A PS3 executable imports PRX functions through a sys_proc_prx_param block
(PT 0x60000002) whose libstub range lists one 0x2C-byte entry per module:
{name, FNID table, import-address table}. Each import-address slot initially
holds the address of that import's 0x20-byte trampoline in .text; the loader
(RPCS3: ppu_load_imports) overwrites the slot with the address of the target's
function descriptor, and the trampoline calls through it:

    li    r12, 0
    oris  r12, r12, slot@h
    lwz   r12, slot@l(r12)
    std   r2, 40(r1)
    lwz   r0, 0(r12)
    lwz   r2, 4(r12)
    mtctr r0
    bctr

A sys_process_param block (PT 0x60000001) carries the SDK version, which libsre
consults. The import tables must sit in the low 32 KB of the data segment so
`slot@l` is a positive 16-bit displacement.
"""
import hashlib
import struct

import ppc_asm as P

NID_SUFFIX = bytes([0x67, 0x59, 0x65, 0x99, 0x04, 0x25, 0x04, 0x90,
                    0x56, 0x64, 0x27, 0x49, 0x94, 0x89, 0x74, 0x1A])
PROC_PARAM_MAGIC = 0x13BCC5F6
PRX_PARAM_MAGIC = 0x1B434CEC
SDK_VERSION = 0x00360001


def nid(name):
    return struct.unpack("<I", hashlib.sha1(name.encode() + NID_SUFFIX).digest()[:4])[0]


class Imports:
    """modules: {module name: [function names]}. Reserve with take(D) before any
    other data so the tables land in the first 32 KB, emit trampolines with
    emit(t, d), and write the tables with fill(put, d, t)."""

    def __init__(self, modules):
        self.modules = modules
        self.funcs = [(m, f) for m, fs in modules.items() for f in fs]

    def take(self, D):
        D.take("proc_param", 0x40)
        D.take("prx_param", 0x40)
        for m, fs in self.modules.items():
            D.take("modname_" + m, len(m) + 1, 4)
            D.take("nids_" + m, 4 * len(fs), 4)
            D.take("addrs_" + m, 4 * len(fs), 4)
        D.take("libstub", 0x2C * len(self.modules), 4)

    def emit(self, t, d):
        for m, fs in self.modules.items():
            for i, f in enumerate(fs):
                slot = d["addrs_" + m] + 4 * i
                assert slot & 0xFFFF < 0x8000, "import table outside the low 32 KB"
                t.label("imp_" + f)
                t.emit(P.addi(12, 0, 0), P.oris(12, 12, slot >> 16), P.lwz(12, 12, slot & 0xFFFF),
                       P.std(2, 1, 40), P.lwz(0, 12, 0), P.lwz(2, 12, 4),
                       P.mtspr(9, 0), 0x4E800420)

    def fill(self, put, d, t):
        put(d["proc_param"], struct.pack(">IIIIiIII", 0x40, PROC_PARAM_MAGIC, 0x00009000, SDK_VERSION,
                                         1001, 0x100000, 0x100000, 0))
        stubs = d["libstub"]
        put(d["prx_param"], struct.pack(">IIIIIIIIHHI", 0x40, PRX_PARAM_MAGIC, 4, 0, 0, 0,
                                        stubs, stubs + 0x2C * len(self.modules), 0, 0, 0))
        for k, (m, fs) in enumerate(self.modules.items()):
            put(d["modname_" + m], m.encode() + b"\0")
            put(d["nids_" + m], b"".join(struct.pack(">I", nid(f)) for f in fs))
            put(d["addrs_" + m], b"".join(struct.pack(">I", t.labels["imp_" + f]) for f in fs))
            put(stubs + 0x2C * k, struct.pack(">BBHHHHHBB2sIIIIIII", 0x2C, 0, 1, 1, len(fs), 0, 0, 0, 0, b"\0\0",
                                              d["modname_" + m], d["nids_" + m], d["addrs_" + m], 0, 0, 0, 0))

    def extra_ph(self, d):
        return [(0x60000001, d["proc_param"], 0x40), (0x60000002, d["prx_param"], 0x40)]
