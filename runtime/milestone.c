/*
 * ps3recomp - boot milestone log (see include/ps3emu/milestone.h)
 *
 * Fixed-capacity open-addressed table, never rehashed. Lookups are lock-free;
 * only the insert path takes a lock, and inserts are bounded by the number of
 * DISTINCT keys a title produces (a few hundred: its import set plus its syscall
 * set), not by how often it calls them. That is what keeps the hot path down to
 * a hash and an atomic add.
 *
 * Atomics are the __atomic builtins rather than C11 <stdatomic.h>, matching
 * runtime/platform/win32_compat.h: _Atomic and atomic_load_explicit are C-only
 * spellings and this tree is mixed C/C++. All four supported toolchains are
 * gcc or clang (clang-cl on Windows), so the builtins are always available.
 */
#include "ps3emu/milestone.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Capacity is a power of two so the probe can mask instead of divide. Sized well
 * above the largest real key set we have seen (VF5: 107 imports + ~60 syscalls;
 * flOw, the widest, is nearer 400 total). Overflow degrades to dropping new keys
 * with one warning rather than corrupting the log -- see ms_insert.
 * ponytail: fixed cap, no rehash. Raise the constant if a title ever warns. */
#define MS_CAP      2048u
#define MS_KEY_MAX    96

typedef struct {
    /* `state` is the synchronisation point: 0 = empty, 1 = reserved, 2 = key
     * readable. A reader that sees 2 with acquire ordering is guaranteed to see
     * the completed key bytes written before the releasing store. */
    int          state;
    unsigned     ord;
    unsigned     hits;
    char         key[MS_KEY_MAX];
} ms_slot;

static ms_slot   g_slots[MS_CAP];
static int       g_lock;              /* 0 = free, 1 = held (insert path only) */
static unsigned  g_next_ord;          /* guarded by g_lock */
static int       g_full_warned;       /* guarded by g_lock */

/* -1 = not yet resolved, 0 = disabled, 1 = enabled. */
static int       g_enabled = -1;
static FILE*     g_out;               /* guarded by g_lock */
static int       g_dumped;            /* guarded by g_lock */

/* Counts are the one part of the record a killed process would otherwise lose,
 * and "is it still drawing?" is exactly the question a blank or hung run asks.
 * Almost every gated title is killed on a timeout (expect = "timeout"), so an
 * atexit-only counts section was a section no port in the gate ever produced --
 * which left regress.py's DROPPED check, the one meant to catch rendering
 * stopping, with nothing at all to compare against.
 *
 * So the section is re-appended as the run goes and the reader takes the last
 * value for each key, newest block wins. The trigger is elapsed TIME, not call
 * count: a title that has stopped making progress -- the failure worth catching
 * -- makes almost no calls, and Rubber Ducky at 4 fps never reached 65536 of
 * them in a 60-second gate run at all. One-second granularity is plenty;
 * nothing here needs to be precise, only recent.
 * ponytail: time(NULL), not a monotonic clock. A wall-clock step just moves one
 * checkpoint. */
#define MS_CKPT_SECS  2
#define MS_TICK       1024u   /* how often the hot path bothers to look at all */
static unsigned  g_calls;             /* relaxed; only its low bits matter */
static time_t    g_last_ckpt;         /* guarded by g_lock */
static void ms_checkpoint(void);
static void ms_maybe_counts_locked(void);

/* ------------------------------------------------------------------------- */

static void ms_lock(void)
{
    while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE)) {
        /* Contention here is inserts racing inserts: bounded, brief, and only
         * during bring-up while keys are still new. A spin costs less than
         * dragging a platform mutex into a module whose whole job is to not
         * perturb guest thread timing. */
    }
}

static void ms_unlock(void) { __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE); }

static unsigned ms_hash(const char* s)
{
    unsigned h = 2166136261u;               /* FNV-1a */
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h;
}

/* Resolved once. Reading the env var on every call would dominate the hot path
 * that the rest of this file exists to keep cheap. */
static int ms_enabled(void)
{
    int e = __atomic_load_n(&g_enabled, __ATOMIC_RELAXED);
    if (e >= 0) return e;

    ms_lock();
    e = __atomic_load_n(&g_enabled, __ATOMIC_RELAXED);
    if (e < 0) {
        const char* path = getenv("PS3_MILESTONE_OUT");
        e = 0;
        if (path && *path) {
            g_out = fopen(path, "w");
            if (g_out) {
                fprintf(g_out, "# ps3recomp milestone log v1\n");
                fflush(g_out);
                atexit(ps3_ms_dump);
                e = 1;
            } else {
                fprintf(stderr, "[milestone] cannot open %s -- log disabled\n", path);
            }
        }
        __atomic_store_n(&g_enabled, e, __ATOMIC_RELAXED);
    }
    ms_unlock();
    return e;
}

/* Slow path: `key` was not found on the lock-free scan. Re-scan under the lock,
 * because a racing thread may have inserted it in between. */
