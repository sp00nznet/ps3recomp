#!/usr/bin/env python3
"""Frame-by-frame report of a PS3_FTRACE timeline.

  tools/ftrace_report.py <trace.txt> [--frames N] [--ms-per-col F] [--threads a,b]

PS3_FTRACE=<start s>:<seconds>:<path> (toolkit, include/ps3emu/ps3_ftrace.h)
records spans: PPU syscalls that blocked and merged usleep poll runs (with the
guest call site), SPU busy/wait spans (busy tagged with the lifted image and the
SPURS workload id), the RSX walker blocked on a semaphore ACQUIRE, FIFO drains,
flips and presents. A frame here is the time between two guest flip requests.

The report says, for the median frame, how each thread spent it, and draws a
few frames as a timeline (one column per --ms-per-col ms):
  PPU   '#' running   '.' usleep poll   's' other blocking syscall
  SPU   '#' busy      ' ' waiting on a channel
  RSX   'S' blocked on ACQUIRE   'D' FIFO drain   'P' present   '|' flip reached
        inside D/P: 'E' Metal encode   'G' waiting for the GPU   'R' render-target readback
"""
import argparse
import collections
import statistics
import sys

(SYSCALL, USLEEP, SPU_WAIT, SPU_BUSY, SEMA, SEMA_GAVEUP, DRAIN, FLIP_FIFO, FLIP_REQ, PRESENT,
 GPU_ENCODE, GPU_WAIT, READBACK, GPU_EXEC) = range(1, 15)
GPU_WAIT_KIND = {0: "gpu wait (flush)", 1: "gpu wait (present)", 2: "gpu wait (in-flight)"}

SYSCALL_NAMES = {   # runtime/syscalls/lv2_syscall_table.h
    43: "thread_yield", 44: "thread_join", 85: "event_flag_wait", 92: "semaphore_wait",
    97: "lwmutex_lock", 102: "mutex_lock", 107: "cond_wait", 113: "lwcond_wait",
    122: "rwlock_rlock", 125: "rwlock_wlock", 130: "event_queue_receive", 138: "event_port_send",
    141: "usleep", 142: "sleep", 178: "spu_group_join", 801: "fs_open", 802: "fs_read", 818: "fs_lseek",
}


RSX_ROWS = ("acquire wait", "drain", "present", "encode (flush)", "encode (present)",
            "gpu wait (flush)", "gpu wait (present)", "gpu wait (in-flight)", "readback")


def load(path):
    names, spans, header = {}, [], ""
    with open(path) as f:
        for line in f:
            if line.startswith("#"):
                header = line.strip()
                continue
            if line.startswith("T "):
                _, tid, name = line.rstrip("\n").split(" ", 2)
                names[int(tid)] = name
                continue
            k, who, a, b, t0, t1 = line.split()
            spans.append((int(k), int(who), int(a, 16), int(b, 16), float(t0) / 1e3, float(t1) / 1e3))
    spans.sort(key=lambda s: s[4])
    return header, names, spans


