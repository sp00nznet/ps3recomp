/*
 * ps3recomp - the clock NV4097_GET_REPORT stamps its reports with.
 *
 * A report's time stamp is when the RSX reached the command. The walker
 * that stands in for the RSX reaches it after converting every draw before
 * it on the host, which takes far longer than the RSX took, so a title that
 * times its frames on the GPU with reports reads that host work as GPU load.
 * Resistance 3 does: three marker reports a frame give it the GPU frame time,
 * and above 0.84-0.90 of its frame budget it drops decor (cloth, bushes,
 * saplings) until the time comes down.
 *
 * The clock is the host's monotonic time less (1 - scale) of the time the
 * walker has spent in drain passes. scale 1 (the default) is the plain host
 * time; scale 0 is an RSX whose own work costs nothing, whose clock moves only
 * while it waits for commands, as a starved RSX's does. Stamps rise strictly.
 *
 * Plain functions of the caller's time, so a test can drive them; the walker
 * calls them under its FIFO lock.
 */
#ifndef PS3RECOMP_GCM_REPORT_CLOCK_H
#define PS3RECOMP_GCM_REPORT_CLOCK_H

#include <stdint.h>
#include <stdlib.h>

typedef struct gcm_report_clock {
    double   scale;      /* share of drain time that reaches the stamps, 0..1 */
    uint64_t busy_ns;    /* drain time of the passes that have ended          */
    uint64_t drain_t0;   /* start of the pass under way                       */
    int      in_drain;
    uint64_t last;       /* last stamp handed out                             */
} gcm_report_clock;

/* A PS3RECOMP_GCM_REPORT_BUSY_SCALE value. Unset, empty or not a number
 * (trailing text included: "off", "0x") is 1, the host time, so a value meant
 * to turn the knob off never turns it fully on. */
static inline double gcm_report_clock_parse_scale(const char* s)
{
    if (!s || !*s) return 1.0;
    char* end = NULL;
    const double v = strtod(s, &end);
    if (end == s) return 1.0;
    while (*end == ' ' || *end == '\t') end++;
    return *end == '\0' ? v : 1.0;
}

/* scale outside 0..1 (or not a number) is taken as 1. */
static inline void gcm_report_clock_init(gcm_report_clock* c, double scale)
{
    c->scale = (scale >= 0.0 && scale <= 1.0) ? scale : 1.0;
    c->busy_ns = 0;
    c->drain_t0 = 0;
    c->in_drain = 0;
    c->last = 0;
}

static inline void gcm_report_clock_drain_begin(gcm_report_clock* c, uint64_t now)
{
    c->drain_t0 = now;
    c->in_drain = 1;
}

static inline void gcm_report_clock_drain_end(gcm_report_clock* c, uint64_t now)
{
    if (c->in_drain && now > c->drain_t0) c->busy_ns += now - c->drain_t0;
    c->in_drain = 0;
}

static inline uint64_t gcm_report_clock_stamp(gcm_report_clock* c, uint64_t now)
{
    uint64_t t = now;
    if (c->scale < 1.0) {
        uint64_t busy = c->busy_ns;
        if (c->in_drain && now > c->drain_t0) busy += now - c->drain_t0;
        const uint64_t hidden = (uint64_t)((double)busy * (1.0 - c->scale));
        t = hidden < now ? now - hidden : 0;
    }
    if (t <= c->last) t = c->last + 1u;
    c->last = t;
    return t;
}

#endif /* PS3RECOMP_GCM_REPORT_CLOCK_H */
