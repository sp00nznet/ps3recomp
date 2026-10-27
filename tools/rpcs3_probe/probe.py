"""probe.py - RPCS3 oracle introspection rig (title-agnostic).

Boots a title in RPCS3 with the GDB stub, runs a "recipe" of captures, and dumps
structured JSON: call stacks (PPC64 ELFv1 unwind), registers, memory. Use it to
observe the *correct* execution of a PS3 title and compare against a recompiled
build.

Recipe steps (JSON list):
  {"op":"snapshot", "after_s": 12, "label":"render"}      # run, pause @T, capture
  {"op":"breakpoint","addr":"0x000CBAD0","label":"ret",
   "continue_first": true, "max_hits":1}                   # bp, run, capture on hit
  {"op":"mem","addr":"0x10163364","len":64,"label":"subsys-list"}  # raw dump

Usage:
  python probe.py --rpcs3 <rpcs3.exe> --title <EBOOT.BIN> --recipe r.json --out o.json
  python probe.py ... --boot-only-snapshot 12   # shorthand: one render snapshot @12s

Nothing here is game-specific; point --title / --recipe at any title.
"""
import argparse
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rsp import RSPClient, RSPError


def launch_rpcs3(rpcs3, title, headless=True, extra=None):
    args = [rpcs3, title]
    args.append("--headless" if headless else "--no-gui")
    if extra:
        args += extra
    # detach; RPCS3 keeps running until we kill it
    creation = 0
    if os.name == "nt":
        creation = subprocess.CREATE_NEW_PROCESS_GROUP
    p = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         creationflags=creation)
    return p


PC_NAMES = ("pc", "nip", "iar", "cia", "pad_pc")


def _pc(regs):
    for n in PC_NAMES:
        if n in regs:
            return regs[n] & 0xFFFFFFFF
    return None


def walk_stack(rsp, regs, max_frames=64):
    """PPC64 ELFv1 unwind. Return list of {pc, sp} frames, innermost first.

    Innermost PC is the instruction pointer; each caller's return address is the
    LR-save slot (offset 16) in the *next* (caller) frame found via the back chain
    at *(sp). EAs are 32-bit (low word of the 64-bit slot)."""
    frames = []
    pc = _pc(regs)
    sp = regs.get("r1", regs.get("gpr1", 0)) & 0xFFFFFFFF
    lr = regs.get("lr", 0) & 0xFFFFFFFF
    frames.append({"pc": pc, "sp": sp, "via": "pc"})
    seen = set()
    for _ in range(max_frames):
        if sp == 0 or sp in seen or sp < 0x10000 or sp >= 0xF0000000:
            break
        seen.add(sp)
        try:
            next_sp = rsp.read_u64(sp) & 0xFFFFFFFF
        except RSPError:
            break
        if next_sp == 0 or next_sp <= sp or next_sp >= 0xF0000000:
            break
        try:
            ret = rsp.read_u64(next_sp + 16) & 0xFFFFFFFF
        except RSPError:
            break
        if ret == 0:
            sp = next_sp
            continue
        frames.append({"pc": ret, "sp": next_sp, "via": "lr-save"})
        sp = next_sp
    # LR often holds the true innermost return when frame 0 hasn't saved it yet
    if lr and len(frames) >= 1 and (len(frames) < 2 or frames[1]["pc"] != lr):
        frames.insert(1, {"pc": lr, "sp": frames[0]["sp"], "via": "lr-reg"})
    return frames


def capture(rsp, label, max_frames=64):
    regs = rsp.read_registers()
    stack = walk_stack(rsp, regs, max_frames)
    keep = {k: regs[k] for k in regs
            if k in ("r1", "r2", "r3", "r4", "r13", "lr", "ctr") or k in PC_NAMES}
    return {
        "label": label,
        "pc": _pc(regs),
        "regs": {k: f"0x{v & 0xFFFFFFFFFFFFFFFF:08X}" for k, v in keep.items()},
        "stack": [f"0x{f['pc']:08X}" if f["pc"] is not None else None for f in stack],
        "stack_detail": stack,
    }


