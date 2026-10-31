/*
 * ps3_trace -- a frame timeline in Chrome / Perfetto trace-event JSON.
 *
 * PS3RECOMP_TRACE=<file.json> records, per host thread, spans for SPU job and
 * workload runs and the waits inside them, RSX walker drain passes and the
 * waits that stop them, and PPU guest waits (lwmutex, lwcond, mutex, cond,
 * semaphore, event queue / flag, cellGcm flip and finish waits), plus GCM
 * flips as instant events. Open the file in ui.perfetto.dev or
 * chrome://tracing. Unset (the default), every hook is one load and one
 * predictable branch; nothing is allocated and no thread is started.
 *
 * Where PS3_FTRACE answers one question with its own report, this is the
 * general view: every host thread is a track, a wait nests inside the run it
 * stalls, and the stock trace viewers do the rest.
 *
 * Recording. Each host thread appends fixed-size records to its own
 * single-producer ring; a hook never takes a lock or makes a system call once
 * its thread has a ring. A flusher thread, started with the first hook, wakes
 * every few ms and writes all rings out once PS3RECOMP_TRACE_FLUSH_MB (default
 * 4) of records are pending, or sooner if any one ring is half full; exit
 * writes whatever is left and closes the array. A ring that fills anyway drops
 * new records and counts them -- the count is in the process metadata.
 *
 * The file is the JSON array form, so one cut short by a crash still loads in
 * the viewers (they accept a missing "]"); a clean exit makes it strict JSON.
 * Timestamps are CLOCK_MONOTONIC (QueryPerformanceCounter on Windows), in
 * microseconds from the first hook, written with three exact decimals.
 *
 * Knobs:
 *   PS3RECOMP_TRACE_FLUSH_MB  pending records that trigger a write (4, may be fractional)
 *   PS3RECOMP_TRACE_RING_MB   per-thread ring size (2, rounded up to a power of two)
 *   PS3RECOMP_TRACE_MIN_US    shortest span kept (0: every span)
 *
 * Header-only, like ps3_ftrace.h and for the same reason: the hooked files are
 * also compiled on their own by self-contained tests, and a new link
 * dependency would break each of them. The state is one weak/selectany object
 * the linker folds into a single copy, and the per-thread ring is found through
 * one process-wide TLS key, so every translation unit shares the same rings.
 *
 * Hooks:
 *   PS3_TRACE_SCOPE(ev, a, b);           span from here to the end of the block
 *   PS3_TRACE_SCOPE_IF(cond, ev, a, b);  the same, only if cond holds here (a
 *                                        wait that will block); cond is not
 *                                        evaluated when tracing is off
 *   t0 = ps3_trace_begin(); ... ps3_trace_end(t0, ev, a, b);
 *   ps3_trace_instant(ev, a, b);
 *   ps3_trace_poll(ev, a) / ps3_trace_poll_end(ev)
 *                                        a guest spin seen as repeated calls:
 *                                        calls closer than 2 ms become one span
 *   ps3_trace_thread_name(name)          the calling thread's track name
 * A span whose thread longjmps past its end (an SPU halt) is simply not
 * recorded; nothing is left half-written.
 */
#ifndef PS3EMU_PS3_TRACE_H
#define PS3EMU_PS3_TRACE_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  include <windows.h>
#else
#  include <pthread.h>
#  include <sched.h>
#  include <time.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* What a record is. a and b are the two numbers the hook passes; their names
 * in the JSON "args" are in ps3_tr_desc_of() below. */
