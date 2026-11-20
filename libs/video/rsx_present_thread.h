/* Opt-in present thread: PS3RECOMP_RSX_PRESENT_THREAD=1.
 *
 * Without it a present runs on the thread that walks the FIFO. That thread
 * then waits for the window system to hand out a display image, and while it
 * waits it applies no guest-visible effect (labels, reports, get/ref) the
 * guest may be polling.
 *
 * With it, a present becomes two halves:
 *  - stage (the presenting thread, normally the walker): the backend records
 *    and submits, in queue order behind everything recorded so far, a GPU copy
 *    of the flipped surface into one of a few staging images. It never waits
 *    for a display image.
 *  - show (a thread of its own, started here): it takes the oldest staged
 *    frame, acquires a display image, copies the staging image into it and
 *    presents it.
 *
 * The ring holds RSX_PRESENT_THREAD_MAX_DEPTH staging slots at most. A slot
 * is FREE, STAGING (the presenting thread is recording into it), READY
 * (staged, waiting for the show thread) or SHOWING (the show thread owns it).
 * When no slot is free the oldest READY frame is dropped and counted, never
 * waited for: there is one show thread, so with two slots or more one is
 * always FREE or READY.
 *
 * Locks: the ring has one mutex, held only to change slot states and
 * counters, never across a backend hook. A backend's show hook must take
 * none of the locks its presenting thread holds while it submits, so the two
 * cannot form a lock-order cycle.
 *
 * Header-only, so every translation unit that includes a backend also gets
 * this without a build change. POSIX threads; on Windows it reports itself
 * unsupported and the caller keeps its ordinary present. */
#ifndef RSX_PRESENT_THREAD_H
#define RSX_PRESENT_THREAD_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RSX_PRESENT_THREAD_MAX_DEPTH 3u
#define RSX_PRESENT_THREAD_MIN_DEPTH 2u

/* What the show hook measured. */
typedef struct rsx_present_show_timing {
    uint64_t acquire_ns;  /* waiting for a display image (and any in-flight bound) */
    uint64_t blit_ns;     /* recording, submitting and queueing the present */
} rsx_present_show_timing;

typedef struct rsx_present_hooks {
    void *user;
    /* Presenting thread. Record and submit the GPU copy of `surface` into
     * staging slot `slot`, ordered after all rendering submitted or recorded so
     * far and before any later write to that surface. Must not wait for a
     * display image. (Re)creates the slot's staging image when the display size
     * changed: the slot is the caller's alone while this runs. 1 = staged. */
    int  (*stage)(void *user, uint32_t slot, uint32_t surface);
    /* Show thread. Acquire a display image, copy slot `slot` into it and
     * present. 1 = presented, 0 = no image (the frame is not shown). */
    int  (*show)(void *user, uint32_t slot, rsx_present_show_timing *timing);
    /* Either thread, ring lock not held: the frame staged in `slot` will never
     * be shown (dropped for a newer one, or left over at shutdown). */
    void (*discard)(void *user, uint32_t slot);
} rsx_present_hooks;

typedef struct rsx_present_thread_stats {
    uint64_t staged, presented, dropped, stage_failed, show_failed, discarded;
    uint32_t depth, occupancy, max_occupancy;
    uint64_t stage_ns_total, stage_ns_max;
    uint64_t acquire_ns_total, acquire_ns_max;
    uint64_t blit_ns_total, blit_ns_max;
} rsx_present_thread_stats;

/* The switch. Read where a backend comes up, never per frame. */
static inline int rsx_present_thread_requested(void)
{
    const char *e = getenv("PS3RECOMP_RSX_PRESENT_THREAD");
    return e && !strcmp(e, "1");
}

/* PS3RECOMP_RSX_PRESENT_THREAD_DEPTH: 2 or 3 staging slots, default 3. */
static inline uint32_t rsx_present_thread_depth_from(const char *e)
{
    if (!e || !*e) return RSX_PRESENT_THREAD_MAX_DEPTH;
    const long v = strtol(e, NULL, 10);
    if (v < (long)RSX_PRESENT_THREAD_MIN_DEPTH) return RSX_PRESENT_THREAD_MIN_DEPTH;
    if (v > (long)RSX_PRESENT_THREAD_MAX_DEPTH) return RSX_PRESENT_THREAD_MAX_DEPTH;
    return (uint32_t)v;
}