def run_recipe(rsp, recipe, out, max_frames=64):
    captures = []
    for step in recipe:
        op = step["op"]
        if op == "snapshot":
            after = float(step.get("after_s", 10))
            rsp.cont()
            time.sleep(after)
            rsp.interrupt()
            rsp.wait_stop(deadline=time.time() + 15)
            captures.append(capture(rsp, step.get("label", "snapshot"), max_frames))
        elif op == "breakpoint":
            addr = int(str(step["addr"]), 0)
            rsp.add_breakpoint(addr)
            hits = 0
            maxh = int(step.get("max_hits", 1))
            deadline = time.time() + float(step.get("timeout_s", 60))
            while hits < maxh and time.time() < deadline:
                rsp.cont()
                stop = rsp.wait_stop(deadline=deadline)
                hits += 1
                c = capture(rsp, f"{step.get('label','bp')}#{hits}", max_frames)
                c["stop_reply"] = stop.decode("latin-1", "replace")
                captures.append(c)
            rsp.remove_breakpoint(addr)
        elif op == "mem":
            addr = int(str(step["addr"]), 0)
            n = int(step.get("len", 32))
            data = rsp.read_mem(addr, n)
            captures.append({"label": step.get("label", "mem"),
                             "addr": f"0x{addr:08X}",
                             "hex": data.hex(),
                             "u32_be": [f"0x{int.from_bytes(data[i:i+4],'big'):08X}"
                                        for i in range(0, len(data) - 3, 4)]})
        else:
            captures.append({"label": step.get("label", op), "error": f"unknown op {op}"})
    return captures


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rpcs3", required=True)
    ap.add_argument("--title", required=True)
    ap.add_argument("--recipe", help="JSON recipe file")
    ap.add_argument("--boot-only-snapshot", type=float, default=None,
                    help="shorthand: single render snapshot after N seconds")
    ap.add_argument("--out", default="probe_out.json")
    ap.add_argument("--port", type=int, default=2345)
    ap.add_argument("--no-headless", action="store_true")
    ap.add_argument("--keep-running", action="store_true")
    ap.add_argument("--max-frames", type=int, default=64)
    args = ap.parse_args()

    if args.recipe:
        recipe = json.load(open(args.recipe))
    elif args.boot_only_snapshot is not None:
        recipe = [{"op": "snapshot", "after_s": args.boot_only_snapshot, "label": "render"}]
    else:
        recipe = [{"op": "snapshot", "after_s": 12, "label": "render"}]

    print(f"[probe] launching RPCS3: {args.title}")
    proc = launch_rpcs3(args.rpcs3, args.title, headless=not args.no_headless)
    rsp = RSPClient(port=args.port)
    result = {"title": args.title, "recipe": recipe}
    try:
        print("[probe] connecting to GDB stub...")
        rsp.connect()
        feats = rsp.handshake()
        print(f"[probe] connected. regs discovered: {len(rsp.reg_layout)} "
              f"(pc={'yes' if any(n in rsp.reg_by_name for n in PC_NAMES) else 'NO'}, "
              f"r1={'yes' if 'r1' in rsp.reg_by_name else 'NO'})")
        result["features"] = feats
        result["reg_layout"] = [n for n, _, _ in rsp.reg_layout]
        result["captures"] = run_recipe(rsp, recipe, args.out, args.max_frames)
    except Exception as e:
        result["error"] = f"{type(e).__name__}: {e}"
        print(f"[probe] ERROR: {result['error']}")
    finally:
        try:
            rsp.close()
        except Exception:
            pass
        if not args.keep_running:
            try:
                proc.kill()
            except Exception:
                pass

    json.dump(result, open(args.out, "w"), indent=2)
    print(f"[probe] wrote {args.out}")
    # echo the headline
    for c in result.get("captures", []):
        if "stack" in c:
            print(f"\n=== {c['label']} pc={c.get('pc') and hex(c['pc'])} ===")
            for i, f in enumerate(c["stack"][:16]):
                print(f"  #{i} {f}")


if __name__ == "__main__":
    main()
