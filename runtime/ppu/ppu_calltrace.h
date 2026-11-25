/*
 * ps3recomp - firmware call trace (JSONL), for differential tracing.
 *
 * PS3_CALLTRACE=<file> writes one JSON line per firmware call the title makes:
 * every HLE import (ps3_hle_call, by NID) and every lv2 syscall (lv2_syscall,
 * by number), with the guest thread, a per-thread sequence number, the eight
 * argument registers, the return value and the guest memory the call wrote
 * through its pointer arguments. tools/hle_diff.py compares it with the same
 * trace taken from RPCS3 (tools/rpcs3_probe/), so a firmware difference is
 * found by diff instead of by hand. Schema and options: docs/HLE_TRACE_DIFF.md.
 *
 *   PS3_CALLTRACE=<file>        on; the file is truncated at start-up
 *   PS3_CALLTRACE_FILTER=<list> comma list: NID (0x1234ABCD), name or name
 *                               prefix (cellGcm*), sc (all syscalls), sc:N;
 *                               a leading '-' excludes. Default: everything.
 *   PS3_CALLTRACE_WIN=<bytes>   memory watched past each pointer argument
 *                               (default 256, 0 = no memory capture)
 *   PS3_CALLTRACE_MAX=<n>       stop after n records
 *
 * Off (no PS3_CALLTRACE), the cost is one load and a not-taken branch per
 * call. Game-agnostic.
 */
#ifndef PPU_CALLTRACE_H
#define PPU_CALLTRACE_H

#include <stdint.h>

struct ppu_context;

#ifdef __cplusplus
extern "C" {
#endif

/* -1 = not yet read from the environment, 0 = off, 1 = on. */
extern int g_ps3_calltrace;
int ps3_calltrace_init(void);   /* reads the environment; returns the new state */

enum { PS3_CT_HLE = 1, PS3_CT_SYSCALL = 2 };

/* One call in flight. Lives on the dispatcher's stack; `on` is the only field
 * touched when tracing is off. */
typedef struct ps3_ct_call {
    int      on;
    int      kind, depth;
    uint32_t id;          /* NID or syscall number */
    uint64_t seq;
    uint64_t t0_us;
    uint32_t lr, frame;
    uint64_t args[8];
    uint8_t* snap;        /* pre-call windows, one per argument (thread-local pool) */
} ps3_ct_call;

void ps3_ct_enter(ps3_ct_call* c, int kind, uint32_t id, struct ppu_context* ctx);
void ps3_ct_leave(ps3_ct_call* c, struct ppu_context* ctx);

#ifdef __cplusplus
} /* extern "C" */

/* RAII form for the dispatchers: constructed first, so it is destroyed last,
 * after every other exit guard (the TOC restore) has run and r3 is final. */
struct ps3_ct_scope {
    ps3_ct_call c;
    struct ppu_context* ctx;
    ps3_ct_scope(int kind, uint32_t id, struct ppu_context* x) : ctx(x)
    {
        c.on = 0;
        int s = g_ps3_calltrace;
        if (__builtin_expect(s != 0, 0)) {
            if (s < 0) s = ps3_calltrace_init();
            if (s > 0) ps3_ct_enter(&c, kind, id, x);
        }
    }
    ~ps3_ct_scope() { if (__builtin_expect(c.on, 0)) ps3_ct_leave(&c, ctx); }
    ps3_ct_scope(const ps3_ct_scope&) = delete;
    ps3_ct_scope& operator=(const ps3_ct_scope&) = delete;
};
#endif

#endif /* PPU_CALLTRACE_H */
