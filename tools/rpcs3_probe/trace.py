"""trace.py - hardened stack walk + single-step PC tracer for RPCS3.

Two upgrades over diag.py:
  1. validated stack walk: a frame's return addr is only kept if the instruction
     at ret-4 is a *linking branch* (bl/bcl/bctrl/blrl) -> filters spurious frames.
  2. single-step trace: steps the PPU N times recording PC -> reveals the actual
     loop cycle without needing to get past a title/pause screen.
"""
import argparse, os, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rsp import RSPClient

PCN = ("pc", "nip", "iar", "cia")
def pcof(regs): return next((regs[n] & 0xFFFFFFFF for n in PCN if n in regs), None)

def is_link_branch(insn):
    op = insn >> 26
    if insn & 1 != 1:           # LK bit
        return False
    if op in (16, 18):          # bcl / bl(a)
        return True
    if op == 19:                # bclrl / bcctrl
        return ((insn >> 1) & 0x3FF) in (16, 528)
    return False

def walk(r, regs, maxf=40):
    pc = pcof(regs); sp = regs.get("r1", 0) & 0xFFFFFFFF; lr = regs.get("lr", 0) & 0xFFFFFFFF
    frames = [(pc, "pc")]
    seen = set(); cur = sp
    for _ in range(maxf):
        if cur in seen or cur < 0x10000 or cur >= 0xF0000000: break
        seen.add(cur)
        try:
            nsp = r.read_u64(cur) & 0xFFFFFFFF
            if nsp <= cur or nsp >= 0xF0000000: break
            ret = r.read_u64(nsp + 16) & 0xFFFFFFFF
        except Exception: break
        if 0x10000 <= ret < 0x10000000:
            try:
                if is_link_branch(r.read_u32(ret - 4)):
                    frames.append((ret, "ok"))
                else:
                    frames.append((ret, "?unvalidated"))
            except Exception:
                frames.append((ret, "?"))
        cur = nsp
    return frames

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rpcs3", required=True); ap.add_argument("--title", required=True)
    ap.add_argument("--boot-wait", type=float, default=22.0)
    ap.add_argument("--steps", type=int, default=400)
    a = ap.parse_args()
    args = [a.rpcs3, a.title, "--no-gui"]
    print("[trace] launching:", " ".join(args))
    flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, creationflags=flags)
    r = RSPClient(port=2345, timeout=8)
    try:
        time.sleep(a.boot_wait)
        r.connect(retries=30, delay=1.0); r.handshake()
        regs = r.read_registers()
        print(f"[trace] PC=0x{(pcof(regs) or 0):08X}  validated stack:")
        for i, (f, tag) in enumerate(walk(r, regs)):
            print(f"    #{i:2d} 0x{(f or 0):08X}  {tag}")

        # single-step trace
        print(f"\n[trace] single-stepping {a.steps}x ...")
        pcs = []
        t0 = time.time()
        for k in range(a.steps):
            r.step()
            stop = r.wait_stop(deadline=time.time() + 5)
            if not stop or stop[:1] not in (b"S", b"T"):
                print(f"[trace] step {k}: no stop ({stop[:20]!r}); single-step unsupported under LLVM?")
                break
            regs = r.read_registers()
            pcs.append(pcof(regs))
        dt = time.time() - t0
        if pcs:
            print(f"[trace] stepped {len(pcs)} insns in {dt:.1f}s")
            # PC histogram (hot spots) + range
            from collections import Counter
            hot = Counter(pcs).most_common(12)
            lo, hi = min(pcs), max(pcs)
            print(f"[trace] PC range 0x{lo:08X}..0x{hi:08X}")
            print("[trace] hottest PCs:")
            for pc, n in hot:
                print(f"    0x{pc:08X}  x{n}")
            # detect the loop span (min..max of the most common cluster)
            uniq = sorted(set(pcs))
            print(f"[trace] {len(uniq)} distinct PCs; first 0x{uniq[0]:08X} last 0x{uniq[-1]:08X}")
        try:
            r._send_packet("D"); r._recv_packet()
        except Exception: pass
    except Exception as e:
        import traceback; traceback.print_exc()
    finally:
        try: r.close()
        except Exception: pass
        proc.kill(); print("[trace] done")

if __name__ == "__main__":
    main()
