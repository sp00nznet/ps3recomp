from rsp import RSPClient
import struct
c=RSPClient(port=2345); c.connect()
r=c.read_registers()
print("halted pc=0x%08X r2(toc)=0x%08X"%((r.get("pc",r.get("cia",0)))&0xFFFFFFFF, r.get("r2",0)&0xFFFFFFFF))
def dump(va,n,label):
    try:
        m=c.read_mem(va,n)
        words=' '.join('%08X'%struct.unpack('>I',m[i:i+4])[0] for i in range(0,n,4))
        print("  %-26s 0x%08X: %s"%(label,va,words))
    except Exception as e:
        print("  %-26s 0x%08X: ERR %s"%(label,va,e))
dump(0x008969A8,32,"TOC-anchor/deref")          # past .data end (0x895CD4) in our model
dump(0x00896900,32,"near TOC anchor")
dump(0x0084E760,32,"OPD method table")          # static OPDs in our ELF
dump(0x10071038,32,"class struct")              # vtable-of-vtables
c._send_packet("D"); c._recv_packet(); c.close(); print("detached")
