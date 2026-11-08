/*
 * rsx_fifo_busy -- where the RSX FIFO consumer thread's time goes.
 *
 * PS3RECOMP_RSX_FIFO_BUSY=1 (off by default; then every hook is a load and a
 * predictable branch) charges the consumer thread's wall time to exclusive,
 * nested categories:
 *   - idle waits, split by why the pass before stopped: it reached put, it
 *     is held on a semaphore acquire, it is held on a GET_REPORT, or anything
 *     else (a park, an early return);
 *   - the FIFO walk itself, draws handed to the draw engine, engine flushes,
 *     engine presents, a host's own present calls, the display-image wait
 *     (Metal's in-flight bound and nextDrawable, Vulkan's swapchain acquire
 *     and present) and the window's event pump.
 * Time is charged to the innermost open span only, so the categories add up
 * to the thread's wall time; time outside every span is "other".
 *
 * The consumer thread is the first thread that enters an idle wait
 * (rsx_fifo_busy_idle_begin, from cellGcm_fifo_kick_wait); spans opened on
 * any other thread are ignored. An update is one FIFO flip. About every two
 * seconds (PS3RECOMP_RSX_FIFO_BUSY_PERIOD_MS) one "[rsx fifo busy]" line on
 * stderr reports per-update wall and busy time (wall minus the idle waits),
 * the busy distribution (p50, p95, max, updates over 16.7 ms) and each
 * category's exclusive milliseconds and span count per update.
 *
 * A port or extension can name up to RSX_FIFO_BUSY_EXTENSION_SLOTS
 * categories of its own (rsx_fifo_busy_category) for work it does on the
 * consumer thread. Names are printed as given.
 *
 * Header-only, like ps3_ftrace.h and for the same reason: the hooked files
 * are also compiled on their own by self-contained tests, and a new link
 * dependency would break each of them. The state is one weak/selectany
 * object the linker folds into a single copy.
 */
#ifndef PS3RECOMP_RSX_FIFO_BUSY_H
#define PS3RECOMP_RSX_FIFO_BUSY_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  include <windows.h>
#else
#  include <pthread.h>
#  include <time.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum rsx_fifo_busy_builtin {
    RSX_FIFO_BUSY_OTHER = 0,     /* consumer time outside every span below */
    RSX_FIFO_BUSY_IDLE_PUT,      /* idle wait after a pass that reached put */
    RSX_FIFO_BUSY_IDLE_SEMA,     /* idle wait after a pass held on a semaphore acquire */
    RSX_FIFO_BUSY_IDLE_REPORT,   /* idle wait after a pass held on a GET_REPORT */
    RSX_FIFO_BUSY_IDLE_OTHER,    /* idle wait after any other stop (park, early return) */
    RSX_FIFO_BUSY_DRAIN,         /* FIFO walk and methods not in a nested span */
    RSX_FIFO_BUSY_DRAW,          /* draws handed to the draw engine */
    RSX_FIFO_BUSY_FLUSH,         /* engine encode/commit (and any wait) that is not a present */
    RSX_FIFO_BUSY_PRESENT,       /* presenting a guest frame, except the display-image wait */
    RSX_FIFO_BUSY_DRAWABLE,      /* in-flight bound and display-image acquire/present */
    RSX_FIFO_BUSY_PUMP,          /* window and event pump */
    RSX_FIFO_BUSY_PRESENT_HOST,  /* a host-driven present call, outside the present it makes */
    RSX_FIFO_BUSY_BUILTIN_COUNT
};
#define RSX_FIFO_BUSY_EXTENSION_SLOTS 8
#define RSX_FIFO_BUSY_CATEGORIES (RSX_FIFO_BUSY_BUILTIN_COUNT + RSX_FIFO_BUSY_EXTENSION_SLOTS)

/* Why the last FIFO pass stopped; selects the idle category of the next wait. */
enum rsx_fifo_busy_stop {
    RSX_FIFO_BUSY_STOP_PUT = 0,
    RSX_FIFO_BUSY_STOP_SEMA,
    RSX_FIFO_BUSY_STOP_REPORT,
    RSX_FIFO_BUSY_STOP_OTHER
};

