#!/usr/bin/env python3
"""Find HLE functions that dereference a pointer parameter as a HOST pointer.

The HLE ABI adapter (ppu_hle.cpp) passes a guest function's arguments straight
through as the raw PPC register values. A pointer parameter is therefore a
GUEST address. Dereferencing it directly writes to (or reads from) whatever
host address happens to share that number -- usually an access violation, and
occasionally silent corruption.

The convention that IS correct, spelled out in libs/video/cellGcmSys.c
(cellGcmGetConfiguration): translate through vm_base, or use vm_read*/vm_write*.

This scans for the mistake so a whole library can be fixed in one pass instead
of one crash at a time. It is a heuristic -- it reports candidates, it does not
edit anything.

    python tools/audit_guest_ptrs.py libs/            # everything
    python tools/audit_guest_ptrs.py libs/font        # one library

Output is one line per suspect function:

    libs/font/cellFont.c:412  cellFontOpenFontset  derefs: fontType, font
"""
import os
import re
import sys

# A function definition at column 0 returning a typical HLE type.
FUNC_RE = re.compile(
    r'^(?:s32|u32|s64|u64|int|void|float)\s+'      # return type
    r'([A-Za-z_][A-Za-z0-9_]*)\s*'                 # name
    r'\(([^)]*)\)\s*$',                            # params (single line)
    re.M)

# A pointer parameter: "const char* path", "CellFontType* fontType", "u32 *out"
PARAM_PTR_RE = re.compile(r'(?:const\s+)?[A-Za-z_][A-Za-z0-9_]*\s*\*+\s*([A-Za-z_][A-Za-z0-9_]*)\s*$')

# Uses that are already correct: the parameter is converted, not dereferenced.
SAFE_USE = re.compile(r'(?:uintptr_t\)\s*|vm_read\w*\(|vm_write\w*\(|gptr\(|gpath\(|guest_str\('
                      r'|g_ps3_guest_caller\(|yz_g2h\(|rtc_str\(|rtc_tick_read\(|rtc_tick_write\('
                      r'|GUEST_PTR\(|GUEST_EA\()')


def pointer_params(param_text):
    out = []
    for raw in param_text.split(','):
        raw = raw.strip()
        if not raw or raw == 'void':
            continue
        m = PARAM_PTR_RE.match(raw)
        if m:
            out.append(m.group(1))
    return out


def function_bodies(src):
    """Yield (name, params, start_line, body) for each column-0 definition."""
    for m in FUNC_RE.finditer(src):
        brace = src.find('{', m.end())
        if brace < 0:
            continue
        depth = 0
        i = brace
        while i < len(src):
            if src[i] == '{':
                depth += 1
            elif src[i] == '}':
                depth -= 1
                if depth == 0:
                    break
            i += 1
        yield (m.group(1), m.group(2), src.count('\n', 0, m.start()) + 1,
               src[brace:i])


# Host functions that DEREFERENCE a pointer argument. Passing a guest pointer
# bare to any of these is the same bug as *p, and is how cellFsRead shipped an
# entire sound bank to an untranslated address -- fread(buf,...) reads no
# differently from *buf, but the old checks only looked for *p and p->.
DEREFERENCING = (
    'strcpy strncpy strcat strncat strlen strcmp strncmp strchr strrchr strstr'
    ' strdup sprintf snprintf sscanf atoi atol strtol strtoul strtod'
    ' memcpy memmove memset memcmp memchr'
    ' fopen freopen fread fwrite fputs fgets remove rename mkdir stat'
).split()

def bare_deref_calls(body, param):
    """Lines calling a dereferencing host function with `param` passed bare."""
    out = []
    for fn in DEREFERENCING:
        pat = re.escape(fn) + r"\s*\([^;]*?(?<![\w.>])" + re.escape(param) + r"\s*[,)]"
        for m in re.finditer(pat, body):
            nlpos = body.rfind(chr(10), 0, m.start()) + 1
            endpos = body.find(chr(10), m.start())
            line = body[nlpos:] if endpos < 0 else body[nlpos:endpos]
            # Check the WHOLE call for a safe use, not just the line the call
            # starts on -- a translated argument frequently sits on a later
            # line, e.g.
            #     memcpy(s_data[slot].info.data,
            #            vm_base + GUEST_EA(info) + offsetof(T, data),
            # which is correct but reads as a bare `info` if you stop at the
            # first line.
            if not SAFE_USE.search(m.group(0)) and not SAFE_USE.search(line):
                out.append(line)
    return out

# Only GUEST-FACING entry points take guest pointers. An internal helper
# (rsx_process_method, rsx_fp_decompile, ...) is handed host pointers by its
# caller and translating those would be the actual bug. The firmware naming
# convention is the reliable discriminator: cell*/sce*/sys_*/_sys_* are reached
# from the guest through the NID table, everything else is ours.
ENTRY_PREFIXES = ('cell', 'sce', '_cell')

def is_guest_entry(name):
    # sys_* / _sys_* are lowercase by convention.
    if name.startswith('sys_') or name.startswith('_sys_'):
        return True
    # cell*/sce* are camelCase in the firmware and never contain an underscore:
    # cellFsOpen, sceNpInit. A lowercase letter after the prefix, or an
    # underscore anywhere, means the function is OURS -- cellfs_set_root_path
    # and cellGame_set_title_id are host-side configuration the port calls with
    # host strings, and translating their arguments would be the bug.
    for p in ENTRY_PREFIXES:
        if name.startswith(p) and len(name) > len(p):
            return name[len(p)].isupper() and '_' not in name
    return False

