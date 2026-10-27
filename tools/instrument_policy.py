import re
p = r'D:/recomp/ps3games/simpsons/src/spu/spurs_policy.c'
s = open(p, encoding='utf-8').read()
if 'int g_pt' not in s:
    s = s.replace('#include "spurs_policy.h"', '#include "spurs_policy.h"\n#include <stdio.h>\nint g_pt;', 1)

def repl(m):
    addr = m.group(1)
    trace = '\n        if (g_pt < 200) { fprintf(stderr, "[pt] %s\\n"); g_pt++; }' % addr
    return m.group(0) + trace

s = re.sub(r'void spP_func_([0-9A-F]+)\(spu_context\* ctx\) \{', repl, s)
open(p, 'w', encoding='utf-8').write(s)
print('policy trace points:', s.count('[pt]'))
