"""Find HLE entry points that call ANOTHER HLE entry point.

An entry point translates its pointer arguments as guest EAs. Calling one from
inside the library hands it host pointers -- a local buffer, the address of a
stack variable -- and it translates them a second time, writing somewhere else
entirely. cellHttpUtilFormUrlEncode did exactly this to cellHttpUtilEscapeUri.

Not every hit is a bug: a forwarding wrapper that passes its OWN untranslated
guest arguments straight through is fine. The ones to look at are calls that mix
in a local address (&x) or a buffer the caller made.

    python tools/audit_entry_calls.py libs/
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import audit_guest_ptrs as A


def main():
    roots = sys.argv[1:] or ['libs']
    files = []
    for root in roots:
        if os.path.isfile(root):
            files.append(root)
            continue
        for dirpath, _d, fs in os.walk(root):
            files += [os.path.join(dirpath, f) for f in sorted(fs) if f.endswith('.c')]

    # every entry point defined anywhere in the tree
    entries = set()
    bodies = {}
    for path in files:
        src = A.strip_comments(
            open(path, 'rb').read().decode('utf-8', 'replace').replace('\r\n', '\n'))
        bodies[path] = list(A.function_bodies(src))
        for name, _p, _l, _b in bodies[path]:
            if A.is_guest_entry(name):
                entries.add(name)

    total = 0
    risky_n = 0
    for path in files:
        for name, params, line, body in bodies[path]:
            if not A.is_guest_entry(name):
                continue
            mine = A.pointer_params(params)
            for m in re.finditer(r'\b([A-Za-z_]\w*)\s*\(([^;]*?)\)', body):
                callee, args = m.group(1), m.group(2)
                if callee == name or callee not in entries:
                    continue
                why = []
                # A host address the caller made: the callee will translate it.
                if re.search(r'(?<![\w])&\w', args):
                    why.append('passes a local address')
                # A parameter the caller ALREADY translated, handed to a callee
                # that translates again -- vm_base gets added twice.
                for p in mine:
                    if re.search(r'(?<![\w.>])' + re.escape(p) + r'(?![\w])', args) \
                       and A.param_is_translated(body, p):
                        why.append('passes already-translated `%s`' % p)
                print('%s:%d  %s -> %s%s' % (
                    path.replace(os.sep, '/'), line, name, callee,
                    ('   <-- ' + '; '.join(why)) if why else ''))
                total += 1
                risky_n += 1 if why else 0
    print('\n%d entry-point-to-entry-point call(s), %d worth a look'
          % (total, risky_n))


if __name__ == '__main__':
    main()
