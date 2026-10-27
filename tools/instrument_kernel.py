import re, sys
p = r'D:/recomp/ps3games/simpsons/src/spu/spurs_kernel.c'
s = open(p, encoding='utf-8').read()

def repl(m):
    addr = m.group(1)
    trace = '\n        if (g_kt < 300) { fprintf(stderr, "[kt] %s\\n"); g_kt++; }' % addr
    return m.group(0) + trace

s2 = re.sub(r'void spK_func_([0-9A-F]+)\(spu_context\* ctx\) \{', repl, s)
open(p, 'w', encoding='utf-8').write(s2)
print('instrumented functions:', s2.count('[kt]'))
