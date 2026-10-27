#!/usr/bin/env python3
"""Self-check for audit_guest_ptrs.py.

The audit is only worth anything if a clean report means the bug is gone rather
than that the heuristic stopped matching -- which happened three times while the
sweep was running. This pins both directions: the shapes it must report, and the
correct forms it must stay quiet about.

    python tools/test_audit_guest_ptrs.py
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import audit_guest_ptrs as A

FIXTURE = '''
s32 cellTestScalarOut(u32* out)
{
    *out = 1;
    return 0;
}

s32 cellTestBareString(const char* p)
{
    strcpy(g_buf, p);
    return 0;
}

s32 cellTestStructDeref(CellFooParam* param)
{
    g_width = param->width;
    return 0;
}

s32 cellTestMultilineCall(const CellFooInfo* info)
{
    memcpy(g_dst,
           info,
           16);
    return 0;
}

s32 cellTestTranslated(CellFooParam* param)
{
    param = GUEST_PTR(param, CellFooParam*);
    g_width = param->width;
    return 0;
}

s32 cellTestWritten(u32* out)
{
    vm_write32((u32)(uintptr_t)out, 1);
    return 0;
}

s32 cellTestMultilineSafe(const CellFooInfo* info)
{
    memcpy(g_dst,
           vm_base + GUEST_EA(info),
           16);
    return 0;
}

s32 cellTestCommentOnly(const char* name)
{
    /* name arrives as a raw GUEST address; strcpy(dst, name) would be wrong */
    return 0;
}

/* Ours, not firmware: host-side config called with HOST strings. Translating
 * its argument would be the actual bug. */
void cellfoo_set_path(const char* path)
{
    strncpy(g_root, path, 64);
}

/* An internal helper takes host pointers too. */
void rsx_process_thing(rsx_state* state)
{
    state->count++;
}
'''

MUST_REPORT = {
    'cellTestScalarOut',
    'cellTestBareString',
    'cellTestStructDeref',
    'cellTestMultilineCall',
}
MUST_NOT_REPORT = {
    'cellTestTranslated',
    'cellTestWritten',
    'cellTestMultilineSafe',
    'cellTestCommentOnly',
    'cellfoo_set_path',
    'rsx_process_thing',
}


def main():
    fd, path = tempfile.mkstemp(suffix='.c')
    try:
        with os.fdopen(fd, 'w') as f:
            f.write(FIXTURE)
        found = {name for _line, name, _bad in A.suspects_in(path)}
    finally:
        os.unlink(path)

    missed = MUST_REPORT - found
    spurious = MUST_NOT_REPORT & found
    for name in sorted(missed):
        print('MISSED (real bug the audit no longer sees): %s' % name)
    for name in sorted(spurious):
        print('SPURIOUS (correct code reported as a bug):  %s' % name)
    if missed or spurious:
        print('\nFAIL: %d missed, %d spurious' % (len(missed), len(spurious)))
        return 1
    print('ok: %d positives caught, %d correct forms ignored'
          % (len(MUST_REPORT), len(MUST_NOT_REPORT)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
