#ifndef RSX_DLSS_MOTION_STATS_H
#define RSX_DLSS_MOTION_STATS_H

#include <stdint.h>

typedef struct rsx_dlss_motion_counts {
    uint32_t tracked, disoccluded, matched, fallback, background, reset;
} rsx_dlss_motion_counts;

typedef struct rsx_dlss_motion_stats {
    uint64_t total, eligible;
    int valid, coverage_available;
    double matched_percent, fallback_percent;
} rsx_dlss_motion_stats;

static inline rsx_dlss_motion_stats rsx_dlss_summarize_motion(
    rsx_dlss_motion_counts c, uint32_t width, uint32_t height)
{
    rsx_dlss_motion_stats s = {0, 0, 0, 0, 0.0, 0.0};
    s.eligible = (uint64_t)c.matched + (uint64_t)c.fallback;
    s.total = s.eligible + (uint64_t)c.background + (uint64_t)c.reset;
    s.valid = width != 0 && height != 0 &&
              s.total == (uint64_t)width * (uint64_t)height &&
              (uint64_t)c.tracked == s.eligible &&
              c.disoccluded <= c.tracked;
    s.coverage_available = s.valid && s.eligible != 0;
    if (s.coverage_available) {
        s.matched_percent = 100.0 * (double)c.matched / (double)s.eligible;
        s.fallback_percent = 100.0 * (double)c.fallback / (double)s.eligible;
    }
    return s;
}

#endif