static void ms_insert(const char* key, unsigned h)
{
    unsigned i;
    ms_lock();

    for (i = 0; i < MS_CAP; i++) {
        ms_slot* s = &g_slots[(h + i) & (MS_CAP - 1)];
        if (s->state == 0) {
            snprintf(s->key, sizeof s->key, "%s", key);
            s->ord  = ++g_next_ord;
            s->hits = 1;
            __atomic_store_n(&s->state, 2, __ATOMIC_RELEASE);
            if (g_out) {
                /* Appended and flushed as it happens, so a title killed on a
                 * timeout still leaves everything it reached. */
                fprintf(g_out, "%u\t%s\n", s->ord, s->key);
                fflush(g_out);
                /* A boot that dies early may never make MS_TICK calls; a new
                 * key is rare and already on the locked I/O path, so let it
                 * carry a checkpoint too. The time floor bounds the cost. */
                ms_maybe_counts_locked();
            }
            ms_unlock();
            return;
        }
        if (s->state == 2 && strcmp(s->key, key) == 0) {
            __atomic_fetch_add(&s->hits, 1u, __ATOMIC_RELAXED);
            ms_unlock();
            return;
        }
    }

    if (!g_full_warned) {
        g_full_warned = 1;
        fprintf(stderr, "[milestone] table full at %u keys -- dropping this key "
                        "and later ones; raise MS_CAP\n", MS_CAP);
    }
    ms_unlock();
}

void ps3_ms(const char* key)
{
    unsigned h, i;
    if (!key || !*key || !ms_enabled()) return;

    if ((__atomic_add_fetch(&g_calls, 1u, __ATOMIC_RELAXED) & (MS_TICK - 1)) == 0)
        ms_checkpoint();

    h = ms_hash(key);
    for (i = 0; i < MS_CAP; i++) {
        ms_slot* s = &g_slots[(h + i) & (MS_CAP - 1)];
        int st = __atomic_load_n(&s->state, __ATOMIC_ACQUIRE);
        if (st == 0) break;                       /* certainly absent -> insert */
        if (st == 2 && strcmp(s->key, key) == 0) {
            __atomic_fetch_add(&s->hits, 1u, __ATOMIC_RELAXED);
            return;                               /* the common case */
        }
        /* st == 1: a concurrent insert owns this slot. Probe past it; the
         * locked re-scan in ms_insert is what makes that safe. */
    }
    ms_insert(key, h);
}

void ps3_msf(const char* fmt, ...)
{
    char buf[MS_KEY_MAX];
    va_list ap;
    if (!ms_enabled()) return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ps3_ms(buf);
}

void ps3_ms_kv(const char* key, long long value)
{
    if (!key || !*key || !ms_enabled()) return;

    /* Written as it is set rather than held for the dump, for the same reason
     * keys are: most titles are killed on a timeout and never reach atexit,
     * and "how many functions did this build have" is exactly the fact you
     * want when a whole run has gone missing. Re-recording a key just appends
     * another line and the reader takes the last one, so last-value-wins
     * survives without keeping a table to hold it in. */
    ms_lock();
    if (g_out) { fprintf(g_out, "kv\t%s\t%lld\n", key, value); fflush(g_out); }
    ms_unlock();
}

/* Occurrence counts are reported as log-scale buckets, not exact numbers. Guest
 * thread scheduling makes an exact count differ run to run, so exact comparison
 * would fail constantly and teach everyone to ignore the gate. A regression
 * worth catching -- rendering stopping, a loop that no longer runs -- moves a
 * count by orders of magnitude, not by a handful.
 * ponytail: decade buckets; tighten to an exact count for a specific key if one
 * ever proves stable enough to be worth it. */
static const char* ms_bucket(unsigned n)
{
    if (n == 0)      return "0";
    if (n == 1)      return "1";
    if (n < 10)      return "2-9";
    if (n < 100)     return "10-99";
    if (n < 1000)    return "100-999";
    if (n < 10000)   return "1k-9k";
    if (n < 100000)  return "10k-99k";
    return "100k+";
}

/* Caller holds g_lock and has checked g_out. */
static void ms_write_counts_locked(void)
{
    /* One pass to index by ordinal, so the section reads in the same sequence
     * as the stream above it and the two line up under a diff. The old
     * ordinal-outer double loop was O(keys * MS_CAP), which was fine once at
     * exit and is not fine on a repeating checkpoint. */
    static unsigned short by_ord[MS_CAP + 1];
    unsigned i, ord;

    memset(by_ord, 0, sizeof by_ord);
    for (i = 0; i < MS_CAP; i++)
        if (g_slots[i].state == 2 && g_slots[i].ord <= MS_CAP)
            by_ord[g_slots[i].ord] = (unsigned short)i;

    fprintf(g_out, "# counts\n");
    for (ord = 1; ord <= g_next_ord && ord <= MS_CAP; ord++) {
        const ms_slot* s = &g_slots[by_ord[ord]];
        if (s->ord == ord)
            fprintf(g_out, "count\t%s\t%s\n", s->key, ms_bucket(s->hits));
    }
    fflush(g_out);
}

/* Periodic re-append, so a title killed in its frame loop still leaves counts.
 * Caller holds g_lock. */
static void ms_maybe_counts_locked(void)
{
    time_t now;
    if (!g_out || g_dumped) return;
    now = time(NULL);
    if (g_last_ckpt && (long)(now - g_last_ckpt) < MS_CKPT_SECS) return;
    g_last_ckpt = now;
    ms_write_counts_locked();
}

static void ms_checkpoint(void)
{
    ms_lock();
    ms_maybe_counts_locked();
    ms_unlock();
}

void ps3_ms_dump(void)
{
    if (ms_enabled() != 1) return;

    ms_lock();
    if (g_dumped || !g_out) { ms_unlock(); return; }
    g_dumped = 1;

    ms_write_counts_locked();
    fprintf(g_out, "# end\n");
    fflush(g_out);
    fclose(g_out);
    g_out = NULL;
    ms_unlock();
}
