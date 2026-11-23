/*
 * ps3recomp - single-producer single-consumer ring of interleaved stereo float
 * frames (pure, host-independent)
 *
 * A generic building block for a pull-model host audio backend: the mixer
 * thread posts blocks, and a render callback on the device's own thread
 * consumes whatever its IO buffer asks for. Neither side may wait for the
 * other: the callback is a real-time thread, and the mixer's clock must never
 * stall on a device.
 *
 * Roles. One thread writes (the producer) and one thread reads (the consumer);
 * the roles may move between threads only across a happens-before edge (thread
 * creation, join, or a stopped output unit). `head` is written by the producer
 * only and `tail` by the consumer only. Both are free-running uint32_t counters,
 * never wrapped to the capacity: fill = head - tail is exact across the 2^32
 * wrap because the capacity is a power of two no larger than 2^30.
 *
 * Underrun is silence, overrun is a drop, and both are counted so a run's log
 * shows how often a device starved or the mixer outran it. Neither blocks.
 *
 * Real-time safety of audio_ring_read: no locks, no allocation, no syscalls; a
 * memcpy and a memset over caller storage and a few atomic operations.
 */

#ifndef PS3RECOMP_AUDIO_RING_H
#define PS3RECOMP_AUDIO_RING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Largest capacity in frames. The 2^30 bound keeps head - tail unambiguous in
 * 32 bits (it can never reach 2^31) and `2 * index` a size_t offset. */
#define AUDIO_RING_MAX_FRAMES (1u << 30)

/* Interleaved stereo: L, R per frame. */
#define AUDIO_RING_CHANNELS 2u

/* The indices sit on their own 128-byte lines (Apple's cache line) so the
 * producer's stores do not invalidate the consumer's line and the reverse. */
typedef struct audio_ring {
    float* buf;                         /* cap_frames * 2 floats, owned by the caller; read-only after init */
    uint32_t cap_frames;                /* power of two */
    uint32_t mask;                      /* cap_frames - 1 */
    _Alignas(128) uint32_t head;        /* frames ever written; producer stores, consumer loads */
    _Alignas(128) uint32_t tail;        /* frames ever read; consumer stores, producer loads */
    _Alignas(128) uint64_t dropped_frames;  /* producer only: frames audio_ring_write could not take */
    _Alignas(128) uint64_t short_events;    /* consumer only: reads that found fewer frames than asked */
    uint64_t short_frames;              /* consumer only: frames those reads filled with silence */
} audio_ring;

/* False and `ring` untouched for NULL arguments, a capacity of 0, a capacity
 * that is not a power of two, or one above AUDIO_RING_MAX_FRAMES. Call before
 * any producer or consumer runs. The storage is not cleared: reads never see a
 * frame that was not written. */
static inline bool audio_ring_init(audio_ring* ring, float* storage, uint32_t cap_frames)
{
    if (!ring || !storage || cap_frames == 0 || cap_frames > AUDIO_RING_MAX_FRAMES ||
        (cap_frames & (cap_frames - 1)) != 0)
        return false;
    ring->buf = storage;
    ring->cap_frames = cap_frames;
    ring->mask = cap_frames - 1;
    __atomic_store_n(&ring->head, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&ring->tail, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&ring->dropped_frames, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&ring->short_events, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&ring->short_frames, 0, __ATOMIC_RELAXED);
    return true;
}

/* Frames queued, 0..cap_frames, from any thread. `tail` is read before `head`
 * so a third thread never sees head < tail; a producer and a consumer that move
 * in between make the value a moment old, not wrong. */
static inline uint32_t audio_ring_fill(const audio_ring* ring)
{
    const uint32_t t = __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE);
    const uint32_t h = __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE);
    const uint32_t fill = h - t;
    return fill > ring->cap_frames ? ring->cap_frames : fill;
}

/* Producer only. Copies min(frames, free) frames and returns that count; the
 * frames that did not fit are dropped, never waited for, and added to
 * dropped_frames. */
