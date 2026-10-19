/*
 * spurs_image_timing.h - source-keyed audio-native dispatch profiling.
 *
 * Diagnostic instrumentation only; recording changes no behavior.
 * Header-only, C17 and C++ compatible, no dynamic allocation and no
 * global or thread-local state. The caller owns one aggregate per chain,
 * calls into it only when its opt-in diagnostic flag is set, and emits and
 * resets it once per window (nominally SPURS_IMAGE_TIMING_WINDOW_US).
 *
 * Evidence semantics:
 *  - Keys are the admitted image fingerprint plus image size, never a
 *    descriptor address. A key identifies an admitted image only; it says
 *    nothing about images that were rejected or never dispatched.
 *  - dispatch_us is wall-clock dispatch duration supplied by the caller.
 *    It includes descheduling and is neither CPU time nor evidence that
 *    the audio output was accepted.
 */
#ifndef SPURS_IMAGE_TIMING_H
#define SPURS_IMAGE_TIMING_H

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#define SPURS_IMAGE_TIMING_CAPACITY 32u
#define SPURS_IMAGE_TIMING_WINDOW_US 5000000u

/* One key's totals. Overflow and unknown buckets reuse this layout with
 * fingerprint and image_size left at zero. */
struct spurs_image_timing_entry {
    uint64_t fingerprint;
    uint32_t image_size;
    uint64_t calls;
    uint64_t dispatch_us;
    uint64_t max_us;
};

struct spurs_image_timing {
    /* Known keys in first-seen order; entries [0, used) are valid. */
    struct spurs_image_timing_entry entries[SPURS_IMAGE_TIMING_CAPACITY];
    uint32_t used;
    /* New valid keys seen after entries[] is full; nothing is replaced. */
    struct spurs_image_timing_entry overflow;
    /* Records with image_size == 0; never merged with a known key. */
    struct spurs_image_timing_entry unknown;
    /* Nonzero once any count or total clamped at UINT64_MAX; kept until
     * reset. */
    int saturated;
};

static inline void spurs_image_timing_entry_clear_(struct spurs_image_timing_entry *e)
{
    e->fingerprint = 0;
    e->image_size = 0;
    e->calls = 0;
    e->dispatch_us = 0;
    e->max_us = 0;
}

static inline uint64_t spurs_image_timing_sat_add_(uint64_t a, uint64_t b, int *saturated)
{
    if (a > UINT64_MAX - b) {
        *saturated = 1;
        return UINT64_MAX;
    }
    return a + b;
}

static inline void spurs_image_timing_entry_record_(struct spurs_image_timing_entry *e,
                                                    uint64_t dispatch_us, int *saturated)
{
    e->calls = spurs_image_timing_sat_add_(e->calls, 1, saturated);
    e->dispatch_us = spurs_image_timing_sat_add_(e->dispatch_us, dispatch_us, saturated);
    if (dispatch_us > e->max_us)
        e->max_us = dispatch_us;
}

/* Clears all keys, buckets and the saturation flag. Null is ignored. */
static inline void spurs_image_timing_reset(struct spurs_image_timing *t)
{
    uint32_t i;
    if (!t)
        return;
    for (i = 0; i < SPURS_IMAGE_TIMING_CAPACITY; ++i)
        spurs_image_timing_entry_clear_(&t->entries[i]);
    t->used = 0;
    spurs_image_timing_entry_clear_(&t->overflow);
    spurs_image_timing_entry_clear_(&t->unknown);
    t->saturated = 0;
}

/* Records one dispatch of an admitted image. A zero fingerprint is a valid
 * key when image_size > 0; image_size == 0 goes to the unknown bucket.
 * Null is ignored. */
static inline void spurs_image_timing_add(struct spurs_image_timing *t, uint64_t fingerprint,
                                          uint32_t image_size, uint64_t dispatch_us)
{
    uint32_t i;
    uint32_t n;
    struct spurs_image_timing_entry *e;

    if (!t)
        return;
    if (image_size == 0) {
        spurs_image_timing_entry_record_(&t->unknown, dispatch_us, &t->saturated);
        return;
    }
    n = t->used < SPURS_IMAGE_TIMING_CAPACITY ? t->used : SPURS_IMAGE_TIMING_CAPACITY;
    for (i = 0; i < n; ++i) {
        e = &t->entries[i];
        if (e->fingerprint == fingerprint && e->image_size == image_size) {
            spurs_image_timing_entry_record_(e, dispatch_us, &t->saturated);
            return;
        }
    }
    if (n < SPURS_IMAGE_TIMING_CAPACITY) {
        e = &t->entries[n];
        spurs_image_timing_entry_clear_(e);
        e->fingerprint = fingerprint;
        e->image_size = image_size;
        t->used = n + 1;
        spurs_image_timing_entry_record_(e, dispatch_us, &t->saturated);
        return;
    }
    spurs_image_timing_entry_record_(&t->overflow, dispatch_us, &t->saturated);
}

static inline int spurs_image_timing_emit_entry_(const struct spurs_image_timing_entry *e,
                                                 FILE *out, uint32_t chain, uint64_t start_us,
                                                 uint64_t end_us, const char *bucket,
                                                 int saturated)
{
    if (e->calls == 0)
        return 0;
    if (fprintf(out,
                "[spurs-image-window] chain=0x%08" PRIX32 " start_us=%" PRIu64
                " end_us=%" PRIu64 " fp=0x%016" PRIX64 " image_size=%" PRIu32
                " calls=%" PRIu64 " dispatch_us=%" PRIu64 " max_us=%" PRIu64
                " bucket=%s saturated=%d\n",
                chain, start_us, end_us, e->fingerprint, e->image_size, e->calls,
                e->dispatch_us, e->max_us, bucket, saturated ? 1 : 0) < 0)
        return -1;
    return 0;
}

/* Writes one line per non-empty known key (first-seen order), then the
 * overflow and unknown buckets if non-empty: at most CAPACITY + 2 lines.
 * Returns 0 on success, -1 on null arguments, end_us < start_us (nothing
 * written) or a write failure. Does not reset the aggregate. */
static inline int spurs_image_timing_emit(const struct spurs_image_timing *t, FILE *out,
                                          uint32_t chain, uint64_t start_us, uint64_t end_us)
{
    uint32_t i;
    uint32_t n;

    if (!t || !out || end_us < start_us)
        return -1;
    n = t->used < SPURS_IMAGE_TIMING_CAPACITY ? t->used : SPURS_IMAGE_TIMING_CAPACITY;
    for (i = 0; i < n; ++i) {
        if (spurs_image_timing_emit_entry_(&t->entries[i], out, chain, start_us, end_us,
                                           "known", t->saturated) != 0)
            return -1;
    }
    if (spurs_image_timing_emit_entry_(&t->overflow, out, chain, start_us, end_us, "overflow",
                                       t->saturated) != 0)
        return -1;
    if (spurs_image_timing_emit_entry_(&t->unknown, out, chain, start_us, end_us, "unknown",
                                       t->saturated) != 0)
        return -1;
    return 0;
}

#endif /* SPURS_IMAGE_TIMING_H */