#if defined(_MSC_VER) || defined(__MINGW32__)
#  define RSX_FIFO_BUSY_SHARED __declspec(selectany)
#else
#  define RSX_FIFO_BUSY_SHARED __attribute__((weak))
#endif

#define RSX_FIFO_BUSY_DEPTH   16
#define RSX_FIFO_BUSY_SAMPLES 1024

typedef struct rsx_fifo_busy_state {
    int enabled;                 /* 0 off, 1 on; valid once `read` is set */
    int read;                    /* the environment has been read */
    int names_lock, extension_count;
    char extension_names[RSX_FIFO_BUSY_EXTENSION_SLOTS][24];
    int owner_claimed;           /* 0 none, 2 being claimed, 1 `owner` is the consumer thread */
#ifdef _WIN32
    DWORD owner;
#else
    pthread_t owner;
#endif
    uint64_t (*clock)(void);
    void (*sink)(const char* line);
    uint64_t period_ns;
    /* Consumer-thread state: only the owner thread touches it. */
    int stack[RSX_FIFO_BUSY_DEPTH];
    int depth, ignored;          /* ignored: opens past the depth, closed silently */
    uint64_t last_ns, period_start_ns, flip_ns, idle_at_flip_ns;
    uint64_t acc_ns[RSX_FIFO_BUSY_CATEGORIES], spans[RSX_FIFO_BUSY_CATEGORIES];
    uint64_t updates, unbalanced, overflow;
    uint32_t samples_us[RSX_FIFO_BUSY_SAMPLES];
    unsigned samples;
    int stop;
} rsx_fifo_busy_state;

#ifdef __cplusplus
RSX_FIFO_BUSY_SHARED rsx_fifo_busy_state g_rsx_fifo_busy = {};
#else
RSX_FIFO_BUSY_SHARED rsx_fifo_busy_state g_rsx_fifo_busy = { 0 };
#endif

static inline uint64_t rsx_fifo_busy_default_clock(void)
{
#ifdef _WIN32
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f);
    const uint64_t ticks = (uint64_t)c.QuadPart, hz = (uint64_t)f.QuadPart;
    return (ticks / hz) * 1000000000ull + (ticks % hz) * 1000000000ull / hz;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static inline void rsx_fifo_busy_default_sink(const char* line) { fprintf(stderr, "%s\n", line); }

static inline void rsx_fifo_busy_reset_locked(rsx_fifo_busy_state* s)
{
    s->depth = s->ignored = 0;
    s->last_ns = s->period_start_ns = s->flip_ns = s->idle_at_flip_ns = 0;
    memset(s->acc_ns, 0, sizeof s->acc_ns);
    memset(s->spans, 0, sizeof s->spans);
    s->updates = s->unbalanced = s->overflow = 0;
    s->samples = 0;
    s->stop = RSX_FIFO_BUSY_STOP_OTHER;
}

/* Read PS3RECOMP_RSX_FIFO_BUSY once. Racing first callers read the same
 * environment and store the same answer. */
static inline int rsx_fifo_busy_enabled(void)
{
    rsx_fifo_busy_state* s = &g_rsx_fifo_busy;
    if (__atomic_load_n(&s->read, __ATOMIC_ACQUIRE)) return __atomic_load_n(&s->enabled, __ATOMIC_RELAXED);
    const char* e = getenv("PS3RECOMP_RSX_FIFO_BUSY");
    const int on = e && !strcmp(e, "1");
    if (on && !s->clock) {
        const char* p = getenv("PS3RECOMP_RSX_FIFO_BUSY_PERIOD_MS");
        const long ms = p ? strtol(p, NULL, 10) : 0;
        s->period_ns = (uint64_t)(ms > 0 ? ms : 2000) * 1000000ull;
        s->clock = rsx_fifo_busy_default_clock;
        s->sink = rsx_fifo_busy_default_sink;
        rsx_fifo_busy_reset_locked(s);
    }
    __atomic_store_n(&s->enabled, on, __ATOMIC_RELAXED);
    __atomic_store_n(&s->read, 1, __ATOMIC_RELEASE);
    return on;
}