static inline uint32_t audio_ring_write(audio_ring* ring, const float* stereo, uint32_t frames)
{
    const uint32_t h = __atomic_load_n(&ring->head, __ATOMIC_RELAXED);
    const uint32_t t = __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE);
    const uint32_t room = ring->cap_frames - (h - t);
    const uint32_t n = frames < room ? frames : room;

    if (n) {
        const uint32_t pos = h & ring->mask;
        const uint32_t first = n < ring->cap_frames - pos ? n : ring->cap_frames - pos;
        memcpy(ring->buf + (size_t)pos * AUDIO_RING_CHANNELS, stereo,
               (size_t)first * AUDIO_RING_CHANNELS * sizeof(float));
        if (n > first)
            memcpy(ring->buf, stereo + (size_t)first * AUDIO_RING_CHANNELS,
                   (size_t)(n - first) * AUDIO_RING_CHANNELS * sizeof(float));
        /* Publishes the frames above to the consumer's acquire load of head. */
        __atomic_store_n(&ring->head, h + n, __ATOMIC_RELEASE);
    }
    if (n < frames) {
        __atomic_store_n(&ring->dropped_frames,
                         __atomic_load_n(&ring->dropped_frames, __ATOMIC_RELAXED) + (frames - n),
                         __ATOMIC_RELAXED);
    }
    return n;
}

/* Consumer only; real-time safe. Copies min(frames, fill) frames into `out` and
 * zero-fills the rest of the `frames` the caller asked for, so `out` is always
 * fully defined. Returns the frames that came from the ring; a smaller count is
 * an underrun, counted in short_events and short_frames. */
static inline uint32_t audio_ring_read(audio_ring* ring, float* out, uint32_t frames)
{
    const uint32_t t = __atomic_load_n(&ring->tail, __ATOMIC_RELAXED);
    const uint32_t h = __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE);
    const uint32_t avail = h - t;
    const uint32_t n = frames < avail ? frames : avail;

    if (n) {
        const uint32_t pos = t & ring->mask;
        const uint32_t first = n < ring->cap_frames - pos ? n : ring->cap_frames - pos;
        memcpy(out, ring->buf + (size_t)pos * AUDIO_RING_CHANNELS,
               (size_t)first * AUDIO_RING_CHANNELS * sizeof(float));
        if (n > first)
            memcpy(out + (size_t)first * AUDIO_RING_CHANNELS, ring->buf,
                   (size_t)(n - first) * AUDIO_RING_CHANNELS * sizeof(float));
        /* Hands the slots back to the producer's acquire load of tail: its next
         * write into them happens after the copy above. */
        __atomic_store_n(&ring->tail, t + n, __ATOMIC_RELEASE);
    }
    if (n < frames) {
        memset(out + (size_t)n * AUDIO_RING_CHANNELS, 0,
               (size_t)(frames - n) * AUDIO_RING_CHANNELS * sizeof(float));
        __atomic_store_n(&ring->short_events,
                         __atomic_load_n(&ring->short_events, __ATOMIC_RELAXED) + 1, __ATOMIC_RELAXED);
        __atomic_store_n(&ring->short_frames,
                         __atomic_load_n(&ring->short_frames, __ATOMIC_RELAXED) + (frames - n),
                         __ATOMIC_RELAXED);
    }
    return n;
}

/* Discards every queued frame. Precondition: neither a producer nor a consumer
 * runs, e.g. the producer calls it while the output unit is stopped. It moves
 * only `tail` (to `head`), so the indices keep counting and the counters keep
 * the run's totals. */
static inline void audio_ring_reset(audio_ring* ring)
{
    __atomic_store_n(&ring->tail, __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
}

static inline uint64_t audio_ring_dropped_frames(const audio_ring* ring)
{
    return __atomic_load_n(&ring->dropped_frames, __ATOMIC_RELAXED);
}

static inline uint64_t audio_ring_short_events(const audio_ring* ring)
{
    return __atomic_load_n(&ring->short_events, __ATOMIC_RELAXED);
}

static inline uint64_t audio_ring_short_frames(const audio_ring* ring)
{
    return __atomic_load_n(&ring->short_frames, __ATOMIC_RELAXED);
}

#endif /* PS3RECOMP_AUDIO_RING_H */