enum ps3_tr_ev {
    PS3_TR_NONE = 0,
    /* SPU runs */
    PS3_TR_SPU_RUN,            /* one lifted SPU run (spu_run_with_halt): a = image id, b = SPU id */
    PS3_TR_SPU_INTERP,         /* one interpreted SPU run: a = entry LS pc, b = args EA            */
    PS3_TR_SPU_JOB,            /* a SPURS job: a = job descriptor EA, b = image id                 */
    PS3_TR_SPU_POLICY,         /* a SPURS policy module quantum: a = image id, b = workload id    */
    PS3_TR_SPU_WORKLOAD,       /* a dispatched SPU workload/task: a = image id, b = args EA        */
    /* SPU waits */
    PS3_TR_SPU_MBOX_WAIT,      /* rdch SPU_RdInMbox blocked: a = channel, b = LS pc                 */
    PS3_TR_SPU_SIGNAL_WAIT,    /* rdch SPU_RdSigNotify1/2 blocked: a = channel, b = LS pc           */
    PS3_TR_SPU_EVENT_WAIT,     /* rdch SPU_RdEventStat blocked: a = event mask, b = LS pc           */
    PS3_TR_SPU_RESV_WAIT,      /* the same, waiting for a lost reservation: a = line EA, b = LS pc  */
    PS3_TR_SPU_OUT_MBOX_WAIT,  /* wrch SPU_WrOutMbox stalled on a full mailbox: a = value, b = LS pc */
    PS3_TR_SPU_LOCKLINE_RETRY, /* first failed PUTLLC to the next success: a = line EA, b = failures */
    PS3_TR_SPU_TAG_WAIT,       /* MFC tag-status wait not satisfied on entry: a = tag mask, b = done */
    /* RSX */
    PS3_TR_RSX_DRAIN,          /* one FIFO walker pass: a = get before, b = get after (IO offsets)   */
    PS3_TR_RSX_SEMA_WAIT,      /* async: walker held on a semaphore acquire: a = label offset, b = value */
    PS3_TR_RSX_PUT_WAIT,       /* async: walker caught up or parked, waiting for put: a = get, b = put */
    /* PPU guest waits */
    PS3_TR_PPU_LWMUTEX,        /* a = lwmutex EA, b = timeout us                    */
    PS3_TR_PPU_LWCOND,         /* a = lwcond EA                                     */
    PS3_TR_PPU_COND,           /* a = cond id,    b = timeout us                    */
    PS3_TR_PPU_SEMAPHORE,      /* a = semaphore id, b = timeout us                  */
    PS3_TR_PPU_EVENT_QUEUE,    /* a = queue id,   b = timeout us                    */
    PS3_TR_PPU_EVENT_FLAG,     /* a = flag id,    b = timeout us                    */
    PS3_TR_PPU_GCM_WAIT_FLIP,  /* cellGcmSetWaitFlip on a flip still waiting        */
    PS3_TR_PPU_GCM_FLIP_POLL,  /* cellGcmGetFlipStatus answered WAITING, run: b = polls */
    PS3_TR_PPU_GCM_FINISH,     /* spin-reads of the ref register, run: a = last ref, b = polls */
    /* instants */
    PS3_TR_GCM_FLIP,           /* the RSX reached a flip: a = buffer                */
    PS3_TR_GCM_FLIP_REQ,       /* the guest queued a flip: a = buffer, b = 1 if through the FIFO */
    PS3_TR_GCM_FLIP_DONE,      /* the vblank tick completed the outstanding flip: a = request number */
    PS3_TR_EV_COUNT
};

enum { PS3_TR_PH_SPAN = 0, PS3_TR_PH_INSTANT = 1, PS3_TR_PH_ASYNC = 2 };

typedef struct {
    const char* name;
    const char* cat;
    uint8_t ph;               /* PS3_TR_PH_* */
    uint8_t hex;              /* bit 0: a is an address, bit 1: b is */
    const char* a_name;       /* NULL: not written */
    const char* b_name;
} ps3_tr_desc;

