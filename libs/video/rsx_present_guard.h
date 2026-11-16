/* Keep a window the player cannot see from gating the frame loop.
 *
 * A swapchain in FIFO mode hands back images only as the compositor takes
 * frames. With the window minimized or hidden, or the display asleep, the
 * compositor may take one only every few hundred milliseconds or never (a
 * Windows box with its display off quantized frames to about 264 ms), and an
 * unbounded acquire on the presenting thread -- the walker or the frame
 * clock -- then paces the whole title: the guest's frame loop, and anything it
 * does once per frame, such as pumping the network.
 *
 * The guard decides, per present, whether to show the frame in the window:
 *
 *  - Hidden (minimized, hidden or display off, as the window system reports
 *    it): skip the window. The frame is still rendered offscreen.
 *  - Otherwise acquire with a bound (PS3RECOMP_VK_ACQUIRE_BOUND_MS, default
 *    100 ms, far above the two refreshes a healthy FIFO swapchain can make a
 *    presenter wait). An acquire that times out marks the swapchain stalled:
 *    later acquires do not wait at all (timeout 0) until one gets an image.
 *  - A wait on the window's presentation (the present call, the blit that
 *    waits for the image's release, the reuse of a slot that blitted into the
 *    window) longer than the bound marks presentation stalled: the window is
 *    skipped, with one probe every 2 s, until a probe's frame gets through
 *    within the bound.
 *
 * A frame left out of the window is held to 60 Hz (rsx_present_guard_pace_ns):
 * the FIFO swapchain paced the title at the display's rate, and a title whose
 * flips no longer wait for anything would spin at hundreds of frames a second.
 *
 * PS3RECOMP_VK_ACQUIRE_BOUND_MS=0 (Metal: PS3RECOMP_METAL_DRAWABLE_BOUND_MS=0)
 * restores the unbounded acquire and turns the guard off. Pure logic, so it is
 * tested without a GPU or window (tools/test_present_guard.py). Fields are touched by the presenting threads
 * and, for `hidden`, the window's message pump; all through atomics. */
#ifndef RSX_PRESENT_GUARD_H
#define RSX_PRESENT_GUARD_H
#include <stdint.h>
#include <stdlib.h>

#define RSX_PRESENT_GUARD_PROBE_NS 2000000000ull
#define RSX_PRESENT_GUARD_PACE_NS 16666667ull     /* 60 Hz, what FIFO paced at */

typedef struct {
    uint64_t bound_ns;          /* 0: off, acquire unbounded */
    int hidden;                 /* the window system says nobody can see it */
    int acquire_stalled;        /* an acquire timed out; do not wait again */
    int present_stalled;        /* a present overran the bound */
    uint64_t probe_at_ns;       /* while present_stalled: the next probe */
    unsigned long long skipped; /* frames not shown in the window */
    unsigned long long stalls;  /* times a stall began */
    uint64_t pace_next_ns;      /* skipped frames: the next 60 Hz slot */
} rsx_present_guard;

#define RSX_PG_LOAD(p) __atomic_load_n((p), __ATOMIC_RELAXED)
#define RSX_PG_STORE(p, v) __atomic_store_n((p), (v), __ATOMIC_RELAXED)
#define RSX_PG_INC(p) __atomic_fetch_add((p), 1, __ATOMIC_RELAXED)

/* bound_text: PS3RECOMP_VK_ACQUIRE_BOUND_MS, or NULL for the default. */
static inline void rsx_present_guard_init(rsx_present_guard* g, const char* bound_text)
{
    long ms = 100;
    if (bound_text && *bound_text) {
        char* end = NULL;
        const long v = strtol(bound_text, &end, 10);
        if (end && !*end && v >= 0 && v <= 10000) ms = v;
    }
    g->bound_ns = (uint64_t)ms * 1000000ull;
    g->hidden = g->acquire_stalled = g->present_stalled = 0;
    g->probe_at_ns = 0;
    g->skipped = g->stalls = 0;
    g->pace_next_ns = 0;
}

static inline void rsx_present_guard_set_hidden(rsx_present_guard* g, int hidden)
{
    RSX_PG_STORE(&g->hidden, hidden ? 1 : 0);
}

/* Before an acquire. Returns 0 to skip the window this frame; otherwise 1,
 * with the acquire timeout to use. */