/* Tests only: replace the clock and the line sink and force enablement; all
 * state is reset and the consumer thread forgotten. NULL restores defaults. */
static inline void rsx_fifo_busy_test_configure(int enabled, uint64_t (*clock_ns)(void),
                                                void (*sink)(const char* line), uint64_t period_ns)
{
    rsx_fifo_busy_state* s = &g_rsx_fifo_busy;
    s->clock = clock_ns ? clock_ns : rsx_fifo_busy_default_clock;
    s->sink = sink ? sink : rsx_fifo_busy_default_sink;
    s->period_ns = period_ns ? period_ns : 2000000000ull;
    rsx_fifo_busy_reset_locked(s);
    __atomic_store_n(&s->owner_claimed, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s->enabled, enabled ? 1 : 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s->read, 1, __ATOMIC_RELEASE);
}

/* A category id for an extension name (at most 23 characters), the same id
 * for the same name; -1 when disabled, the name is empty or every slot is
 * taken. Thread-safe. */
static inline int rsx_fifo_busy_category(const char* name)
{
    rsx_fifo_busy_state* s = &g_rsx_fifo_busy;
    if (!rsx_fifo_busy_enabled() || !name || !*name) return -1;
    while (__atomic_exchange_n(&s->names_lock, 1, __ATOMIC_ACQUIRE)) {}
    int id = -1;
    const int n = __atomic_load_n(&s->extension_count, __ATOMIC_RELAXED);
    for (int i = 0; i < n; ++i)
        if (!strncmp(s->extension_names[i], name, sizeof s->extension_names[i] - 1)) id = i;
    if (id < 0 && n < RSX_FIFO_BUSY_EXTENSION_SLOTS) {
        snprintf(s->extension_names[n], sizeof s->extension_names[n], "%s", name);
        id = n;
        __atomic_store_n(&s->extension_count, n + 1, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&s->names_lock, 0, __ATOMIC_RELEASE);
    return id < 0 ? -1 : RSX_FIFO_BUSY_BUILTIN_COUNT + id;
}

/* On and on the consumer thread. */
static inline int rsx_fifo_busy_owner(void)
{
    rsx_fifo_busy_state* s = &g_rsx_fifo_busy;
    if (!rsx_fifo_busy_enabled() || __atomic_load_n(&s->owner_claimed, __ATOMIC_ACQUIRE) != 1) return 0;
#ifdef _WIN32
    return s->owner == GetCurrentThreadId();
#else
    return pthread_equal(s->owner, pthread_self());
#endif
}

/* Charge the time since the last event to the innermost open span. */
static inline uint64_t rsx_fifo_busy_charge(rsx_fifo_busy_state* s)
{
    const uint64_t now = s->clock();
    const int top = s->depth ? s->stack[s->depth - 1] : RSX_FIFO_BUSY_OTHER;
    if (s->last_ns && now > s->last_ns) s->acc_ns[top] += now - s->last_ns;
    s->last_ns = now;
    return now;
}

static inline int rsx_fifo_busy_is_idle(int c)
{
    return c >= RSX_FIFO_BUSY_IDLE_PUT && c <= RSX_FIFO_BUSY_IDLE_OTHER;
}

static inline uint64_t rsx_fifo_busy_idle_total(const rsx_fifo_busy_state* s)
{
    uint64_t t = 0;
    for (int c = RSX_FIFO_BUSY_IDLE_PUT; c <= RSX_FIFO_BUSY_IDLE_OTHER; ++c) t += s->acc_ns[c];
    return t;
}

static inline void rsx_fifo_busy_push(rsx_fifo_busy_state* s, int category)
{
    if (s->depth >= RSX_FIFO_BUSY_DEPTH) { ++s->ignored; ++s->overflow; return; }
    rsx_fifo_busy_charge(s);
    s->stack[s->depth++] = category;
    ++s->spans[category];
}

static inline void rsx_fifo_busy_pop(rsx_fifo_busy_state* s, int category)
{
    if (s->ignored) { --s->ignored; return; }
    int at = s->depth - 1;
    while (at >= 0 && s->stack[at] != category) --at;
    if (at < 0) return;                      /* not open: ignore */
    rsx_fifo_busy_charge(s);
    if (at != s->depth - 1) ++s->unbalanced;
    s->depth = at;
}

/* Open/close a span of a category on the consumer thread. Closing a category
 * that is not innermost closes the spans inside it too and is counted as
 * unbalanced; closing one that is not open is ignored. */
static inline void rsx_fifo_busy_enter(int category)
{
    if (!rsx_fifo_busy_owner() || category < 0 || category >= RSX_FIFO_BUSY_CATEGORIES) return;
    rsx_fifo_busy_push(&g_rsx_fifo_busy, category);
}

static inline void rsx_fifo_busy_leave(int category)
{
    if (!rsx_fifo_busy_owner() || category < 0 || category >= RSX_FIFO_BUSY_CATEGORIES) return;
    rsx_fifo_busy_pop(&g_rsx_fifo_busy, category);
}

static inline void rsx_fifo_busy_pass_stopped(int stop)
{
    if (!rsx_fifo_busy_owner()) return;
    g_rsx_fifo_busy.stop = (stop >= RSX_FIFO_BUSY_STOP_PUT && stop <= RSX_FIFO_BUSY_STOP_OTHER)
                         ? stop : RSX_FIFO_BUSY_STOP_OTHER;
}

/* One update boundary (a flip reached in the FIFO), on the consumer thread. */
static inline void rsx_fifo_busy_flip(void)
{
    if (!rsx_fifo_busy_owner()) return;
    rsx_fifo_busy_state* s = &g_rsx_fifo_busy;
    const uint64_t now = rsx_fifo_busy_charge(s);
    const uint64_t idle = rsx_fifo_busy_idle_total(s);
    if (s->flip_ns) {
        const uint64_t wall = now - s->flip_ns, waited = idle - s->idle_at_flip_ns;
        const uint64_t busy = wall > waited ? wall - waited : 0;
        if (s->samples < RSX_FIFO_BUSY_SAMPLES) s->samples_us[s->samples++] = (uint32_t)(busy / 1000u);
    }
    s->flip_ns = now;
    s->idle_at_flip_ns = idle;
    ++s->updates;
}

static inline int rsx_fifo_busy_compare_u32(const void* a, const void* b)
{
    const uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return x < y ? -1 : x > y;
}

static inline void rsx_fifo_busy_report(rsx_fifo_busy_state* s, uint64_t now)
{
    static const char* const names[RSX_FIFO_BUSY_BUILTIN_COUNT] = {
        "other", "idle_put", "idle_sema", "idle_report", "idle_other", "drain", "draw",
        "flush", "present", "drawable", "pump", "present_host" };
    const uint64_t wall = now - s->period_start_ns;
    const uint64_t idle = rsx_fifo_busy_idle_total(s), busy = wall > idle ? wall - idle : 0;
    const int per_update = s->updates != 0;
    const double div = per_update ? (double)s->updates : (double)wall / 1e9;
    uint32_t sorted[RSX_FIFO_BUSY_SAMPLES];
    const unsigned n = s->samples;
    memcpy(sorted, s->samples_us, n * sizeof sorted[0]);
    qsort(sorted, n, sizeof sorted[0], rsx_fifo_busy_compare_u32);
    const unsigned i95 = (n * 95u) / 100u < n ? (n * 95u) / 100u : (n ? n - 1 : 0);
    const double p50 = n ? sorted[n / 2] / 1e3 : 0, p95 = n ? sorted[i95] / 1e3 : 0,
                 max = n ? sorted[n - 1] / 1e3 : 0;
    unsigned over = 0;
    for (unsigned i = 0; i < n; ++i) over += sorted[i] > 16667u;
    char line[2048];
    int len = snprintf(line, sizeof line,
                       "[rsx fifo busy] seconds=%.2f updates=%llu per=%s wall=%.2f busy=%.2f busy_pct=%.1f "
                       "busy_p50=%.2f busy_p95=%.2f busy_max=%.2f busy_over_16.7=%u/%u",
                       wall / 1e9, (unsigned long long)s->updates, per_update ? "update" : "second",
                       wall / 1e6 / div, busy / 1e6 / div, wall ? 100.0 * (double)busy / (double)wall : 0.0,
                       p50, p95, max, over, n);
    const int extensions = __atomic_load_n(&s->extension_count, __ATOMIC_ACQUIRE);
    for (int c = 0; c < RSX_FIFO_BUSY_BUILTIN_COUNT + extensions && len > 0 && (size_t)len < sizeof line; ++c) {
        if (c > RSX_FIFO_BUSY_OTHER && !rsx_fifo_busy_is_idle(c) && !s->acc_ns[c] && !s->spans[c]) continue;
        const char* name = c < RSX_FIFO_BUSY_BUILTIN_COUNT ? names[c]
                                                         : s->extension_names[c - RSX_FIFO_BUSY_BUILTIN_COUNT];
        len += snprintf(line + len, sizeof line - (size_t)len, " %s=%.2f/%.1f", name,
                        s->acc_ns[c] / 1e6 / div, (double)s->spans[c] / div);
    }
    if (len > 0 && (size_t)len < sizeof line)
        snprintf(line + len, sizeof line - (size_t)len, " unbalanced=%llu overflow=%llu",
                 (unsigned long long)s->unbalanced, (unsigned long long)s->overflow);
    s->sink(line);
    /* Carry the idle time since the last flip into the next period, so the
     * update that straddles the report keeps its own busy figure (modular). */
    s->idle_at_flip_ns = 0 - (idle - s->idle_at_flip_ns);
    memset(s->acc_ns, 0, sizeof s->acc_ns);
    memset(s->spans, 0, sizeof s->spans);
    s->updates = 0; s->samples = 0; s->unbalanced = 0; s->overflow = 0;
    s->period_start_ns = now;
}

/* The idle wait. The first thread to enter one becomes the consumer thread.
 * Leaving it may print the periodic line. */
static inline void rsx_fifo_busy_idle_begin(void)
{
    rsx_fifo_busy_state* s = &g_rsx_fifo_busy;
    if (!rsx_fifo_busy_enabled()) return;
    if (!rsx_fifo_busy_owner()) {
        int expected = 0;
        if (!__atomic_compare_exchange_n(&s->owner_claimed, &expected, 2, 0,
                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
#ifdef _WIN32
        s->owner = GetCurrentThreadId();
#else
        s->owner = pthread_self();
#endif
        s->last_ns = s->period_start_ns = s->clock();
        __atomic_store_n(&s->owner_claimed, 1, __ATOMIC_RELEASE);
    }
    static const int idle_of_stop[] = { RSX_FIFO_BUSY_IDLE_PUT, RSX_FIFO_BUSY_IDLE_SEMA,
                                        RSX_FIFO_BUSY_IDLE_REPORT, RSX_FIFO_BUSY_IDLE_OTHER };
    rsx_fifo_busy_push(s, idle_of_stop[s->stop]);
}

static inline void rsx_fifo_busy_idle_end(void)
{
    if (!rsx_fifo_busy_owner()) return;
    rsx_fifo_busy_state* s = &g_rsx_fifo_busy;
    const int top = s->depth ? s->stack[s->depth - 1] : -1;
    if (top >= 0 && rsx_fifo_busy_is_idle(top)) rsx_fifo_busy_pop(s, top);
    else rsx_fifo_busy_charge(s);
    if (s->depth == 0 && !s->ignored && s->last_ns - s->period_start_ns >= s->period_ns)
        rsx_fifo_busy_report(s, s->last_ns);
}

#ifdef __cplusplus
}
#endif
#endif
