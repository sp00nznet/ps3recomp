/*
 * Host audio SPSC ring (libs/audio/audio_ring.h): the contract a pull-model
 * backend relies on -- one mixer thread writing, one real-time callback
 * reading. Plain C17 and pthreads, no runtime.
 *
 *  init      rejects NULL, 0, non-power-of-two and > 2^30 frames, and leaves the ring alone
 *  wrap      frames written across the end of the storage read back in order
 *  short     a read of more than is queued returns what there is, zero-fills exactly the
 *            missing tail (and nothing past it) and counts the event and the frames
 *  full      a write into a full ring takes what fits, returns it, counts what it dropped
 *  fill      fill follows every write and read, to the capacity
 *  reset     empties the ring and keeps the counters
 *  index     head and tail wrapping past 2^32 keep fill, order and room exact
 *  stress    two threads, odd chunk sizes, strict continuity and exact accounting
 */

#define _POSIX_C_SOURCE 200809L
#include "../audio_ring.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>

/* assert() vanishes under NDEBUG; a contract test must not. */
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            fprintf(stderr, "test_audio_ring_spsc: %s:%d: CHECK failed: %s\n", __FILE__,  \
                    __LINE__, #cond);                                                     \
            exit(1);                                                                      \
        }                                                                                 \
    } while (0)

#define SENTINEL 7.0f

/* Frame number n as a stereo frame: L = n, R = -n. n >= 1, so no real frame is all zero
 * and a zero-filled frame cannot pass for one. */
static void make_frames(float* dst, uint32_t first, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i) {
        dst[2 * i] = (float)(first + i);
        dst[2 * i + 1] = -(float)(first + i);
    }
}

static int frames_match(const float* src, uint32_t first, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i)
        if (src[2 * i] != (float)(first + i) || src[2 * i + 1] != -(float)(first + i))
            return 0;
    return 1;
}

static void test_init(void)
{
    float storage[16 * 2];
    audio_ring ring;
    memset(&ring, 0xA5, sizeof ring);
    const audio_ring before = ring;

    CHECK(!audio_ring_init(&ring, storage, 0));
    CHECK(!audio_ring_init(&ring, storage, 3));
    CHECK(!audio_ring_init(&ring, storage, 1000));
    CHECK(!audio_ring_init(&ring, storage, AUDIO_RING_MAX_FRAMES + 1));
    CHECK(!audio_ring_init(&ring, storage, 0x80000000u));
    CHECK(!audio_ring_init(&ring, NULL, 16));
    CHECK(!audio_ring_init(NULL, storage, 16));
    CHECK(memcmp(&ring, &before, sizeof ring) == 0);   /* a refusal leaves the ring as it was */

    CHECK(audio_ring_init(&ring, storage, 1));
    CHECK(audio_ring_init(&ring, storage, 16));
    CHECK(audio_ring_fill(&ring) == 0);
    CHECK(audio_ring_dropped_frames(&ring) == 0 && audio_ring_short_events(&ring) == 0 &&
          audio_ring_short_frames(&ring) == 0);
    /* The largest accepted capacity; the storage is never touched here. */
    CHECK(audio_ring_init(&ring, storage, AUDIO_RING_MAX_FRAMES));
}

static void test_wrap_order(void)
{
    float storage[8 * 2], in[8 * 2], out[8 * 2];
    audio_ring ring;
    CHECK(audio_ring_init(&ring, storage, 8));

    make_frames(in, 1, 6);
    CHECK(audio_ring_write(&ring, in, 6) == 6);
    CHECK(audio_ring_read(&ring, out, 6) == 6 && frames_match(out, 1, 6));

    /* Positions 6, 7 then 0, 1, 2: the write and the read both straddle the end. */
    make_frames(in, 7, 5);
    CHECK(audio_ring_write(&ring, in, 5) == 5);
    CHECK(audio_ring_fill(&ring) == 5);
    CHECK(audio_ring_read(&ring, out, 5) == 5 && frames_match(out, 7, 5));
    CHECK(audio_ring_fill(&ring) == 0);
    CHECK(audio_ring_short_events(&ring) == 0 && audio_ring_dropped_frames(&ring) == 0);

    /* Many laps with a chunk size that never divides the capacity. */
    uint32_t next_w = 12, next_r = 12;
    for (int lap = 0; lap < 200; ++lap) {
        make_frames(in, next_w, 5);
        CHECK(audio_ring_write(&ring, in, 5) == 5);
        next_w += 5;
        CHECK(audio_ring_read(&ring, out, 3) == 3 && frames_match(out, next_r, 3));
        next_r += 3;
        CHECK(audio_ring_read(&ring, out, 2) == 2 && frames_match(out, next_r, 2));
        next_r += 2;
    }
    CHECK(audio_ring_fill(&ring) == 0);
}