static inline int rsx_present_guard_before(rsx_present_guard* g, uint64_t now_ns, uint64_t* timeout_ns)
{
    if (!g->bound_ns) { *timeout_ns = UINT64_MAX; return 1; }
    if (RSX_PG_LOAD(&g->hidden)) { RSX_PG_INC(&g->skipped); return 0; }
    if (RSX_PG_LOAD(&g->present_stalled)) {
        if (now_ns < RSX_PG_LOAD(&g->probe_at_ns)) { RSX_PG_INC(&g->skipped); return 0; }
        RSX_PG_STORE(&g->probe_at_ns, now_ns + RSX_PRESENT_GUARD_PROBE_NS);   /* this is the probe */
    }
    *timeout_ns = RSX_PG_LOAD(&g->acquire_stalled) ? 0 : g->bound_ns;
    return 1;
}

/* After the acquire: got_image is 0 when it timed out or was not ready. */
static inline void rsx_present_guard_acquired(rsx_present_guard* g, int got_image)
{
    if (!g->bound_ns) return;
    if (got_image) { RSX_PG_STORE(&g->acquire_stalled, 0); return; }
    if (!RSX_PG_LOAD(&g->acquire_stalled)) RSX_PG_INC(&g->stalls);
    RSX_PG_STORE(&g->acquire_stalled, 1);
    RSX_PG_INC(&g->skipped);
}

/* A wait on the window's presentation took took_ns: over the bound, it marks
 * presentation stalled; within it, nothing (one quick call does not show the
 * window's frames get through). */
static inline int rsx_present_guard_waited(rsx_present_guard* g, uint64_t now_ns, uint64_t took_ns)
{
    if (!g->bound_ns || took_ns <= g->bound_ns) return 0;
    if (!RSX_PG_LOAD(&g->present_stalled)) RSX_PG_INC(&g->stalls);
    RSX_PG_STORE(&g->probe_at_ns, now_ns + RSX_PRESENT_GUARD_PROBE_NS);
    RSX_PG_STORE(&g->present_stalled, 1);
    return 1;
}
/* A window frame finished, all its waits taking took_ns: over the bound, as
 * above; within it, presentation is not stalled. */
static inline void rsx_present_guard_presented(rsx_present_guard* g, uint64_t now_ns, uint64_t took_ns)
{
    if (!rsx_present_guard_waited(g, now_ns, took_ns) && g->bound_ns)
        RSX_PG_STORE(&g->present_stalled, 0);
}
/* A presenter that cannot poll for an image (Metal's nextDrawable blocks, up
 * to a second, and returns nil when none came) reports each attempt here:
 * an image within the bound is a frame through; a wait over it, with or
 * without an image, marks presentation stalled. */
static inline void rsx_present_guard_drawable(rsx_present_guard* g, uint64_t now_ns, uint64_t took_ns, int got)
{
    if (!g->bound_ns) return;
    if (got) rsx_present_guard_presented(g, now_ns, took_ns);
    else rsx_present_guard_waited(g, now_ns, took_ns);
}
/* A wait for a frame slot (frames in flight on the GPU) took took_ns: it is
 * the GPU's, not the window's, so it never skips the window; returns 1 when
 * it overran the bound, for the caller to say so. */
static inline int rsx_present_guard_slot_wait(const rsx_present_guard* g, uint64_t took_ns)
{
    return g->bound_ns && took_ns > g->bound_ns;
}
/* A frame was left out of the window at now_ns: how long to wait so frames
 * left out go no faster than 60 Hz, as the display paced them. Returns 0
 * once the slot is due; a presenter that fell behind by more than a period
 * starts a new grid rather than catching up. Presenting threads only. */
static inline uint64_t rsx_present_guard_pace_ns(rsx_present_guard* g, uint64_t now_ns)
{
    const uint64_t next = g->pace_next_ns;
    if (!next || now_ns >= next) {
        g->pace_next_ns = (next && now_ns - next < RSX_PRESENT_GUARD_PACE_NS ? next : now_ns) +
                          RSX_PRESENT_GUARD_PACE_NS;
        return 0;
    }
    g->pace_next_ns = next + RSX_PRESENT_GUARD_PACE_NS;
    return next - now_ns;
}
#endif