static inline const ps3_tr_desc* ps3_tr_desc_of(uint32_t ev)
{
    static const ps3_tr_desc d[PS3_TR_EV_COUNT] = {
        { "none",               "trace", PS3_TR_PH_SPAN,    0, 0, 0 },
        { "spu.run",            "spu",   PS3_TR_PH_SPAN,    0, "image", "spu" },
        { "spu.interp",         "spu",   PS3_TR_PH_SPAN,    3, "entry", "args" },
        { "spu.job",            "spu",   PS3_TR_PH_SPAN,    1, "job", "image" },
        { "spu.policy_module",  "spu",   PS3_TR_PH_SPAN,    0, "image", "wid" },
        { "spu.workload",       "spu",   PS3_TR_PH_SPAN,    2, "image", "args" },
        { "spu.mbox_wait",      "spu",   PS3_TR_PH_SPAN,    2, "ch", "pc" },
        { "spu.signal_wait",    "spu",   PS3_TR_PH_SPAN,    2, "ch", "pc" },
        { "spu.event_wait",     "spu",   PS3_TR_PH_SPAN,    3, "mask", "pc" },
        { "spu.resv_lost_wait", "spu",   PS3_TR_PH_SPAN,    3, "ea", "pc" },
        { "spu.out_mbox_wait",  "spu",   PS3_TR_PH_SPAN,    3, "value", "pc" },
        { "spu.lockline_retry", "spu",   PS3_TR_PH_SPAN,    1, "ea", "failures" },
        { "spu.tag_wait",       "spu",   PS3_TR_PH_SPAN,    3, "mask", "completed" },
        { "rsx.drain",          "rsx",   PS3_TR_PH_SPAN,    3, "get0", "get" },
        { "rsx.sema_wait",      "rsx",   PS3_TR_PH_ASYNC,   3, "offset", "value" },
        { "rsx.put_wait",       "rsx",   PS3_TR_PH_ASYNC,   3, "get", "put" },
        { "ppu.lwmutex",        "ppu",   PS3_TR_PH_SPAN,    1, "lwmutex", "timeout_us" },
        { "ppu.lwcond",         "ppu",   PS3_TR_PH_SPAN,    1, "lwcond", 0 },
        { "ppu.cond",           "ppu",   PS3_TR_PH_SPAN,    0, "id", "timeout_us" },
        { "ppu.semaphore",      "ppu",   PS3_TR_PH_SPAN,    0, "id", "timeout_us" },
        { "ppu.event_queue",    "ppu",   PS3_TR_PH_SPAN,    0, "queue", "timeout_us" },
        { "ppu.event_flag",     "ppu",   PS3_TR_PH_SPAN,    0, "id", "timeout_us" },
        { "ppu.gcm_wait_flip",  "ppu",   PS3_TR_PH_SPAN,    0, 0, 0 },
        { "ppu.gcm_flip_poll",  "ppu",   PS3_TR_PH_SPAN,    0, 0, "polls" },
        { "ppu.gcm_finish",     "ppu",   PS3_TR_PH_SPAN,    1, "ref", "polls" },
        { "gcm.flip",           "gcm",   PS3_TR_PH_INSTANT, 0, "buffer", 0 },
        { "gcm.flip_request",   "gcm",   PS3_TR_PH_INSTANT, 0, "buffer", "fifo" },
        { "gcm.flip_done",      "gcm",   PS3_TR_PH_INSTANT, 0, "request", 0 },
    };
    return &d[ev < (uint32_t)PS3_TR_EV_COUNT ? ev : 0];
}

#if defined(_MSC_VER)
#  define PS3_TR_SHARED __declspec(selectany)
#else
#  define PS3_TR_SHARED __attribute__((weak))
#endif
#if defined(__GNUC__) || defined(__clang__)
#  define PS3_TR_LIKELY(x)   __builtin_expect(!!(x), 1)
#  define PS3_TR_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#  define PS3_TR_LIKELY(x)   (x)
#  define PS3_TR_UNLIKELY(x) (x)
#endif
#if defined(__cplusplus)
#  define PS3_TR_TLS thread_local
#elif defined(_MSC_VER) && !defined(__clang__)
#  define PS3_TR_TLS __declspec(thread)
#else
#  define PS3_TR_TLS _Thread_local
#endif

enum {
    PS3_TR_MAX_RINGS = 1024,       /* host threads ever seen; more are not traced */
    PS3_TR_POLL_MS   = 5,          /* flusher wake-up period */
    PS3_TR_POLL_GAP_NS = 2000000,  /* ps3_trace_poll: calls closer than this are one span */
};

typedef struct { uint64_t t0, t1; uint32_t a, b, ev, pad; } ps3_tr_rec;   /* 32 bytes */

typedef struct {
    ps3_tr_rec* buf;            /* allocated by the owner on its first record */
    uint64_t mask;
    uint64_t head;              /* next record; written by the owner only */
    uint64_t tail;              /* first unwritten-out record; written by the flusher only */
    uint64_t dropped;           /* owner */
    uint32_t tid;               /* the "tid" in the JSON, 1.. in order of first use */
    uint32_t name_gen;          /* bumped by the owner after each rename */
    uint32_t name_out;          /* flusher: the generation last written out */
    int alloc_failed;
    char name[48];
    /* ps3_trace_poll's open run; owner only */
    uint32_t poll_ev, poll_a, poll_n;
    uint64_t poll_t0, poll_t1;
} ps3_tr_ring;

typedef struct {
    int on;                     /* 0 unknown, 1 off (or finished), 2 recording, 3 initialising */
    int lock;                   /* the consumer side: flusher and shutdown */
    int first;                  /* no event written yet (comma placement) */
    uint32_t nrings, next_tid;
    uint64_t ring_recs, flush_recs, min_ns, base, written;
    FILE* f;
#ifdef _WIN32
    DWORD tls;
#else
    pthread_key_t tls;
#endif
    char path[1024];
    ps3_tr_ring* rings[PS3_TR_MAX_RINGS];
} ps3_tr_state;

