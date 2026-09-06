#!/usr/bin/env python3
"""Build a call graph over lifted PPU output, and answer reachability questions.

One pass over the generated .cpp files builds caller<->callee maps and caches
them as JSON; after that "who reaches func_X?" is a query instead of a grep
across hundreds of megabytes.

    python callgraph.py <lifted-dir> --build
    python callgraph.py <lifted-dir> --callers func_0020B20C
    python callgraph.py <lifted-dir> --why    func_0020B20C   # path from a root
    python callgraph.py <lifted-dir> --roots                  # never-called funcs

`--why` is the one that matters during bring-up: a function that never runs is
either unreachable (no path from any root) or reachable only through an
indirect call the runtime is not making. This tells the two apart.
"""
import os, re, sys, json, collections

DEF = re.compile(r'^void (func_[0-9A-Fa-f]+)\(ppu_context\*')
CALL = re.compile(r'\b(func_[0-9A-Fa-f]+)\(ctx\)')
TRAMP = re.compile(r'g_trampoline_fn\s*=\s*\(void\(\*\)\(void\*\)\)(func_[0-9A-Fa-f]+)')

def build(d):
    callers = collections.defaultdict(set)   # callee -> callers
    callees = collections.defaultdict(set)   # caller -> callees
    defined = set()
    for fn in sorted(os.listdir(d)):
        if not fn.endswith('.cpp'):
            continue
        cur = None
        with open(os.path.join(d, fn), encoding='utf-8', errors='replace') as f:
            for line in f:
                m = DEF.match(line)
                if m:
                    cur = m.group(1); defined.add(cur); continue
                if cur is None:
                    continue
                for m in CALL.finditer(line):
                    t = m.group(1)
                    if t != cur:
                        callees[cur].add(t); callers[t].add(cur)
                # a lifted tail call is a real edge, just a deferred one
                for m in TRAMP.finditer(line):
                    t = m.group(1)
                    callees[cur].add(t); callers[t].add(cur)
    return defined, callers, callees

def load_or_build(d, rebuild=False):
    cache = os.path.join(d, '.callgraph.json')
    if os.path.exists(cache) and not rebuild:
        j = json.load(open(cache))
        return (set(j['defined']),
                {k: set(v) for k, v in j['callers'].items()},
                {k: set(v) for k, v in j['callees'].items()})
    defined, callers, callees = build(d)
    json.dump({'defined': sorted(defined),
               'callers': {k: sorted(v) for k, v in callers.items()},
               'callees': {k: sorted(v) for k, v in callees.items()}},
              open(cache, 'w'))
    print('built: %d functions, %d edges -> %s'
          % (len(defined), sum(len(v) for v in callees.values()), cache))
    return defined, callers, callees

def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    d = sys.argv[1]; args = sys.argv[2:]
    rebuild = '--build' in args
    defined, callers, callees = load_or_build(d, rebuild)

    def opt(name):
        return args[args.index(name) + 1] if name in args else None

    t = opt('--callers')
    if t:
        c = sorted(callers.get(t, ()))
        print('%s: %d direct callers' % (t, len(c)))
        for x in c: print('   ', x)

    t = opt('--callees')
    if t:
        for x in sorted(callees.get(t, ())): print('   ', x)

    t = opt('--why')
    if t:
        # walk up to any function that itself has no callers (a root)
        seen = {t}; parent = {}; frontier = [t]; roots = []
        while frontier:
            nxt = []
            for n in frontier:
                ups = callers.get(n, ())
                if not ups:
                    roots.append(n); continue
                for u in ups:
                    if u not in seen:
                        seen.add(u); parent[u] = n; nxt.append(u)
            frontier = nxt
        print('%s: %d functions can reach it; %d root(s)' % (t, len(seen) - 1, len(roots)))
        for r in roots[:6]:
            path = [r]; cur = r
            while cur in parent:
                cur = parent[cur]; path.append(cur)
            print('   root %s  ->  %s' % (r, ' -> '.join(path[1:]) or t))

    if '--roots' in args:
        r = [f for f in defined if not callers.get(f)]
        print('%d functions with no direct caller (entry points, callbacks, dead code)' % len(r))
        for x in sorted(r)[:20]: print('   ', x)

if __name__ == '__main__':
    main()
