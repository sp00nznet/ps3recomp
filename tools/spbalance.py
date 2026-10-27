"""spbalance.py - find stack-imbalance bugs in lifted PPU output.

Lifted functions form "logical functions": an entry (with a prologue
`gpr[1] += -N`) plus tail-entry continuations reached via `g_trampoline_fn`.
Only the entry allocates the frame; some continuation restores it before a real
`return`. A balanced logical function restores exactly what the entry allocated
on every real-return path.

This walks each entry's trampoline+goto reachable set, tracking cumulative sp
allocation, and flags real returns whose restore != cumulative alloc. Those are
mis-split / mis-lifted functions that corrupt sp when dispatched.

Usage: python spbalance.py <dir-with-ppu_recomp_*.cpp> [--focus func_XXXXXXXX]
"""
import glob, os, re, sys, collections

ALLOC = re.compile(r'ctx->gpr\[1\] \+= -(0x[0-9A-Fa-f]+);')
REST  = re.compile(r'ctx->gpr\[1\] = \(int64_t\)\(int32_t\)\(ctx->gpr\[1\] \+ (0x[0-9A-Fa-f]+)\)')
SIG   = re.compile(r'^void (func_[0-9A-Fa-f]{8})\(ppu_context\* ctx\) \{')
TRAMP = re.compile(r'g_trampoline_fn = \(void\(\*\)\(void\*\)\)(func_[0-9A-Fa-f]{8})')

def parse(d):
    """name -> dict(alloc, real_restores=[ints], tramps=set(names), has_real_ret)"""
    funcs = {}
    for path in sorted(glob.glob(os.path.join(d, 'ppu_recomp_*.cpp'))):
        cur = None; lines = []
        def flush(name, body):
            alloc = 0
            for m in ALLOC.finditer('\n'.join(body)):
                alloc += int(m.group(1), 16)
            tramps = set(TRAMP.findall('\n'.join(body)))
            real = []
            # a `return;` not on a line that set g_trampoline_fn is a real return;
            # capture the most recent restore before it
            last_rest = 0
            for ln in body:
                rm = REST.search(ln)
                if rm: last_rest = int(rm.group(1), 16)
                if re.search(r'\breturn;', ln) and 'g_trampoline_fn' not in ln:
                    real.append(last_rest); last_rest = 0
            funcs[name] = dict(alloc=alloc, real=real, tramps=tramps,
                               has_real=bool(real))
        for line in open(path, encoding='utf-8', errors='replace'):
            m = SIG.match(line)
            if m:
                if cur: flush(cur, lines)
                cur = m.group(1); lines = []
            elif cur is not None:
                if line.startswith('}'):
                    flush(cur, lines); cur = None; lines = []
                else:
                    lines.append(line)
    return funcs

def check(funcs, focus=None):
    bugs = []
    entries = [n for n, f in funcs.items() if f['alloc'] > 0]
    if focus:
        entries = [focus] if focus in funcs else []
    for e in entries:
        # BFS through trampolines; only the entry contributes alloc (continuations
        # are tail-entries with alloc 0). Track real returns reached.
        seen = set([e]); q = collections.deque([e]); alloc = funcs[e]['alloc']
        chain_real = []
        steps = 0
        while q and steps < 5000:
            steps += 1
            n = q.popleft()
            fn = funcs.get(n)
            if not fn: continue
            for r in fn['real']:
                chain_real.append((n, r))
            for t in fn['tramps']:
                if t not in seen and funcs.get(t) and funcs[t]['alloc'] == 0:
                    seen.add(t); q.append(t)
        for (rn, r) in chain_real:
            if r != alloc:
                bugs.append((e, alloc, rn, r, len(seen)))
    return bugs

def main():
    d = sys.argv[1] if len(sys.argv) > 1 else '.'
    focus = None
    if '--focus' in sys.argv:
        focus = sys.argv[sys.argv.index('--focus') + 1]
    funcs = parse(d)
    print(f"parsed {len(funcs)} functions; {sum(1 for f in funcs.values() if f['alloc']>0)} with a prologue")
    bugs = check(funcs, focus)
    # dedup by entry
    by_entry = collections.OrderedDict()
    for e, a, rn, r, sz in bugs:
        by_entry.setdefault(e, (a, rn, r, sz))
    print(f"IMBALANCED logical functions (restore != entry alloc): {len(by_entry)}")
    for i, (e, (a, rn, r, sz)) in enumerate(by_entry.items()):
        if i >= 40 and not focus: break
        print(f"  {e}: alloc=0x{a:X} but {rn} returns restoring 0x{r:X} "
              f"(leak 0x{a-r:X}); chain size {sz}")

if __name__ == '__main__':
    main()