/* A backend that cannot run the show on a thread of its own says so once
 * and keeps its ordinary present. */
static inline void rsx_present_thread_unsupported(const char *backend, const char *why)
{
    static int said;
    if (said++) return;
    fprintf(stderr, "[rsx present-thread] present thread unsupported on this backend (%s%s%s); "
            "presents stay on the presenting thread\n", backend ? backend : "?",
            why ? ": " : "", why ? why : "");
}

#if defined(_WIN32)

typedef struct rsx_present_thread rsx_present_thread;
static inline rsx_present_thread *rsx_present_thread_create(const rsx_present_hooks *hooks, uint32_t depth)
{ (void)hooks; (void)depth; rsx_present_thread_unsupported("windows", "no POSIX threads"); return NULL; }
static inline int  rsx_present_thread_submit(rsx_present_thread *pt, uint32_t surface)
{ (void)pt; (void)surface; return 0; }
static inline void rsx_present_thread_destroy(rsx_present_thread *pt) { (void)pt; }
static inline void rsx_present_thread_get_stats(rsx_present_thread *pt, rsx_present_thread_stats *out)
{ (void)pt; if (out) memset(out, 0, sizeof *out); }

#else

#include <pthread.h>
#include <time.h>

enum { RSX_PT_FREE = 0, RSX_PT_STAGING, RSX_PT_READY, RSX_PT_SHOWING };

typedef struct rsx_present_thread {
    rsx_present_hooks hooks;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t thread;
    int started, stop;
    uint32_t depth;
    int state[RSX_PRESENT_THREAD_MAX_DEPTH];
    uint64_t seq[RSX_PRESENT_THREAD_MAX_DEPTH];   /* READY order */
    uint64_t next_seq;
    rsx_present_thread_stats stats;
    /* The periodic line: counts since the last one, and its clock. */
    rsx_present_thread_stats last;
    uint64_t window_stage_max, window_acquire_max, window_blit_max;
    uint64_t report_at_ns, report_period_ns;
} rsx_present_thread;

static inline uint64_t rsx_present_thread_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* Ring lock held. READY slots waiting, and the slot of the oldest. */
static inline uint32_t rsx_pt_ready_locked(const rsx_present_thread *pt, uint32_t *oldest)
{
    uint32_t n = 0, best = pt->depth;
    for (uint32_t i = 0; i < pt->depth; i++) {
        if (pt->state[i] != RSX_PT_READY) continue;
        if (best == pt->depth || pt->seq[i] < pt->seq[best]) best = i;
        n++;
    }
    if (oldest) *oldest = best;
    return n;
}

/* Ring lock held: slots holding a frame not yet shown (STAGING, READY or SHOWING). */
static inline uint32_t rsx_pt_occupancy_locked(const rsx_present_thread *pt)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < pt->depth; i++) n += pt->state[i] != RSX_PT_FREE;
    return n;
}

static inline void rsx_pt_max(uint64_t *m, uint64_t v) { if (v > *m) *m = v; }

/* Ring lock held: the periodic stderr line, at most once per period. */
static inline void rsx_pt_report_locked(rsx_present_thread *pt, uint64_t now, int final)
{
    if (!final && (!pt->report_period_ns || now < pt->report_at_ns)) return;
    pt->report_at_ns = now + pt->report_period_ns;
    const rsx_present_thread_stats *s = &pt->stats, *l = &pt->last;
    const uint64_t staged = s->staged - l->staged, shown = s->presented - l->presented;
    const uint64_t shows = shown + (s->show_failed - l->show_failed);
    const double stage_ms = staged ? (double)(s->stage_ns_total - l->stage_ns_total) / staged / 1e6 : 0.0;
    const double acquire_ms = shows ? (double)(s->acquire_ns_total - l->acquire_ns_total) / shows / 1e6 : 0.0;
    const double blit_ms = shown ? (double)(s->blit_ns_total - l->blit_ns_total) / shown / 1e6 : 0.0;
    fprintf(stderr, "[rsx present-thread] %sstaged=%llu presented=%llu dropped=%llu stage_failed=%llu "
            "show_failed=%llu discarded=%llu ring_max=%u/%u stage_ms avg=%.3f max=%.3f "
            "acquire_ms avg=%.3f max=%.3f blit_ms avg=%.3f max=%.3f\n",
            final ? "final " : "",
            (unsigned long long)s->staged, (unsigned long long)s->presented,
            (unsigned long long)s->dropped, (unsigned long long)s->stage_failed,
            (unsigned long long)s->show_failed, (unsigned long long)s->discarded,
            s->max_occupancy, pt->depth,
            stage_ms, (double)pt->window_stage_max / 1e6,
            acquire_ms, (double)pt->window_acquire_max / 1e6,
            blit_ms, (double)pt->window_blit_max / 1e6);
    pt->last = *s;
    pt->window_stage_max = pt->window_acquire_max = pt->window_blit_max = 0;
}