#ifdef __cplusplus
PS3_TR_SHARED ps3_tr_state g_ps3_tr = {};
#else
PS3_TR_SHARED ps3_tr_state g_ps3_tr = { 0 };
#endif

static inline uint64_t ps3_trace_now(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    /* Seconds and remainder apart: counter * 1e9 overflows after hours. */
    return (uint64_t)(c.QuadPart / f.QuadPart) * 1000000000ull +
           (uint64_t)(c.QuadPart % f.QuadPart) * 1000000000ull / (uint64_t)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static inline void ps3_tr_sleep_ms(unsigned ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec d = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
    nanosleep(&d, NULL);
#endif
}

static inline void ps3_tr_lock(void)
{
    while (__atomic_exchange_n(&g_ps3_tr.lock, 1, __ATOMIC_ACQUIRE)) {
#ifdef _WIN32
        SwitchToThread();
#else
        sched_yield();
#endif
    }
}
static inline int ps3_tr_trylock(void) { return !__atomic_exchange_n(&g_ps3_tr.lock, 1, __ATOMIC_ACQUIRE); }
static inline void ps3_tr_unlock(void) { __atomic_store_n(&g_ps3_tr.lock, 0, __ATOMIC_RELEASE); }

/* ---- writing out (the consumer side; lock held) ------------------------- */

static inline void ps3_tr_ts(char* out, size_t n, uint64_t ns)
{
    snprintf(out, n, "%llu.%03u", (unsigned long long)(ns / 1000u), (unsigned)(ns % 1000u));
}

static inline void ps3_tr_json_str(FILE* f, const char* s)
{
    fputc('"', f);
    for (; *s; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { fputc('\\', f); fputc(c, f); }
        else if (c < 0x20) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

static inline void ps3_tr_sep(FILE* f)
{
    if (g_ps3_tr.first) g_ps3_tr.first = 0;
    else fputs(",\n", f);
}

static inline void ps3_tr_args(FILE* f, const ps3_tr_desc* d, uint32_t a, uint32_t b)
{
    if (!d->a_name && !d->b_name) return;
    fputs(",\"args\":{", f);
    if (d->a_name) {
        fprintf(f, (d->hex & 1) ? "\"%s\":\"0x%X\"" : "\"%s\":%u", d->a_name, a);
        if (d->b_name) fputc(',', f);
    }
    if (d->b_name) fprintf(f, (d->hex & 2) ? "\"%s\":\"0x%X\"" : "\"%s\":%u", d->b_name, b);
    fputc('}', f);
}

static inline void ps3_tr_write_rec(FILE* f, uint32_t tid, const ps3_tr_rec* r)
{
    const ps3_tr_desc* d = ps3_tr_desc_of(r->ev);
    const uint64_t base = g_ps3_tr.base;
    const uint64_t t0 = r->t0 > base ? r->t0 - base : 0;
    const uint64_t t1 = r->t1 > base ? r->t1 - base : 0;
    char ts[32], dur[32];
    ps3_tr_ts(ts, sizeof ts, t0);
    ps3_tr_sep(f);
    if (d->ph == PS3_TR_PH_SPAN) {
        ps3_tr_ts(dur, sizeof dur, t1 > t0 ? t1 - t0 : 0);
        fprintf(f, "{\"name\":\"%s\",\"cat\":\"%s\",\"ph\":\"X\",\"pid\":1,\"tid\":%u,\"ts\":%s,\"dur\":%s",
                d->name, d->cat, tid, ts, dur);
        ps3_tr_args(f, d, r->a, r->b);
        fputc('}', f);
    } else if (d->ph == PS3_TR_PH_INSTANT) {
        fprintf(f, "{\"name\":\"%s\",\"cat\":\"%s\",\"ph\":\"i\",\"s\":\"t\",\"pid\":1,\"tid\":%u,\"ts\":%s",
                d->name, d->cat, tid, ts);
        ps3_tr_args(f, d, r->a, r->b);
        fputc('}', f);
    } else {
        /* An async pair: a wait that outlives the pass it began in, so it
         * cannot nest in the thread's spans. Its own track, keyed by the id. */
        ps3_tr_ts(dur, sizeof dur, t1 > t0 ? t1 : t0);
        fprintf(f, "{\"name\":\"%s\",\"cat\":\"%s\",\"ph\":\"b\",\"id\":\"0x%X\",\"pid\":1,\"tid\":%u,\"ts\":%s",
                d->name, d->cat, r->a, tid, ts);
        ps3_tr_args(f, d, r->a, r->b);
        fputs("},\n", f);
        fprintf(f, "{\"name\":\"%s\",\"cat\":\"%s\",\"ph\":\"e\",\"id\":\"0x%X\",\"pid\":1,\"tid\":%u,\"ts\":%s}",
                d->name, d->cat, r->a, tid, dur);
    }
}

static inline void ps3_tr_write_name(FILE* f, ps3_tr_ring* r)
{
    const uint32_t g = __atomic_load_n(&r->name_gen, __ATOMIC_ACQUIRE);
    if (g == r->name_out) return;
    char name[sizeof r->name];
    memcpy(name, r->name, sizeof name);
    name[sizeof name - 1] = 0;
    if (__atomic_load_n(&r->name_gen, __ATOMIC_ACQUIRE) != g) return;   /* renamed meanwhile: next time */
    r->name_out = g;
    ps3_tr_sep(f);
    fprintf(f, "{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,\"tid\":%u,\"args\":{\"name\":", r->tid);
    ps3_tr_json_str(f, name);
    fputs("}}", f);
}

/* Write every pending record of every ring. */
static inline void ps3_tr_drain(void)
{
    ps3_tr_state* s = &g_ps3_tr;
    FILE* f = s->f;
    if (!f) return;
    const uint32_t n = __atomic_load_n(&s->nrings, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < n && i < PS3_TR_MAX_RINGS; i++) {
        ps3_tr_ring* r = __atomic_load_n(&s->rings[i], __ATOMIC_ACQUIRE);
        if (!r) continue;   /* slot claimed, ring not published yet */
        ps3_tr_write_name(f, r);
        const uint64_t h = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
        uint64_t t = r->tail;
        for (; t != h; t++) ps3_tr_write_rec(f, r->tid, &r->buf[t & r->mask]);
        s->written += h - r->tail;
        __atomic_store_n(&r->tail, h, __ATOMIC_RELEASE);
    }
    fflush(f);
}

/* Time to write: PS3RECOMP_TRACE_FLUSH_MB pending in all, or a ring half full. */
static inline int ps3_tr_flush_due(void)
{
    ps3_tr_state* s = &g_ps3_tr;
    const uint32_t n = __atomic_load_n(&s->nrings, __ATOMIC_ACQUIRE);
    uint64_t total = 0;
    for (uint32_t i = 0; i < n && i < PS3_TR_MAX_RINGS; i++) {
        ps3_tr_ring* r = __atomic_load_n(&s->rings[i], __ATOMIC_ACQUIRE);
        if (!r) continue;
        const uint64_t p = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE) - r->tail;
        if (p > s->ring_recs / 2) return 1;
        total += p;
    }
    return total >= s->flush_recs;
}

#ifdef _WIN32
static inline DWORD WINAPI ps3_tr_flusher(LPVOID unused)
#else
static inline void* ps3_tr_flusher(void* unused)
#endif
{
    (void)unused;
    for (;;) {
        ps3_tr_sleep_ms(PS3_TR_POLL_MS);
        if (__atomic_load_n(&g_ps3_tr.on, __ATOMIC_ACQUIRE) != 2) break;
        if (!ps3_tr_trylock()) continue;
        if (__atomic_load_n(&g_ps3_tr.on, __ATOMIC_ACQUIRE) == 2 && ps3_tr_flush_due())
            ps3_tr_drain();
        ps3_tr_unlock();
    }
    return 0;
}

/* Write what is left and close the file. Runs at exit; callable earlier (a
 * crash handler, a test). Recording stops for good. */
static inline void ps3_trace_shutdown(void)
{
    ps3_tr_state* s = &g_ps3_tr;
    int expect = 2;
    if (!__atomic_compare_exchange_n(&s->on, &expect, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;
    ps3_tr_lock();
    ps3_tr_drain();
    uint64_t dropped = 0;
    const uint32_t n = __atomic_load_n(&s->nrings, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < n && i < PS3_TR_MAX_RINGS; i++) {
        ps3_tr_ring* r = __atomic_load_n(&s->rings[i], __ATOMIC_ACQUIRE);
        if (r) dropped += __atomic_load_n(&r->dropped, __ATOMIC_RELAXED);
    }
    FILE* f = s->f;
    if (f) {
        /* Last, so the array ends on an element rather than a comma. */
        ps3_tr_sep(f);
        fprintf(f, "{\"name\":\"process_name\",\"ph\":\"M\",\"pid\":1,\"args\":{\"name\":\"ps3recomp\","
                   "\"events\":%llu,\"dropped\":%llu}}\n]\n",
                (unsigned long long)s->written, (unsigned long long)dropped);
        fclose(f);
        s->f = NULL;
        fprintf(stderr, "[trace] wrote %llu events (%llu dropped) from %u threads to %s\n",
                (unsigned long long)s->written, (unsigned long long)dropped, n, s->path);
    }
    ps3_tr_unlock();
}

static inline void ps3_tr_atexit(void) { ps3_trace_shutdown(); }

static inline double ps3_tr_env_num(const char* name, double dflt)
{
    const char* e = getenv(name);
    if (!e || !*e) return dflt;
    const double v = strtod(e, NULL);
    return v > 0 ? v : dflt;
}

/* First hook: read PS3RECOMP_TRACE once. Hooks that run while another thread
 * is still in here see "off" and lose only those few events. */
static inline int ps3_tr_init(void)
{
    ps3_tr_state* s = &g_ps3_tr;
    int expect = 0;
    if (!__atomic_compare_exchange_n(&s->on, &expect, 3, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return expect == 2;
    int on = 1;
    const char* path = getenv("PS3RECOMP_TRACE");
    if (path && *path) {
        snprintf(s->path, sizeof s->path, "%s", path);
        const double ring_mb = ps3_tr_env_num("PS3RECOMP_TRACE_RING_MB", 2.0);
        uint64_t want = (uint64_t)(ring_mb * 1048576.0) / sizeof(ps3_tr_rec);
        uint64_t recs = 64;
        while (recs < want && recs < (1ull << 30)) recs <<= 1;
        s->ring_recs = recs;
        s->flush_recs = (uint64_t)(ps3_tr_env_num("PS3RECOMP_TRACE_FLUSH_MB", 4.0) * 1048576.0) / sizeof(ps3_tr_rec);
        if (!s->flush_recs) s->flush_recs = 1;
        s->min_ns = (uint64_t)(ps3_tr_env_num("PS3RECOMP_TRACE_MIN_US", 0.0) * 1000.0);
        s->f = fopen(s->path, "wb");
#ifdef _WIN32
        s->tls = TlsAlloc();
        const int key_ok = s->tls != TLS_OUT_OF_INDEXES;
#else
        const int key_ok = pthread_key_create(&s->tls, NULL) == 0;
#endif
        if (!s->f || !key_ok) {
            fprintf(stderr, "[trace] cannot record to %s\n", s->path);
            if (s->f) { fclose(s->f); s->f = NULL; }
        } else {
            setvbuf(s->f, NULL, _IOFBF, 1u << 20);
            fputs("[\n", s->f);
            s->first = 1;
            s->base = ps3_trace_now();
            atexit(ps3_tr_atexit);
            on = 2;
            __atomic_store_n(&s->on, on, __ATOMIC_RELEASE);   /* the flusher checks it */
#ifdef _WIN32
            HANDLE th = CreateThread(NULL, 0, ps3_tr_flusher, NULL, 0, NULL);
            const int started = th != NULL;
            if (th) CloseHandle(th);
#else
            pthread_t th;
            const int started = pthread_create(&th, NULL, ps3_tr_flusher, NULL) == 0;
            if (started) pthread_detach(th);
#endif
            fprintf(stderr, "[trace] recording to %s (%llu KB per thread, flush every %.2f MB)%s\n",
                    s->path, (unsigned long long)(recs * sizeof(ps3_tr_rec) / 1024u),
                    (double)(s->flush_recs * sizeof(ps3_tr_rec)) / 1048576.0,
                    started ? "" : " -- no flusher thread, written at exit only");
        }
    }
    __atomic_store_n(&s->on, on, __ATOMIC_RELEASE);
    return on == 2;
}

/* The hook guard. Off, it is one load and one branch taken the same way every
 * time. */
static inline int ps3_trace_on(void)
{
    const int on = __atomic_load_n(&g_ps3_tr.on, __ATOMIC_RELAXED);
    if (PS3_TR_LIKELY(on == 1)) return 0;
    if (on == 2) return 1;
    return on == 0 ? ps3_tr_init() : 0;
}

/* ---- the producer side --------------------------------------------------- */

static inline ps3_tr_ring* ps3_tr_ring_new(void)
{
    ps3_tr_state* s = &g_ps3_tr;
    if (__atomic_load_n(&s->nrings, __ATOMIC_RELAXED) >= PS3_TR_MAX_RINGS) return NULL;
    ps3_tr_ring* r = (ps3_tr_ring*)calloc(1, sizeof *r);
    if (!r) return NULL;
    const uint32_t i = __atomic_fetch_add(&s->nrings, 1, __ATOMIC_ACQ_REL);
    if (i >= PS3_TR_MAX_RINGS) { free(r); return NULL; }
    r->tid = __atomic_add_fetch(&s->next_tid, 1, __ATOMIC_RELAXED);
    r->mask = s->ring_recs - 1;
    snprintf(r->name, sizeof r->name, "host thread %u", r->tid);
    r->name_gen = 1;
    __atomic_store_n(&s->rings[i], r, __ATOMIC_RELEASE);
    return r;   /* never freed: the flusher may hold it at any time */
}

/* The calling thread's ring. One lookup per thread per translation unit; the
 * TLS key makes it the same ring in all of them. */
static inline ps3_tr_ring* ps3_tr_ring_self(void)
{
    static PS3_TR_TLS ps3_tr_ring* t_ring;
    ps3_tr_ring* r = t_ring;
    if (PS3_TR_LIKELY(r != NULL)) return r;
#ifdef _WIN32
    r = (ps3_tr_ring*)TlsGetValue(g_ps3_tr.tls);
    if (!r && (r = ps3_tr_ring_new()) != NULL) TlsSetValue(g_ps3_tr.tls, r);
#else
    r = (ps3_tr_ring*)pthread_getspecific(g_ps3_tr.tls);
    if (!r && (r = ps3_tr_ring_new()) != NULL) pthread_setspecific(g_ps3_tr.tls, r);
#endif
    t_ring = r;
    return r;
}

static inline void ps3_tr_put(ps3_tr_ring* r, uint32_t ev, uint64_t t0, uint64_t t1,
                              uint32_t a, uint32_t b)
{
    if (PS3_TR_UNLIKELY(!r->buf)) {
        if (r->alloc_failed) { __atomic_fetch_add(&r->dropped, 1, __ATOMIC_RELAXED); return; }
        r->buf = (ps3_tr_rec*)malloc((size_t)(r->mask + 1) * sizeof(ps3_tr_rec));
        if (!r->buf) { r->alloc_failed = 1; __atomic_fetch_add(&r->dropped, 1, __ATOMIC_RELAXED); return; }
    }
    const uint64_t h = r->head;
    if (h - __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE) > r->mask) {
        __atomic_fetch_add(&r->dropped, 1, __ATOMIC_RELAXED);   /* full: the flusher is behind */
        return;
    }
    ps3_tr_rec* x = &r->buf[h & r->mask];
    x->t0 = t0; x->t1 = t1; x->a = a; x->b = b; x->ev = ev; x->pad = 0;
    __atomic_store_n(&r->head, h + 1, __ATOMIC_RELEASE);
}

/* A span with both ends known. */
static inline void ps3_trace_span(int ev, uint64_t t0, uint64_t t1, uint32_t a, uint32_t b)
{
    if (__atomic_load_n(&g_ps3_tr.on, __ATOMIC_RELAXED) != 2) return;
    if (t1 < t0 || t1 - t0 < g_ps3_tr.min_ns) return;
    ps3_tr_ring* r = ps3_tr_ring_self();
    if (r) ps3_tr_put(r, (uint32_t)ev, t0, t1, a, b);
}

/* t0 for ps3_trace_end, 0 when not recording. */
static inline uint64_t ps3_trace_begin(void)
{
    return ps3_trace_on() ? ps3_trace_now() : 0;
}

static inline void ps3_trace_end(uint64_t t0, int ev, uint32_t a, uint32_t b)
{
    if (PS3_TR_LIKELY(!t0)) return;
    ps3_trace_span(ev, t0, ps3_trace_now(), a, b);
}

static inline void ps3_trace_instant(int ev, uint32_t a, uint32_t b)
{
    if (PS3_TR_LIKELY(!ps3_trace_on())) return;
    const uint64_t t = ps3_trace_now();
    ps3_tr_ring* r = ps3_tr_ring_self();
    if (r) ps3_tr_put(r, (uint32_t)ev, t, t, a, b);
}

/* A wait the runtime only sees as a guest spinning on one call (a status
 * read, a register poll). Calls of one event on one thread closer than
 * PS3_TR_POLL_GAP_NS become one span with b = calls and a = the last a; the
 * run is written when a later call starts a new one, or by ps3_trace_poll_end
 * when the caller can tell the wait is over. Runs of one call are not waits
 * and are not written. */
static inline void ps3_tr_poll_close(ps3_tr_ring* r)
{
    if (r->poll_n > 1 && r->poll_t1 - r->poll_t0 >= g_ps3_tr.min_ns)
        ps3_tr_put(r, r->poll_ev, r->poll_t0, r->poll_t1, r->poll_a, r->poll_n);
    r->poll_n = 0;
}

static inline void ps3_trace_poll(int ev, uint32_t a)
{
    if (PS3_TR_LIKELY(!ps3_trace_on())) return;
    ps3_tr_ring* r = ps3_tr_ring_self();
    if (!r) return;
    const uint64_t t = ps3_trace_now();
    if (r->poll_n && (r->poll_ev != (uint32_t)ev || t - r->poll_t1 > PS3_TR_POLL_GAP_NS))
        ps3_tr_poll_close(r);
    if (!r->poll_n) { r->poll_ev = (uint32_t)ev; r->poll_t0 = t; }
    r->poll_t1 = t;
    r->poll_a = a;
    r->poll_n++;
}

static inline void ps3_trace_poll_end(int ev)
{
    if (PS3_TR_LIKELY(!ps3_trace_on())) return;
    ps3_tr_ring* r = ps3_tr_ring_self();
    if (!r || !r->poll_n || r->poll_ev != (uint32_t)ev) return;
    r->poll_t1 = ps3_trace_now();
    r->poll_n++;          /* the call that saw the wait end */
    ps3_tr_poll_close(r);
}

/* Name the calling thread's track. */
static inline void ps3_trace_thread_name(const char* name)
{
    if (PS3_TR_LIKELY(!ps3_trace_on()) || !name) return;
    ps3_tr_ring* r = ps3_tr_ring_self();
    if (!r) return;
    snprintf(r->name, sizeof r->name, "%s", name);
    __atomic_add_fetch(&r->name_gen, 1, __ATOMIC_RELEASE);
}

/* The same, unless the thread already has a name of its own. */
static inline void ps3_trace_thread_name_default(const char* name)
{
    if (PS3_TR_LIKELY(!ps3_trace_on()) || !name) return;
    ps3_tr_ring* r = ps3_tr_ring_self();
    if (r && r->name_gen == 1) ps3_trace_thread_name(name);
}

/* ---- scoped spans ------------------------------------------------------- */

typedef struct { uint64_t t0; uint32_t a, b; int ev; } ps3_trace_scope_t;

/* A timeout or size argument as a 32-bit arg, saturated. */
static inline uint32_t ps3_trace_clamp32(uint64_t v) { return v > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)v; }

static inline void ps3_trace_scope_close(ps3_trace_scope_t* s)
{
    if (PS3_TR_LIKELY(!s->t0)) return;
    ps3_trace_span(s->ev, s->t0, ps3_trace_now(), s->a, s->b);
}

#define PS3_TR_CAT2(x, y) x##y
#define PS3_TR_CAT(x, y)  PS3_TR_CAT2(x, y)

#ifdef __cplusplus
}   /* extern "C" */

struct ps3_trace_scope_cpp {
    ps3_trace_scope_t s;
    ~ps3_trace_scope_cpp() { ps3_trace_scope_close(&s); }
};
#  define PS3_TRACE_SCOPE_IF(cond, ev, a, b) \
    ps3_trace_scope_cpp PS3_TR_CAT(ps3_tr_scope_, __LINE__) \
        { { (ps3_trace_on() && (cond)) ? ps3_trace_now() : 0, \
            (uint32_t)(a), (uint32_t)(b), (int)(ev) } }
#elif defined(__GNUC__) || defined(__clang__)
#  define PS3_TRACE_SCOPE_IF(cond, ev, a, b) \
    ps3_trace_scope_t PS3_TR_CAT(ps3_tr_scope_, __LINE__) \
        __attribute__((cleanup(ps3_trace_scope_close))) = \
        { (ps3_trace_on() && (cond)) ? ps3_trace_now() : 0, \
          (uint32_t)(a), (uint32_t)(b), (int)(ev) }
#else
#  define PS3_TRACE_SCOPE_IF(cond, ev, a, b) ((void)0)   /* cl.exe, which cannot build the runtime anyway */
#endif
#define PS3_TRACE_SCOPE(ev, a, b) PS3_TRACE_SCOPE_IF(1, ev, a, b)

#endif /* PS3EMU_PS3_TRACE_H */
