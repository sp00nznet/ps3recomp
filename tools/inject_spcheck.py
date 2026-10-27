"""inject_spcheck.py - instrument lifted chunks for the dynamic stack-imbalance audit.

Adds a SPCALL macro (-> the spcall() wrapper in vm_bridge) after each chunk's
DRAIN_TRAMPOLINE define, and rewrites every `func_X(ctx); DRAIN_TRAMPOLINE(ctx);`
bl call-site to `SPCALL(func_X, ctx);`. The wrapper checks sp before/after each
subroutine call and logs [SPLEAK] on mismatch (a real recompilation stack bug,
zero false positives). The host must define:
    extern "C" void spcall(void(*)(void*), ppu_context*, const char*);
    extern "C" void sp_leak_report(const char*, unsigned, unsigned);
Idempotent. Usage: python inject_spcheck.py <dir-with-ppu_recomp_*.cpp> [--revert]
"""
import glob, os, re, sys

MACRO = ('\nextern "C" void spcall(void(*)(void*), ppu_context*, const char*);\n'
         '#define SPCALL(fn, ctx) spcall((void(*)(void*))fn, (ctx), #fn)\n')
DRAIN = '#define DRAIN_TRAMPOLINE(ctx)'
CALL  = re.compile(r'(func_[0-9A-Fa-f]{8})\(ctx\); DRAIN_TRAMPOLINE\(ctx\);')

def inject(path):
    t = open(path, encoding='utf-8', errors='replace').read()
    if 'SPCALL' not in t:
        # insert macro right after the DRAIN_TRAMPOLINE define line
        lines = t.split('\n')
        for i, l in enumerate(lines):
            if l.startswith(DRAIN):
                lines[i] = l + MACRO
                break
        t = '\n'.join(lines)
    t, n = CALL.subn(r'SPCALL(\1, ctx);', t)
    open(path, 'w', encoding='utf-8').write(t)
    return n

def revert(path):
    t = open(path, encoding='utf-8', errors='replace').read()
    t = re.sub(r'\nextern "C" void spcall\(.*?#fn\)\n', '\n', t, flags=re.S)
    t, n = re.subn(r'SPCALL\((func_[0-9A-Fa-f]{8}), ctx\);',
                   r'\1(ctx); DRAIN_TRAMPOLINE(ctx);', t)
    open(path, 'w', encoding='utf-8').write(t)
    return n

def main():
    d = sys.argv[1]
    rev = '--revert' in sys.argv
    total = 0
    for f in sorted(glob.glob(os.path.join(d, 'ppu_recomp_[0-9][0-9][0-9].cpp'))):
        n = revert(f) if rev else inject(f)
        total += n
    print(f"{'reverted' if rev else 'instrumented'} {total} call-sites across chunks")

if __name__ == '__main__':
    main()