static inline void *rsx_present_thread_main(void *arg)
{
    rsx_present_thread *pt = (rsx_present_thread *)arg;
    pthread_mutex_lock(&pt->mu);
    for (;;) {
        uint32_t slot = 0;
        while (!pt->stop && !rsx_pt_ready_locked(pt, &slot)) pthread_cond_wait(&pt->cv, &pt->mu);
        if (pt->stop) break;
        pt->state[slot] = RSX_PT_SHOWING;
        pthread_mutex_unlock(&pt->mu);
        rsx_present_show_timing t = {0, 0};
        const int shown = pt->hooks.show(pt->hooks.user, slot, &t);
        pthread_mutex_lock(&pt->mu);
        /* Free as soon as the show is submitted: whatever stages into the slot
         * next is queued behind the show's read of it. */
        pt->state[slot] = RSX_PT_FREE;
        if (shown) pt->stats.presented++; else pt->stats.show_failed++;
        pt->stats.acquire_ns_total += t.acquire_ns;
        rsx_pt_max(&pt->stats.acquire_ns_max, t.acquire_ns);
        rsx_pt_max(&pt->window_acquire_max, t.acquire_ns);
        if (shown) {
            pt->stats.blit_ns_total += t.blit_ns;
            rsx_pt_max(&pt->stats.blit_ns_max, t.blit_ns);
            rsx_pt_max(&pt->window_blit_max, t.blit_ns);
        }
        pt->stats.occupancy = rsx_pt_occupancy_locked(pt);
        pthread_cond_broadcast(&pt->cv);
    }
    pthread_mutex_unlock(&pt->mu);
    return NULL;
}

/* PS3RECOMP_RSX_PRESENT_THREAD_REPORT_S: seconds between the periodic lines
 * (default 5, 0 = only the final one). */
static inline rsx_present_thread *rsx_present_thread_create(const rsx_present_hooks *hooks, uint32_t depth)
{
    if (!hooks || !hooks->stage || !hooks->show || !hooks->discard) return NULL;
    if (depth < RSX_PRESENT_THREAD_MIN_DEPTH) depth = RSX_PRESENT_THREAD_MIN_DEPTH;
    if (depth > RSX_PRESENT_THREAD_MAX_DEPTH) depth = RSX_PRESENT_THREAD_MAX_DEPTH;
    rsx_present_thread *pt = (rsx_present_thread *)calloc(1, sizeof *pt);
    if (!pt) return NULL;
    pt->hooks = *hooks;
    pt->depth = depth;
    pt->stats.depth = depth;
    { const char *e = getenv("PS3RECOMP_RSX_PRESENT_THREAD_REPORT_S");
      const double s = e && *e ? strtod(e, NULL) : 5.0;
      pt->report_period_ns = s > 0.0 ? (uint64_t)(s * 1e9) : 0; }
    pt->report_at_ns = rsx_present_thread_now() + pt->report_period_ns;
    if (pthread_mutex_init(&pt->mu, NULL)) { free(pt); return NULL; }
    if (pthread_cond_init(&pt->cv, NULL)) { pthread_mutex_destroy(&pt->mu); free(pt); return NULL; }
    if (pthread_create(&pt->thread, NULL, rsx_present_thread_main, pt)) {
        pthread_cond_destroy(&pt->cv); pthread_mutex_destroy(&pt->mu); free(pt);
        return NULL;
    }
    pt->started = 1;
    fprintf(stderr, "[rsx present-thread] on: %u staging slots; presents leave the presenting thread\n", depth);
    return pt;
}