def overlap(s0, s1, f0, f1):
    return max(0.0, min(s1, f1) - max(s0, f0))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--frames", type=int, default=3, help="frames drawn as a timeline")
    ap.add_argument("--ms-per-col", type=float, default=1.0)
    ap.add_argument("--threads", default="", help="PPU tids to draw (default: the six least blocked)")
    args = ap.parse_args()

    header, names, spans = load(args.trace)
    print(header)
    flips = [s[4] for s in spans if s[0] == FLIP_REQ]
    if len(flips) < 3:
        sys.exit("fewer than 3 flip requests in the window")
    frames = list(zip(flips, flips[1:]))
    durs = [b - a for a, b in frames]
    print(f"{len(frames)} frames, median {statistics.median(durs):.1f} ms "
          f"({1000 / statistics.median(durs):.1f} fps), min {min(durs):.1f}, max {max(durs):.1f}")

    # Per-frame totals, then the median of each.
    ppu = collections.defaultdict(lambda: collections.defaultdict(list))   # tid -> (kind, site) -> [ms per frame]
    spu_busy = collections.defaultdict(list)                                # spu -> [ms per frame]
    spu_wid = collections.defaultdict(float)                                # (wid) -> ms total
    spu_img = collections.defaultdict(float)
    rsx = collections.defaultdict(list)
    tids = sorted({s[1] for s in spans if s[0] in (SYSCALL, USLEEP)})
    spus = sorted({s[1] for s in spans if s[0] in (SPU_BUSY, SPU_WAIT)})
    lo = frames[0][0]
    j0 = 0
    for f0, f1 in frames:
        acc = collections.defaultdict(float)
        while j0 < len(spans) and spans[j0][5] < lo:
            j0 += 1
        for s in spans[j0:]:
            k, who, a, b, t0, t1 = s
            if t0 > f1:
                break
            o = overlap(t0, t1, f0, f1)
            if o <= 0 and k not in (FLIP_FIFO,):
                continue
            if k == USLEEP:
                acc[("ppu", who, "usleep", b)] += o
            elif k == SYSCALL:
                acc[("ppu", who, SYSCALL_NAMES.get(a, f"sc{a}"), b)] += o
            elif k == SPU_BUSY:
                acc[("spu", who)] += o
                spu_wid[b] += o
                spu_img[a] += o
            elif k in (SEMA, SEMA_GAVEUP):
                acc[("rsx", "acquire wait")] += o
            elif k == DRAIN:
                acc[("rsx", "drain")] += o
            elif k == PRESENT:
                acc[("rsx", "present")] += o
            elif k == GPU_ENCODE:
                acc[("rsx", "encode (present)" if a else "encode (flush)")] += o
            elif k == GPU_WAIT:
                acc[("rsx", GPU_WAIT_KIND.get(a, "gpu wait"))] += o
            elif k == READBACK:
                acc[("rsx", "readback")] += o
        for t in tids:
            keys = [key for key in acc if key[0] == "ppu" and key[1] == t]
            for key in keys:
                ppu[t][(key[2], key[3])].append(acc[key])
        for sp in spus:
            spu_busy[sp].append(acc[("spu", sp)])
        for what in RSX_ROWS:
            rsx[what].append(acc[("rsx", what)])
        lo = f0

    n = len(frames)
    med = statistics.median(durs)

    def mean(xs):
        return sum(xs) / n   # frames where it never happened count as 0

    print(f"\nPer frame (mean over {n} frames, ms; frame median {med:.1f}):")
    print("  PPU threads (time blocked, by kind and guest call site):")
    rows = []
    for t in tids:
        total = sum(mean(v) for v in ppu[t].values())
        rows.append((total, t))
    for total, t in sorted(rows, reverse=True)[:12]:
        nm = names.get(t, "?")
        sites = sorted(((mean(v), k) for k, v in ppu[t].items()), reverse=True)[:4]
        desc = ", ".join(f"{kind}@{site:08X} {ms:.1f}" for ms, (kind, site) in sites)
        print(f"    tid {t:3d} {nm[:28]:28s} blocked {total:5.1f}  ({desc})")
    print("  SPUs (busy):")
    for sp in spus:
        print(f"    spu {sp:3d} busy {mean(spu_busy[sp]):5.1f}  ({100 * mean(spu_busy[sp]) / med:.0f}%)")
    tot = sum(spu_wid.values()) or 1
    print("  SPU busy time by SPURS workload id (LS 0x1DC) / lifted image:")
    print("    " + ", ".join(f"wid {w:#x} {100 * v / tot:.0f}%" for w, v in sorted(spu_wid.items(), key=lambda x: -x[1])[:8]))
    print("    " + ", ".join(f"img {i} {100 * v / tot:.0f}%" for i, v in sorted(spu_img.items(), key=lambda x: -x[1])[:8]))
    print("  RSX / present thread:")
    for what in RSX_ROWS:
        print(f"    {what:20s} {mean(rsx[what]):5.1f}")

    # GPU execution per command buffer (headless commits wait for each one).
    ex = [s for s in spans if s[0] == GPU_EXEC and flips[0] <= s[5] <= flips[-1]]
    if ex:
        print("  Metal command buffers (GPU's own execution time):")
        for kind, label in ((1, "present"), (0, "flush")):
            xs = [s for s in ex if s[3] == kind]
            if not xs:
                continue
            ms = [s[5] - s[4] for s in xs]
            print(f"    {label:8s} {len(xs) / n:4.1f}/frame  gpu {statistics.median(ms):6.2f} ms median"
                  f"  ({sum(ms) / n:5.1f} ms/frame)  passes {statistics.median([s[1] for s in xs]):.0f}"
                  f"  depth resolves {statistics.median([s[2] & 0xFFFF for s in xs]):.0f}"
                  f"  colour copies {statistics.median([s[2] >> 16 for s in xs]):.0f}")

    # Timelines.
    if args.threads:
        draw = [int(x) for x in args.threads.split(",")]
    else:
        draw = [t for _, t in sorted(rows)[:6]]   # the threads that work, not the idle ones
    pick = [frames[len(frames) // 2 + i] for i in range(args.frames) if len(frames) // 2 + i < len(frames)]
    for f0, f1 in pick:
        cols = int((f1 - f0) / args.ms_per_col) + 1
        print(f"\nframe at {f0 / 1000:.3f} s, {f1 - f0:.1f} ms ({args.ms_per_col:g} ms/col)")
        lines = {}
        for t in draw:
            lines[("ppu", t)] = ["#"] * cols
        for sp in spus:
            lines[("spu", sp)] = [" "] * cols
        lines[("rsx",)] = [" "] * cols

        def paint(row, t0, t1, ch, prio=False):
            c0 = max(0, int((t0 - f0) / args.ms_per_col))
            c1 = min(cols - 1, int((t1 - f0) / args.ms_per_col))
            for c in range(c0, c1 + 1):
                if prio or row[c] in " #":
                    row[c] = ch

        for k, who, a, b, t0, t1 in spans:
            if t1 < f0 or t0 > f1:
                continue
            if k == USLEEP and ("ppu", who) in lines:
                paint(lines[("ppu", who)], t0, t1, ".", True)
            elif k == SYSCALL and ("ppu", who) in lines:
                paint(lines[("ppu", who)], t0, t1, "s", True)
            elif k == SPU_BUSY and ("spu", who) in lines:
                paint(lines[("spu", who)], t0, t1, "#")
            elif k in (SEMA, SEMA_GAVEUP):
                paint(lines[("rsx",)], t0, t1, "S", True)
            elif k == DRAIN:
                paint(lines[("rsx",)], t0, t1, "D")
            elif k == PRESENT:
                paint(lines[("rsx",)], t0, t1, "P", True)
            elif k == FLIP_FIFO:
                paint(lines[("rsx",)], t0, t0, "|", True)
        for k, who, a, b, t0, t1 in spans:   # second pass: what the drain/present was doing
            if t1 < f0 or t0 > f1:
                continue
            if k == GPU_ENCODE:
                paint(lines[("rsx",)], t0, t1, "E", True)
            elif k == GPU_WAIT:
                paint(lines[("rsx",)], t0, t1, "G", True)
            elif k == READBACK:
                paint(lines[("rsx",)], t0, t1, "R", True)
        for key, row in lines.items():
            if key[0] == "ppu":
                label = f"{key[1]:3d} {names.get(key[1], '?')[:14]:14s}"
            elif key[0] == "spu":
                label = f"spu {key[1]:<14d}"
            else:
                label = "rsx" + " " * 15
            print(f"  {label} {''.join(row)}")


if __name__ == "__main__":
    main()
