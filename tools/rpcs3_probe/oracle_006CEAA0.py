#!/usr/bin/env python3
"""Oracle probe: capture func_006CEAA0 calls on real-HW RPCS3 to resolve the flОw
metaclass deserialization divergence. Question: does real HW ever call
func_006CEAA0 with [r3+0]==0x10071038 && r4==5, and what does it return?
"""
import sys, time
from rsp import RSPClient, RSPError

BP = 0x006CEAA0
TARGET_TABLE = 0x10071038
MAX_HITS = int(sys.argv[1]) if len(sys.argv) > 1 else 400
DEADLINE = time.time() + (int(sys.argv[2]) if len(sys.argv) > 2 else 240)

c = RSPClient(port=2345)
c.connect()
print("connected; halted target")
c.add_breakpoint(BP)
print(f"bp @ 0x{BP:08X}; capturing up to {MAX_HITS} hits ...")

dist = {}        # (r4, [r3+0]) -> count
matches = []     # full captures where [r3+0]==TARGET_TABLE
hits = 0
first = True
while hits < MAX_HITS and time.time() < DEADLINE:
    c.cont()
    # First hit may be MINUTES away (slow interpreter boot to deserialization). Wait up to the
    # whole deadline for it; once deserialization starts, subsequent hits are fast.
    wdl = DEADLINE if first else min(DEADLINE, time.time() + 60)
    rep = c.wait_stop(deadline=wdl)
    if rep[:1] in (b"W", b"X"):
        print("target exited/terminated:", rep[:40]); break
    if not rep:
        print(f"  (no stop; hits so far={hits}, elapsed={int(time.time()-(DEADLINE-200))}s)")
        if not first: break
        continue
    first = False
    regs = c.read_registers()
    pc = regs.get("pc", regs.get("cia", 0)) & 0xFFFFFFFF
    if pc != BP:
        # stopped somewhere else (signal); keep going
        continue
    hits += 1
    r3 = regs["r3"] & 0xFFFFFFFF
    r4 = regs["r4"] & 0xFFFFFFFF
    try:
        t0 = c.read_u32(r3) & 0xFFFFFFFF
    except Exception:
        t0 = 0xDEADBEEF
    key = (r4, t0)
    dist[key] = dist.get(key, 0) + 1
    if t0 == TARGET_TABLE or (r4 == 5 and 0x10070000 <= t0 < 0x10080000):
        lr = regs.get("lr", 0) & 0xFFFFFFFF
        # get return value: bp at LR, continue, read r3
        ret = None
        try:
            c.add_breakpoint(lr)
            c.cont(); c.wait_stop(deadline=time.time() + 10)
            rr = c.read_registers()
            ret = rr["r3"] & 0xFFFFFFFF
            c.remove_breakpoint(lr)
        except Exception as e:
            ret = f"err:{e}"
        cap = {"hit": hits, "r3_key": f"0x{r3:08X}", "r4_index": r4,
               "key[0]": f"0x{t0:08X}", "lr": f"0x{lr:08X}", "return": f"0x{ret:08X}" if isinstance(ret,int) else str(ret)}
        matches.append(cap)
        print("  MATCH:", cap)

print(f"\n=== {hits} hits captured ===")
print("distribution (r4_index, [r3+0]) -> count  [top 20]:")
for k, v in sorted(dist.items(), key=lambda kv: -kv[1])[:20]:
    print(f"  r4={k[0]:<4} [r3+0]=0x{k[1]:08X}  x{v}")
print(f"\n{len(matches)} matches with target table / r4==5+class-range:")
for m in matches:
    print("  ", m)
try:
    c.remove_breakpoint(BP)
    c._send_packet("D"); c._recv_packet()
except Exception:
    pass
c.close()
print("detached.")