/* Presenting thread: stage `surface` for the show thread. Never waits for the
 * show thread or a display image: with no slot free the oldest staged frame
 * is dropped instead. 1 = staged. */
static inline int rsx_present_thread_submit(rsx_present_thread *pt, uint32_t surface)
{
    if (!pt) return 0;
    pthread_mutex_lock(&pt->mu);
    uint32_t slot = pt->depth, oldest = pt->depth;
    for (uint32_t i = 0; i < pt->depth; i++)
        if (pt->state[i] == RSX_PT_FREE) { slot = i; break; }
    int dropped = 0;
    if (slot == pt->depth && rsx_pt_ready_locked(pt, &oldest)) {
        slot = oldest;
        dropped = 1;
        pt->stats.dropped++;
    }
    if (slot == pt->depth) {
        /* Not reachable with one show thread and two slots or more; refuse
         * rather than wait. */
        pt->stats.stage_failed++;
        pthread_mutex_unlock(&pt->mu);
        return 0;
    }
    pt->state[slot] = RSX_PT_STAGING;
    pthread_mutex_unlock(&pt->mu);

    if (dropped) pt->hooks.discard(pt->hooks.user, slot);
    const uint64_t t0 = rsx_present_thread_now();
    const int ok = pt->hooks.stage(pt->hooks.user, slot, surface);
    const uint64_t t1 = rsx_present_thread_now();

    pthread_mutex_lock(&pt->mu);
    if (ok) {
        pt->state[slot] = RSX_PT_READY;
        pt->seq[slot] = ++pt->next_seq;
        pt->stats.staged++;
        pt->stats.stage_ns_total += t1 - t0;
        rsx_pt_max(&pt->stats.stage_ns_max, t1 - t0);
        rsx_pt_max(&pt->window_stage_max, t1 - t0);
        pthread_cond_broadcast(&pt->cv);
    } else {
        pt->state[slot] = RSX_PT_FREE;
        pt->stats.stage_failed++;
    }
    pt->stats.occupancy = rsx_pt_occupancy_locked(pt);
    if (pt->stats.occupancy > pt->stats.max_occupancy) pt->stats.max_occupancy = pt->stats.occupancy;
    rsx_pt_report_locked(pt, t1, 0);
    pthread_mutex_unlock(&pt->mu);
    return ok;
}

static inline void rsx_present_thread_get_stats(rsx_present_thread *pt, rsx_present_thread_stats *out)
{
    if (!out) return;
    if (!pt) { memset(out, 0, sizeof *out); return; }
    pthread_mutex_lock(&pt->mu);
    *out = pt->stats;
    pthread_mutex_unlock(&pt->mu);
}

/* Stop the show thread (it finishes the show it is in), join it, and discard
 * every frame still staged. The backend then frees its staging images. Must
 * not be called from the show thread or with the ring in use by a submit. */
static inline void rsx_present_thread_destroy(rsx_present_thread *pt)
{
    if (!pt) return;
    pthread_mutex_lock(&pt->mu);
    pt->stop = 1;
    pthread_cond_broadcast(&pt->cv);
    pthread_mutex_unlock(&pt->mu);
    if (pt->started) pthread_join(pt->thread, NULL);
    for (uint32_t i = 0; i < pt->depth; i++) {
        if (pt->state[i] != RSX_PT_READY) continue;
        pt->hooks.discard(pt->hooks.user, i);
        pt->state[i] = RSX_PT_FREE;
        pt->stats.discarded++;
    }
    pt->stats.occupancy = 0;
    rsx_pt_report_locked(pt, rsx_present_thread_now(), 1);
    pthread_cond_destroy(&pt->cv);
    pthread_mutex_destroy(&pt->mu);
    free(pt);
}

#endif /* !_WIN32 */
#endif /* RSX_PRESENT_THREAD_H */
