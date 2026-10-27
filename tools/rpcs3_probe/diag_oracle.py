from rsp import RSPClient
import struct
c=RSPClient(port=2345); c.connect()
r=c.read_registers()
pc=r.get("pc",r.get("cia",0))&0xFFFFFFFF
print("halted pc=0x%08X r1=0x%08X"%(pc, r.get("r1",0)&0xFFFFFFFF))
try:
    m=c.read_mem(0x006CEAA0,8)
    print("mem[0x6CEAA0]=",m.hex(),"(expect f821ff817c0802a6)")
except Exception as e: print("mem read err:",e)
ok=c.add_breakpoint(0x006CEAA0); print("add_breakpoint(0x6CEAA0) ->",ok)
# step 5 instructions to confirm interpreter executes
for i in range(5):
    c.step(); rep=c.wait_stop(deadline=__import__("time").time()+10)
    rr=c.read_registers(); p=rr.get("pc",rr.get("cia",0))&0xFFFFFFFF
    print("  step%d pc=0x%08X stop=%s"%(i,p,rep[:6]))
c.remove_breakpoint(0x006CEAA0)
c._send_packet("D"); c._recv_packet(); c.close(); print("detached")
