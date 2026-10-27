"""diag.py - learn RPCS3's GDB stub behavior step-by-step (defensive).

RPCS3's GDB stub HALTS the target when a client connects (?, returns S05). So we
read state directly (no interrupt needed), then cont()+sleep()+interrupt() to grab
a running-state snapshot (e.g. during render)."""
import argparse, os, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rsp import RSPClient

ap = argparse.ArgumentParser()
ap.add_argument("--rpcs3", required=True)
ap.add_argument("--title", required=True)
ap.add_argument("--boot-wait", type=float, default=12.0)
ap.add_argument("--run-secs", type=float, default=12.0, help="run time before render snapshot")
a = ap.parse_args()

PCN = ("pc", "nip", "iar", "cia")
def pcof(regs): return next((regs[n] for n in PCN if n in regs), None)

def walk(r, regs, n=24):
    pc = pcof(regs); sp = regs.get("r1", 0) & 0xFFFFFFFF; lr = regs.get("lr", 0) & 0xFFFFFFFF
    frames = [("pc", pc)]
    if lr: frames.append(("lr", lr))
    seen = set()
    for _ in range(n):
        if not sp or sp in seen or sp < 0x10000 or sp >= 0xF0000000: break
        seen.add(sp)
        try:
            nsp = r.read_u64(sp) & 0xFFFFFFFF
            if not nsp or nsp <= sp: break
            ret = r.read_u64(nsp + 16) & 0xFFFFFFFF
        except Exception: break
        if ret: frames.append(("bc", ret))
        sp = nsp
    return frames

args = [a.rpcs3, a.title, "--no-gui"]
print("[diag] launching:", " ".join(args))
flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, creationflags=flags)
r = RSPClient(port=2345, timeout=8)
try:
    print(f"[diag] waiting {a.boot_wait}s for GDB server...")
    time.sleep(a.boot_wait)
    r.connect(retries=30, delay=1.0)
    r.handshake()
    print(f"[diag] connected; {len(r.reg_layout)} regs; special:",
          [n for n, _, _ in r.reg_layout][64:])
    r._send_packet("?"); print("[diag] state:", r._recv_packet()[:40])

    # Connecting halts the running game at its current PC. Walk that stack —
    # if we connected during the steady loop, the stack reveals the loop.
    regs = r.read_registers()
    pc = pcof(regs)
    print(f"\n[diag] *** HALT-ON-CONNECT SNAPSHOT ***  PC=0x{(pc or 0):08X} "
          f"r1=0x{regs.get('r1',0):08X}")
    print("[diag] CALL STACK (innermost first):")
    for i, (via, f) in enumerate(walk(r, regs)):
        print(f"    #{i:2d} 0x{(f or 0):08X}  ({via})")

    # clean detach so RPCS3's GDB server thread doesn't fault on socket close
    try:
        r._send_packet("D")
        r._recv_packet()
    except Exception:
        pass
except Exception as e:
    import traceback; traceback.print_exc()
    print("[diag] FAILED:", type(e).__name__, e)
finally:
    try: r.close()
    except Exception: pass
    proc.kill(); print("[diag] done")
