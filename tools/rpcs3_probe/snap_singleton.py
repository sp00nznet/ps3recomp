from rsp import RSPClient
import struct
c=RSPClient(port=2345); c.connect()
def u32(va):
    try: return struct.unpack('>I', c.read_mem(va,4))[0]
    except Exception: return None
TOC=0x008969A8
sing = u32(TOC-0x4BFC)            # metaclass singleton = [r2-0x4BFC]
print("REAL-HW metaclass singleton [0x%08X] = 0x%08X"%(TOC-0x4BFC, sing or 0))
if sing:
    for off in (-0x7DAC,-0x7DE8,-0x7DD4,-0x7CC0,-0x7FE8):
        p=u32(sing+ (off & 0xFFFFFFFF) if off>=0 else sing+off)
        print("  [sing%+#x] = 0x%08X"%(off, p or 0))
    reg = u32(sing-0x7DAC)
    if reg:
        print("  registry [sing-0x7DAC]=0x%08X fields:"%reg, ' '.join('0x%08X'%(u32(reg+i*4) or 0) for i in range(6)))
c._send_packet("D"); c._recv_packet(); c.close(); print("detached")
