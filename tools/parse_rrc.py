#!/usr/bin/env python3
"""Parse an RPCS3 RSX capture (.rrc.gz) and dump a draw-list manifest.

Ground-truth oracle for rendering bring-up: what draws/textures/shaders/blend
state the REAL game issues for a frame, decoded from RPCS3's frame_capture_data
(Emu/RSX/Capture/rsx_replay.h, c_fc_version 6, compressed with gzip by
make_compressed_serialization_file_handler).

Serialization (util/serialization.hpp):
  - container sizes: VLE (7 bits per byte, high bit = continue)
  - ENABLE_BITWISE_SERIALIZATION structs: raw sizeof() bytes
  - unordered_map: vle count, then per entry serialize(key), serialize(value)
  - replay_command: pair<u32,u32> raw, unordered_set<u64> (vle+8*n), u64, u64

Command word: (reg << 2) | (count << 18) | flags (0x40000000 = non-increment);
continuation entries have cmd==0 (value only, applied to the running method).

Usage: python parse_rrc.py <capture.rrc.gz> [out_manifest.txt]
"""
import gzip, struct, sys

class R:
    def __init__(self, data):
        self.d = data; self.o = 0
    def u8(self):
        v = self.d[self.o]; self.o += 1; return v
    def u32(self):
        v = struct.unpack_from('<I', self.d, self.o)[0]; self.o += 4; return v
    def u64(self):
        v = struct.unpack_from('<Q', self.d, self.o)[0]; self.o += 8; return v
    def raw(self, n):
        v = self.d[self.o:self.o+n]; self.o += n; return v
    def vle(self):
        v = 0; sh = 0
        while True:
            b = self.u8()
            v |= (b & 0x7F) << sh; sh += 7
            if not (b & 0x80): return v

def parse(path):
    data = gzip.open(path, 'rb').read()
    r = R(data)
    magic, version, le = r.u32(), r.u32(), r.u32()
    assert magic == struct.unpack('<I', b'RRC\0')[0], hex(magic)
    assert version == 6, version
    # tile_map: key tile_state = 15*tile_info(16B) + 8*zcull(24B) = 432B
    for _ in range(r.vle()):
        r.raw(432); r.u64()
    # memory_map: key memory_block {u32,u32,u64} = 16B
    mem_map = {}
    for _ in range(r.vle()):
        off, loc = r.u32(), r.u32()
        ds = r.u64(); idx = r.u64()
        mem_map[idx] = (off, loc, ds)
    # memory_data_map: key = vector<u8>
    mem_data = {}
    for _ in range(r.vle()):
        n = r.vle(); blob = r.raw(n); idx = r.u64()
        mem_data[idx] = blob
    # display_buffers_map: key 8*buffer_state(16B)+u32 = 132B
    for _ in range(r.vle()):
        r.raw(132); r.u64()
    # replay_commands
    n_cmds = r.vle()
    cmds = []
    for _ in range(n_cmds):
        cmd, val = r.u32(), r.u32()
        ms = frozenset(r.u64() for _ in range(r.vle()))
        ts, dbs = r.u64(), r.u64()
        cmds.append((cmd, val, ms))
    return cmds, mem_map, mem_data

PRIMS = {1:'POINTS',2:'LINES',3:'LINE_LOOP',4:'LINE_STRIP',5:'TRIANGLES',
         6:'TRI_STRIP',7:'TRI_FAN',8:'QUADS',9:'QUAD_STRIP',10:'POLYGON'}

def decode(cmds, out):
    regs = {}
    cur_method = 0; non_inc = False
    draw_no = 0; verts = 0; prim = 0; idx_count = 0
    def texinfo(u):
        base = 0x1A00//4 + u*8
        off  = regs.get(base+0, 0); fmt = regs.get(base+1, 0)
        ctl1 = regs.get(base+4, 0); rect = regs.get(base+6, 0)
        w, hgt = rect >> 16, rect & 0xFFFF
        f = (fmt >> 8) & 0xFF
        return off, fmt, f, ctl1, w, hgt
    for cmd, val, ms in cmds:
        if cmd == 0:                       # continuation of previous method
            reg = cur_method
            if not non_inc: cur_method += 1
        else:
            reg = (cmd >> 2) & 0xFFFF
            cnt = (cmd >> 18) & 0x7FF
            non_inc = bool(cmd & 0x40000000)
            cur_method = reg if non_inc else reg + 1
        regs[reg] = val
        moff = reg << 2                    # NV4097 method byte offset
        if moff == 0x1808:                 # SET_BEGIN_END
            if val:
                prim = val; verts = 0; idx_count = 0
            else:
                draw_no += 1
                n = verts + idx_count
                sh  = regs.get(0x8E4//4, 0)   # SHADER_PROGRAM (FP addr|location)
                be  = regs.get(0x310//4, 0)   # BLEND_ENABLE
                bf  = regs.get(0x314//4, 0)   # BLEND_FUNC_SFACTOR
                bq  = regs.get(0x320//4, 0)   # BLEND_EQUATION
                out.write(f"draw{draw_no:03d} {PRIMS.get(prim,prim)} n={n} "
                          f"fp=0x{sh:08X} blend={be&1} bf=0x{bf:08X} beq=0x{bq:08X}\n")
                for u in range(4):
                    off, fmt, f, ctl1, w, hgt = texinfo(u)
                    en = regs.get(0x1A0C//4 + u*8, 0)  # CONTROL0 enable
                    if off or fmt:
                        out.write(f"   t{u} off=0x{off:08X} fmt=0x{f:02X}"
                                  f"{'(LN)' if fmt&0x2000 else '(SZ)'} {w}x{hgt}"
                                  f" remap=0x{ctl1&0xFFFF:04X} ctl0=0x{en:08X}\n")
        elif moff == 0x1814:               # DRAW_ARRAYS
            verts += ((val >> 24) & 0xFF) + 1
        elif moff == 0x1824:               # DRAW_INDEX_ARRAY
            idx_count += ((val >> 24) & 0xFF) + 1
        elif moff == 0x1810:               # SET_INDEX_ARRAY_ADDRESS
            pass
    out.write(f"\ntotal draws: {draw_no}\n")

if __name__ == '__main__':
    path = sys.argv[1]
    outp = sys.argv[2] if len(sys.argv) > 2 else None
    cmds, mem_map, mem_data = parse(path)
    import io
    out = open(outp, 'w') if outp else sys.stdout
    out.write(f"replay_commands: {len(cmds)}  memory_blocks: {len(mem_map)}\n")
    decode(cmds, out)