def param_is_translated(body, param):
    """True if the function reassigns `param` through a translation helper.

    The common correct idiom is a single conversion at the top:

        mutex = GUEST_PTR(mutex, CellSyncMutex*);

    after which every mutex->field is a HOST deref and perfectly fine. Flagging
    those produced most of the reported candidates -- cellSync.c alone accounted
    for 30 of them while being entirely correct.
    """
    # A named translator, or any helper following the house naming convention
    # (*_host / *_g2h / *_xlat / *_ptr), e.g. cellPamf's pamf_host().
    pat = (r"(?:^|[^\w])" + re.escape(param) + r"\s*=\s*"
           r"(?:GUEST_PTR|gptr|gpath|guest_str|vm_to_host|yz_g2h"
           r"|\w*_(?:host|g2h|xlat|ptr|str)|\w*_host_\w+)\s*\(")
    if re.search(pat, body):
        return True
    # An in-place translating MACRO reassigns the parameter just as much as an
    # "=" does, e.g. sysPrxForUser's  YZ_XLAT(lwmutex, sys_lwmutex_t_hle*);
    macro = r"(?:YZ_XLAT|GUEST_XLAT)\s*\(\s*" + re.escape(param) + r"\s*[,)]"
    if re.search(macro, body):
        return True
    # Rebound to a HOST object, the shape used where a function fills a struct
    # in pieces and publishes it once on the way out:
    #     u32 info_ea = GIF_EA(info);
    #     CellGifDecInfo host_info;
    #     info = &host_info;
    # Every info-> after that is a host deref.
    rebind = r"(?:^|[^\w])" + re.escape(param) + r"\s*=\s*&\s*\w"
    return re.search(rebind, body) is not None

def strip_comments(src):
    """Blank out comment text, keeping every newline so line numbers still match.

    Without this, a function whose comment EXPLAINS the bug reads as having it:
    sys_prx_get_module_id_by_name translates both of its pointers correctly and
    was reported anyway, on the strength of a comment saying "name/id arrive as
    raw GUEST effective addresses".
    """
    out = []
    i, n = 0, len(src)
    while i < n:
        two = src[i:i + 2]
        if two == '/*':
            j = src.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(''.join(c if c == '\n' else ' ' for c in src[i:j]))
            i = j
        elif two == '//':
            j = src.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i))
            i = j
        elif src[i] == '"' or src[i] == "'":
            q = src[i]
            j = i + 1
            while j < n and src[j] != q and src[j] != '\n':
                j += 2 if src[j] == '\\' else 1
            j = min(j + 1, n)
            out.append(src[i:j])
            i = j
        else:
            out.append(src[i])
            i += 1
    return ''.join(out)


def suspects_in(path):
    src = open(path, 'rb').read().decode('utf-8', 'replace').replace('\r\n', '\n')
    src = strip_comments(src)
    found = []
    for name, params, line, body in function_bodies(src):
        if not is_guest_entry(name):
            continue
        ptrs = pointer_params(params)
        if not ptrs:
            continue
        bad = []
        for p in ptrs:
            if param_is_translated(body, p):
                continue
            # p-> or *p, but not when the line also converts it safely
            uses = re.findall(r'^.*(?:\*\s*%s\b|\b%s\s*->).*$' % (p, p), body, re.M)
            uses = [u for u in uses if not SAFE_USE.search(u)]
            # A declaration like "CellFsStat* sb = (...)" inside the body is not
            # a use. Require a TYPE NAME before the star: the old pattern also
            # matched a bare "*out = (u32)x", which is a real deref through a
            # parameter and the single most common form of this bug. cellSail's
            # "*handle = (u32)i" was invisible because of it, and so was every
            # other out-param write of that shape -- the audit reported 0
            # suspects for a file full of them.
            uses = [u for u in uses
                    if not re.search(r'[A-Za-z_][A-Za-z0-9_]*\s*\*+\s*%s\s*=' % p, u)]
            uses += bare_deref_calls(body, p)
            if uses:
                bad.append(p)
        if bad:
            found.append((line, name, bad))
    return found


def main():
    roots = sys.argv[1:] or ['libs']
    total = 0
    files = []
    for root in roots:
        if os.path.isfile(root):
            files.append(root.replace(chr(92), '/'))
            continue
        for dirpath, _dirs, fs in os.walk(root):
            for fn in sorted(fs):
                if fn.endswith('.c'):
                    files.append(os.path.join(dirpath, fn).replace(chr(92), '/'))
    for path in files:
        for line, name, bad in suspects_in(path):
            print('%s:%d  %s  derefs: %s' % (path, line, name, ', '.join(bad)))
            total += 1
    if False:
      for root in roots:
        for dirpath, _dirs, files in os.walk(root):
            for fn in sorted(files):
                if not fn.endswith('.c'):
                    continue
                path = os.path.join(dirpath, fn).replace('\\', '/')
                for line, name, bad in suspects_in(path):
                    print('%s:%d  %s  derefs: %s' % (path, line, name, ', '.join(bad)))
                    total += 1
    print('\n%d suspect function(s)' % total)
    return 0


if __name__ == '__main__':
    sys.exit(main())