static void test_short_read(void)
{
    float storage[8 * 2], in[8 * 2], out[12 * 2];
    audio_ring ring;
    CHECK(audio_ring_init(&ring, storage, 8));
    make_frames(in, 1, 3);
    CHECK(audio_ring_write(&ring, in, 3) == 3);

    for (size_t i = 0; i < 12 * 2; ++i)
        out[i] = SENTINEL;
    CHECK(audio_ring_read(&ring, out, 10) == 3);           /* asked 10, there were 3 */
    CHECK(frames_match(out, 1, 3));
    for (size_t i = 3 * 2; i < 10 * 2; ++i)                 /* the 7 missing frames are silence */
        CHECK(out[i] == 0.0f);
    for (size_t i = 10 * 2; i < 12 * 2; ++i)                /* and nothing past the request is written */
        CHECK(out[i] == SENTINEL);
    CHECK(audio_ring_short_events(&ring) == 1);
    CHECK(audio_ring_short_frames(&ring) == 7);
    CHECK(audio_ring_fill(&ring) == 0);

    /* Empty ring: the whole request is silence. */
    CHECK(audio_ring_read(&ring, out, 4) == 0);
    for (size_t i = 0; i < 4 * 2; ++i)
        CHECK(out[i] == 0.0f);
    CHECK(audio_ring_short_events(&ring) == 2);
    CHECK(audio_ring_short_frames(&ring) == 11);

    /* A read that is met in full, and a read of nothing, are not underruns. */
    make_frames(in, 4, 4);
    CHECK(audio_ring_write(&ring, in, 4) == 4);
    CHECK(audio_ring_read(&ring, out, 4) == 4 && frames_match(out, 4, 4));
    CHECK(audio_ring_read(&ring, out, 0) == 0);
    CHECK(audio_ring_short_events(&ring) == 2 && audio_ring_short_frames(&ring) == 11);
}

static void test_full_write(void)
{
    float storage[8 * 2], in[8 * 2], out[8 * 2];
    audio_ring ring;
    CHECK(audio_ring_init(&ring, storage, 8));

    make_frames(in, 1, 8);
    CHECK(audio_ring_write(&ring, in, 8) == 8);
    CHECK(audio_ring_fill(&ring) == 8);
    CHECK(audio_ring_dropped_frames(&ring) == 0);

    make_frames(in, 9, 3);
    CHECK(audio_ring_write(&ring, in, 3) == 0);             /* full: nothing taken, 3 dropped */
    CHECK(audio_ring_dropped_frames(&ring) == 3);
    CHECK(audio_ring_fill(&ring) == 8);

    CHECK(audio_ring_read(&ring, out, 2) == 2 && frames_match(out, 1, 2));
    make_frames(in, 9, 5);
    CHECK(audio_ring_write(&ring, in, 5) == 2);             /* room for 2 of the 5 */
    CHECK(audio_ring_dropped_frames(&ring) == 6);
    CHECK(audio_ring_fill(&ring) == 8);

    /* What was accepted is 3..8 then 9, 10: the dropped tail never entered. */
    CHECK(audio_ring_read(&ring, out, 8) == 8 && frames_match(out, 3, 8));
    CHECK(audio_ring_short_events(&ring) == 0);
}

static void test_fill_and_reset(void)
{
    float storage[4 * 2], in[4 * 2], out[8 * 2];
    audio_ring ring;
    CHECK(audio_ring_init(&ring, storage, 4));
    make_frames(in, 1, 4);

    for (uint32_t i = 1; i <= 4; ++i) {
        CHECK(audio_ring_write(&ring, in, 1) == 1);
        CHECK(audio_ring_fill(&ring) == i);
    }
    CHECK(audio_ring_read(&ring, out, 3) == 3);
    CHECK(audio_ring_fill(&ring) == 1);

    CHECK(audio_ring_write(&ring, in, 4) == 3);
    CHECK(audio_ring_read(&ring, out, 8) == 4);             /* short by 4 */
    const uint64_t dropped = audio_ring_dropped_frames(&ring);
    const uint64_t ev = audio_ring_short_events(&ring), fr = audio_ring_short_frames(&ring);
    CHECK(dropped == 1 && ev == 1 && fr == 4);

    make_frames(in, 100, 4);
    CHECK(audio_ring_write(&ring, in, 4) == 4);
    audio_ring_reset(&ring);
    CHECK(audio_ring_fill(&ring) == 0);
    CHECK(audio_ring_dropped_frames(&ring) == dropped);     /* the run's totals survive a reset */
    CHECK(audio_ring_short_events(&ring) == ev && audio_ring_short_frames(&ring) == fr);

    make_frames(in, 200, 3);                                /* and the ring is usable, with no stale frame */
    CHECK(audio_ring_write(&ring, in, 3) == 3);
    CHECK(audio_ring_read(&ring, out, 3) == 3 && frames_match(out, 200, 3));
}

