from rsp import RSPClient
import time
c=RSPClient(port=2345); c.connect()
def pc(): r=c.read_registers(); return (r.get("pc",r.get("cia",0)))&0xFFFFFFFF
p0=pc(); print("connected; halted pc=0x%08X"%p0)
# 1) step mechanism
print("--- single-step x5 ---")
ok_step=False
for i in range(5):
    c.step(); rep=c.wait_stop(deadline=time.time()+8)
    p=pc(); print("  step%d -> pc=0x%08X stop=%s"%(i,p,rep[:6]))
    if p!=p0: ok_step=True
    p0=p
print("STEP WORKS:", ok_step)
# 2) breakpoint mechanism: bp a bit ahead, cont, see if it hits
tgt=(p0+0x10)&0xFFFFFFFF
print("--- bp @0x%08X, cont ---"%tgt)
c.add_breakpoint(tgt)
c.cont(); rep=c.wait_stop(deadline=time.time()+15)
ph=pc(); print("  after cont: pc=0x%08X stop=%s  HIT=%s"%(ph,rep[:6], ph==tgt))
c.remove_breakpoint(tgt)
c._send_packet("D"); c._recv_packet(); c.close(); print("detached")