/* The free-running indices wrap at 2^32, not at the capacity. Start both a few frames short of it. */
static void test_index_wrap(void)
{
    float storage[8 * 2], in[8 * 2], out[8 * 2];
    audio_ring ring;
    CHECK(audio_ring_init(&ring, storage, 8));
    ring.head = ring.tail = 0xFFFFFFFCu;

    uint32_t next_w = 1, next_r = 1;
    for (int i = 0; i < 6; ++i) {
        make_frames(in, next_w, 5);
        CHECK(audio_ring_write(&ring, in, 5) == 5);
        next_w += 5;
        CHECK(audio_ring_fill(&ring) == 5);
        CHECK(audio_ring_read(&ring, out, 5) == 5 && frames_match(out, next_r, 5));
        next_r += 5;
    }
    CHECK(ring.head == 0xFFFFFFFCu + 30u);                   /* passed 2^32 */
    make_frames(in, next_w, 8);
    CHECK(audio_ring_write(&ring, in, 8) == 8);
    CHECK(audio_ring_write(&ring, in, 1) == 0);              /* full is still full */
    CHECK(audio_ring_fill(&ring) == 8);
}

/* ------------------------------------------------------------------------ */

#define STRESS_CAP 256u
#define STRESS_FRAMES 1000000u   /* < 2^24: every counter is an exactly representable float */

typedef struct stress {
    audio_ring ring;
    float storage[STRESS_CAP * 2];
    uint32_t consumer_chunk;     /* 0: the consumer waits until the whole chunk is there */
    uint64_t requested;          /* consumer: frames asked for over the run */
    uint64_t delivered;          /* consumer: real frames it received */
    int bursts;                  /* producer: write in bursts with idle gaps */
} stress;

static void* stress_producer(void* arg)
{
    stress* s = arg;
    float chunk[97 * 2];
    uint32_t next = 1, sent = 0, size = 1;
    while (sent < STRESS_FRAMES) {
        uint32_t want = size;
        if (want > STRESS_FRAMES - sent)
            want = STRESS_FRAMES - sent;
        /* Never over-write: a drop would be a legitimate loss, and this run asserts there is none. */
        const uint32_t room = STRESS_CAP - audio_ring_fill(&s->ring);
        if (room < want) {
            sched_yield();
            continue;
        }
        make_frames(chunk, next, want);
        CHECK(audio_ring_write(&s->ring, chunk, want) == want);
        next += want;
        sent += want;
        size = size % 97 + 1;                                /* 1..97, never the capacity's divisor */
        if (s->bursts && (sent & 0x3FF) < 97)
            sched_yield();
    }
    return NULL;
}

static void* stress_consumer(void* arg)
{
    stress* s = arg;
    float out[61 * 2 + 8];
    uint32_t expect = 1, size = 1;
    uint64_t seen = 0;
    const uint32_t chunk = s->consumer_chunk;
    while (seen < STRESS_FRAMES) {
        uint32_t ask = chunk ? chunk : size;
        if (!chunk) {
            /* Never outrun the producer: ask for what is there, so a short read is a bug. */
            uint32_t have = audio_ring_fill(&s->ring);
            if (have == 0) {
                sched_yield();
                continue;
            }
            if (ask > have)
                ask = have;
            size = size % 61 + 1;
        }
        const uint32_t got = audio_ring_read(&s->ring, out, ask);
        s->requested += ask;
        /* The real frames are the next ones in order, whatever the gaps between reads. */
        CHECK(got <= ask);
        CHECK(frames_match(out, expect, got));
        for (uint32_t i = got * 2; i < ask * 2; ++i)         /* the rest is silence, not stale frames */
            CHECK(out[i] == 0.0f);
        expect += got;
        seen += got;
        s->delivered += got;
        if (got < ask)
            sched_yield();
    }
    return NULL;
}

static void run_stress(stress* s)
{
    CHECK(audio_ring_init(&s->ring, s->storage, STRESS_CAP));
    pthread_t p, c;
    CHECK(pthread_create(&p, NULL, stress_producer, s) == 0);
    CHECK(pthread_create(&c, NULL, stress_consumer, s) == 0);
    CHECK(pthread_join(p, NULL) == 0);
    CHECK(pthread_join(c, NULL) == 0);
    CHECK(s->delivered == STRESS_FRAMES);                    /* no loss, no duplicate */
    CHECK(audio_ring_fill(&s->ring) == 0);
    CHECK(audio_ring_dropped_frames(&s->ring) == 0);
    /* Every frame asked for was delivered or counted as silence. */
    CHECK(s->requested == s->delivered + audio_ring_short_frames(&s->ring));
}

static void test_stress(void)
{
    static stress s;

    /* A consumer that never outruns: strict continuity and not one short read. */
    memset(&s, 0, sizeof s);
    run_stress(&s);
    CHECK(audio_ring_short_events(&s.ring) == 0 && audio_ring_short_frames(&s.ring) == 0);

    /* A fixed-size consumer on a bursty producer, as a device pulls: it does underrun, and
     * the silence is accounted for exactly. */
    memset(&s, 0, sizeof s);
    s.consumer_chunk = 48;
    s.bursts = 1;
    run_stress(&s);
    CHECK(audio_ring_short_frames(&s.ring) >= audio_ring_short_events(&s.ring));
}

int main(void)
{
    test_init();
    test_wrap_order();
    test_short_read();
    test_full_write();
    test_fill_and_reset();
    test_index_wrap();
    test_stress();
    printf("test_audio_ring_spsc: OK\n");
    return 0;
}
