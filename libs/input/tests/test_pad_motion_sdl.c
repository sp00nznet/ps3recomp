/*
 * Standalone test for pad_motion_sdl.h. SDL headers only; every SDL call is
 * routed to fakes, so no SDL initialization, library, hardware or GPU is used.
 *
 *   clang -std=c11 -Wall -Wextra -Werror -I<SDL include root> test_pad_motion_sdl.c -lm
 */
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

enum { FAKE_ACCEL = 0, FAKE_GYRO = 1 };

/* Opaque SDL_GameController* points at this; SDL never dereferences it. */
typedef struct FakePad {
    int attached;
    int has[2];
    int on[2];
    int set_fail[2][2]; /* [sensor][enabled] */
    int is_hide[2];     /* IsSensorEnabled reports false even when on */
    int get_rc[2];
    Uint64 ts[2];
    float data[2][3];
    int n_attached, n_has, n_set[2][2], n_is[2], n_get[2], bad_args;
} FakePad;

static int fake_index(SDL_SensorType type)
{
    return type == SDL_SENSOR_ACCEL ? FAKE_ACCEL : type == SDL_SENSOR_GYRO ? FAKE_GYRO : -1;
}

static FakePad* fake_of(SDL_GameController* gc) { return (FakePad*)(void*)gc; }
static SDL_GameController* gc_of(FakePad* p) { return (SDL_GameController*)(void*)p; }

static SDL_bool fake_attached(SDL_GameController* gc)
{
    FakePad* p = fake_of(gc);
    if (!p)
        return SDL_FALSE;
    p->n_attached++;
    return p->attached ? SDL_TRUE : SDL_FALSE;
}

static SDL_bool fake_has_sensor(SDL_GameController* gc, SDL_SensorType type)
{
    FakePad* p = fake_of(gc);
    int i = fake_index(type);
    if (!p)
        return SDL_FALSE;
    p->n_has++;
    if (i < 0) {
        p->bad_args++;
        return SDL_FALSE;
    }
    return p->has[i] ? SDL_TRUE : SDL_FALSE;
}

static int fake_set_enabled(SDL_GameController* gc, SDL_SensorType type, SDL_bool enabled)
{
    FakePad* p = fake_of(gc);
    int i = fake_index(type), e = enabled ? 1 : 0;
    if (!p)
        return -1;
    if (i < 0 || (enabled != SDL_TRUE && enabled != SDL_FALSE)) {
        p->bad_args++;
        return -1;
    }
    p->n_set[i][e]++;
    if (p->set_fail[i][e])
        return -1;
    p->on[i] = e;
    return 0;
}

static SDL_bool fake_is_enabled(SDL_GameController* gc, SDL_SensorType type)
{
    FakePad* p = fake_of(gc);
    int i = fake_index(type);
    if (!p)
        return SDL_FALSE;
    if (i < 0) {
        p->bad_args++;
        return SDL_FALSE;
    }
    p->n_is[i]++;
    return (p->on[i] && !p->is_hide[i]) ? SDL_TRUE : SDL_FALSE;
}

static int fake_get_data(SDL_GameController* gc, SDL_SensorType type, Uint64* timestamp, float* data, int num_values)
{
    FakePad* p = fake_of(gc);
    int i = fake_index(type), k;
    if (!p)
        return -1;
    if (i < 0 || !timestamp || !data || num_values != 3) {
        p->bad_args++;
        return -1;
    }
    p->n_get[i]++;
    /* Plausible values are written even on failure; the adapter must not publish them. */
    *timestamp = p->ts[i];
    for (k = 0; k < 3; ++k)
        data[k] = p->data[i][k];
    return p->get_rc[i];
}

#define PAD_MOTION_ATTACHED fake_attached
#define PAD_MOTION_HAS_SENSOR fake_has_sensor
#define PAD_MOTION_SET_ENABLED fake_set_enabled
#define PAD_MOTION_IS_ENABLED fake_is_enabled
#define PAD_MOTION_GET_DATA fake_get_data
static Uint64 fake_now_ms;
static Uint64 fake_clock(void) { return fake_now_ms; }
#define PAD_MOTION_NOW_MS fake_clock
#include "../pad_motion_sdl.h"

#define G 9.80665f
#define HALF_PI 1.57079632679489661923f

static int g_checks, g_failures;

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_failures++;                                                        \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

static void fake_init(FakePad* p)
{
    memset(p, 0, sizeof *p);
    p->attached = 1;
    p->has[FAKE_ACCEL] = p->has[FAKE_GYRO] = 1;
    p->ts[FAKE_ACCEL] = p->ts[FAKE_GYRO] = 1000;
}

static void set_data(FakePad* p, float ax, float ay, float az, float gx, float gy, float gz)
{
    p->data[FAKE_ACCEL][0] = ax;
    p->data[FAKE_ACCEL][1] = ay;
    p->data[FAKE_ACCEL][2] = az;
    p->data[FAKE_GYRO][0] = gx;
    p->data[FAKE_GYRO][1] = gy;
    p->data[FAKE_GYRO][2] = gz;
}

static int set_calls(const FakePad* p)
{
    return p->n_set[0][0] + p->n_set[0][1] + p->n_set[1][0] + p->n_set[1][1];
}

static int get_calls(const FakePad* p) { return p->n_get[0] + p->n_get[1]; }

static int all_calls(const FakePad* p)
{
    return p->n_attached + p->n_has + set_calls(p) + p->n_is[0] + p->n_is[1] + get_calls(p);
}

static int state_clear(const PadMotionSDL* s)
{
    return !s->valid && s->timestamp == 0 && s->sample[0] == 0 && s->sample[1] == 0 &&
           s->sample[2] == 0 && s->sample[3] == 0;
}

static int sample_is(const PadMotionSDL* s, int a, int b, int c, int d)
{
    return s->sample[0] == a && s->sample[1] == b && s->sample[2] == c && s->sample[3] == d;
}

static int same_state(const PadMotionSDL* a, const PadMotionSDL* b)
{
    return a->enabled == b->enabled && a->valid == b->valid && a->timestamp == b->timestamp &&
           memcmp(a->sample, b->sample, sizeof a->sample) == 0;
}

/* Fresh fake + enabled state with one valid poll. */
static void setup_valid(FakePad* p, PadMotionSDL* s)
{
    fake_init(p);
    memset(s, 0, sizeof *s);
    set_data(p, 0.0f, G, 0.0f, 0.0f, 0.0f, 0.0f);
    CHECK(pad_motion_sdl_set_enabled(s, gc_of(p), 1) == 1);
    CHECK(pad_motion_sdl_poll(s, gc_of(p)) == 1);
    CHECK(s->valid == 1);
}

static void test_capable(void)
{
    FakePad p;
    CHECK(pad_motion_sdl_capable(NULL) == 0);
    fake_init(&p);
    CHECK(pad_motion_sdl_capable(gc_of(&p)) == 1);
    CHECK(set_calls(&p) == 0 && get_calls(&p) == 0);
    fake_init(&p);
    p.attached = 0;
    CHECK(pad_motion_sdl_capable(gc_of(&p)) == 0);
    CHECK(p.n_has == 0);
    fake_init(&p);
    p.has[FAKE_ACCEL] = 0;
    CHECK(pad_motion_sdl_capable(gc_of(&p)) == 0);
    fake_init(&p);
    p.has[FAKE_GYRO] = 0;
    CHECK(pad_motion_sdl_capable(gc_of(&p)) == 0);
    CHECK(p.bad_args == 0);
}

static void test_invalid_args(void)
{
    FakePad p;
    PadMotionSDL s, before;
    int calls;

    fake_init(&p);
    CHECK(pad_motion_sdl_set_enabled(NULL, gc_of(&p), 1) == 0);
    CHECK(pad_motion_sdl_set_enabled(NULL, gc_of(&p), 0) == 0);
    CHECK(pad_motion_sdl_poll(NULL, gc_of(&p)) == 0);
    CHECK(all_calls(&p) == 0);

    setup_valid(&p, &s);
    before = s;
    calls = all_calls(&p);
    CHECK(pad_motion_sdl_set_enabled(&s, gc_of(&p), 2) == 0);
    CHECK(pad_motion_sdl_set_enabled(&s, gc_of(&p), -1) == 0);
    CHECK(same_state(&s, &before));
    CHECK(all_calls(&p) == calls);
    CHECK(p.on[FAKE_ACCEL] == 1 && p.on[FAKE_GYRO] == 1);
}

static void test_disabled_poll(void)
{
    FakePad p;
    PadMotionSDL s;
    fake_init(&p);
    memset(&s, 0, sizeof s);
    CHECK(pad_motion_sdl_poll(&s, gc_of(&p)) == 0);
    CHECK(state_clear(&s) && s.enabled == 0);
    CHECK(all_calls(&p) == 0);
}

static void test_enable_and_conversion(void)
{
    FakePad p;
    PadMotionSDL s;
    fake_init(&p);
    memset(&s, 0, sizeof s);

    CHECK(pad_motion_sdl_set_enabled(&s, gc_of(&p), 1) == 1);
    CHECK(s.enabled == 1 && state_clear(&s));
    CHECK(p.on[FAKE_ACCEL] == 1 && p.on[FAKE_GYRO] == 1);
    CHECK(p.n_set[FAKE_ACCEL][1] == 1 && p.n_set[FAKE_GYRO][1] == 1);
    CHECK(get_calls(&p) == 0);

    /* Zero real sampled axes. */
    CHECK(pad_motion_sdl_poll(&s, gc_of(&p)) == 1);
    CHECK(s.valid == 1 && sample_is(&s, 512, 512, 512, 512));
    CHECK(s.timestamp == 1000);

    set_data(&p, 0.0f, G, 0.0f, 0.0f, 0.0f, 0.0f);
    CHECK(pad_motion_sdl_poll(&s, gc_of(&p)) == 1);
    CHECK(sample_is(&s, 512, 399, 512, 512));

    set_data(&p, -G, 0.0f, G, 0.0f, 0.0f, 0.0f);
    CHECK(pad_motion_sdl_poll(&s, gc_of(&p)) == 1);
    CHECK(sample_is(&s, 625, 512, 399, 512));

    /* Only gyro Y feeds sample[3]. */
    set_data(&p, 0.0f, 0.0f, 0.0f, 5.0f, HALF_PI, -5.0f);
    CHECK(pad_motion_sdl_poll(&s, gc_of(&p)) == 1);
    CHECK(sample_is(&s, 512, 512, 512, 635));

    set_data(&p, 0.0f, 0.0f, 0.0f, 0.0f, -HALF_PI, 0.0f);
    CHECK(pad_motion_sdl_poll(&s, gc_of(&p)) == 1);
    CHECK(sample_is(&s, 512, 512, 512, 389));

    /* Truncation toward zero: 500.48, 523.52, 512, 433.70. */
    set_data(&p, 1.0f, -1.0f, 0.0f, 0.0f, -1.0f, 0.0f);
    CHECK(pad_motion_sdl_poll(&s, gc_of(&p)) == 1);
    CHECK(sample_is(&s, 500, 523, 512, 433));
    set_data(&p, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
    CHECK(pad_motion_sdl_poll(&s, gc_of(&p)) == 1);
    CHECK(s.sample[3] == 590);
    CHECK(p.bad_args == 0);
}

static void test_saturation(void)
{
    FakePad p;
    PadMotionSDL s;
    setup_valid(&p, &s);

    set_data(&p, FLT_MAX, -FLT_MAX, 1.0e30f, -FLT_MAX, FLT_MAX, FLT_MAX);
    CHECK(pad_motion_sdl_poll(&s, gc_of(&p)) == 1);
    CHECK(s.valid == 1 && sample_is(&s, 0, 1023, 0, 1023));

    set_data(&p, -50.0f, 50.0f, -1.0e30f, 0.0f, -FLT_MAX, 0.0f);
    CHECK(pad_motion_sdl_poll(&s, gc_of(&p)) == 1);
    CHECK(sample_is(&s, 1023, 0, 1023, 0));
}

static void test_non_finite(void)
{
    const float bad[3] = { NAN, INFINITY, -INFINITY };
    int sensor, axis, b;

    for (sensor = 0; sensor < 2; ++sensor)
        for (axis = 0; axis < 3; ++axis)
            for (b = 0; b < 3; ++b) {
                FakePad p;
                PadMotionSDL s;
                setup_valid(&p, &s);
                float prior = p.data[sensor][axis];
                p.data[sensor][axis] = bad[b];
                CHECK(pad_motion_sdl_poll(&s, gc_of(&p)) == 0);
                CHECK(state_clear(&s) && s.enabled == 1);
                CHECK(p.on[FAKE_ACCEL] == 1 && p.on[FAKE_GYRO] == 1);
                /* Recovery on next valid read. */
                p.data[sensor][axis] = prior;
                CHECK(pad_motion_sdl_poll(&s, gc_of(&p)) == 1);
                CHECK(s.valid == 1 && sample_is(&s, 512, 399, 512, 512));
            }
}

static void test_read_errors(void)
{
    FakePad p;
    PadMotionSDL s;
    int gyro_reads;

    setup_valid(&p, &s);
    p.get_rc[FAKE_ACCEL] = -1;
    gyro_reads = p.n_get[FAKE_GYRO];
    CHECK(pad_motion_sdl_poll(&s, gc_of(&p)) == 0);
    CHECK(state_clear(&s) && s.enabled == 1);
    CHECK(p.n_get[FAKE_GYRO] == gyro_reads);
    p.get_rc[FAKE_ACCEL] = 0;
    CHECK(pad_motion_sdl_poll(&s, gc_of(&p)) == 1);
    CHECK(s.valid == 1);

    p.get_rc[FAKE_GYRO] = -1;
    CHECK(pad_motion_sdl_poll(&s, gc_of(&p)) == 0);
    CHECK(state_clear(&s) && s.enabled == 1);
    p.get_rc[FAKE_GYRO] = 0;
    CHECK(pad_motion_sdl_poll(&s, gc_of(&p)) == 1);
    CHECK(s.valid == 1 && sample_is(&s, 512, 399, 512, 512));
}

static void test_timestamps(void)
{
    FakePad p;
    PadMotionSDL s;
    int reads;

    setup_valid(&p, &s);
    p.ts[FAKE_ACCEL] = 0;
    CHECK(pad_motion_sdl_poll(&s, gc_of(&p)) == 1);
    CHECK(s.valid && s.enabled && sample_is(&s,512,399,512,512));
    p.ts[FAKE_ACCEL] = 300;
    p.ts[FAKE_GYRO] = 0;
    CHECK(pad_motion_sdl_poll(&s, gc_of(&p)) == 1);
    CHECK(s.valid && s.enabled && s.timestamp==300);
    p.ts[FAKE_ACCEL] = 0;
    CHECK(pad_motion_sdl_poll(&s,gc_of(&p))==1);
    CHECK(s.valid && s.timestamp==0 && sample_is(&s,512,399,512,512));

    p.ts[FAKE_ACCEL] = 300;
    p.ts[FAKE_GYRO] = 200;
    CHECK(pad_motion_sdl_poll(&s, gc_of(&p)) == 1);
    CHECK(s.timestamp == 300);
    p.ts[FAKE_ACCEL] = 400;
    p.ts[FAKE_GYRO] = UINT64_C(0xFFFFFFFFFFFFFFF0);
    CHECK(pad_motion_sdl_poll(&s, gc_of(&p)) == 1);
    CHECK(s.timestamp == UINT64_C(0xFFFFFFFFFFFFFFF0));

    /* Duplicate nonzero timestamps are cached real samples. */
    p.ts[FAKE_ACCEL] = p.ts[FAKE_GYRO] = 777;
    reads = get_calls(&p);
    CHECK(pad_motion_sdl_poll(&s, gc_of(&p)) == 1);
    CHECK(pad_motion_sdl_poll(&s, gc_of(&p)) == 1);
    CHECK(s.valid == 1 && s.timestamp == 777 && sample_is(&s, 512, 399, 512, 512));
    CHECK(get_calls(&p) == reads + 4);
}

static void test_disconnect_and_capability_loss(void)
{
    FakePad p;
    PadMotionSDL s;
    int calls;

    setup_valid(&p, &s);
    p.attached = 0;
    calls = get_calls(&p);
    CHECK(pad_motion_sdl_poll(&s, gc_of(&p)) == 0);
    CHECK(state_clear(&s) && s.enabled == 0);
    CHECK(get_calls(&p) == calls);
    p.attached = 1;
    calls = all_calls(&p);
    CHECK(pad_motion_sdl_poll(&s, gc_of(&p)) == 0);
    CHECK(all_calls(&p) == calls);

    setup_valid(&p, &s);
    p.has[FAKE_GYRO] = 0;
    CHECK(pad_motion_sdl_poll(&s, gc_of(&p)) == 0);
    CHECK(state_clear(&s) && s.enabled == 0);

    setup_valid(&p, &s);
    CHECK(pad_motion_sdl_poll(&s, NULL) == 0);
    CHECK(state_clear(&s) && s.enabled == 0);

    /* Enable on non-capable controllers makes no SDL sensor calls. */
    fake_init(&p);
    memset(&s, 0, sizeof s);
    p.has[FAKE_ACCEL] = 0;
    CHECK(pad_motion_sdl_set_enabled(&s, gc_of(&p), 1) == 0);
    CHECK(pad_motion_sdl_set_enabled(&s, NULL, 1) == 0);
    CHECK(state_clear(&s) && s.enabled == 0);
    CHECK(set_calls(&p) == 0);
}

static void test_enable_lost(void)
{
    FakePad p;
    PadMotionSDL s;
    int reads;

    setup_valid(&p, &s);
    p.on[FAKE_GYRO] = 0;
    reads = get_calls(&p);
    CHECK(pad_motion_sdl_poll(&s, gc_of(&p)) == 0);
    CHECK(state_clear(&s) && s.enabled == 1);
    CHECK(get_calls(&p) == reads);
    CHECK(pad_motion_sdl_set_enabled(&s, gc_of(&p), 1) == 1);
    CHECK(p.on[FAKE_GYRO] == 1);
    CHECK(pad_motion_sdl_poll(&s, gc_of(&p)) == 1);
    CHECK(s.valid == 1);

    /* Re-enable failure after loss clears everything. */
    p.on[FAKE_GYRO] = 0;
    p.set_fail[FAKE_GYRO][1] = 1;
    CHECK(pad_motion_sdl_set_enabled(&s, gc_of(&p), 1) == 0);
    CHECK(state_clear(&s) && s.enabled == 0);
    CHECK(p.on[FAKE_ACCEL] == 0 && p.on[FAKE_GYRO] == 0);
}

static void test_already_enabled(void)
{
    FakePad p;
    PadMotionSDL s, before;
    int sets;

    setup_valid(&p, &s);
    before = s;
    sets = set_calls(&p);
    CHECK(pad_motion_sdl_set_enabled(&s, gc_of(&p), 1) == 1);
    CHECK(same_state(&s, &before));
    CHECK(set_calls(&p) == sets);

    p.attached = 0;
    CHECK(pad_motion_sdl_set_enabled(&s, gc_of(&p), 1) == 0);
    CHECK(state_clear(&s) && s.enabled == 0);
    CHECK(set_calls(&p) == sets);
}

static void test_partial_enable_rollback(void)
{
    FakePad p;
    PadMotionSDL s;

    fake_init(&p);
    memset(&s, 0, sizeof s);
    p.set_fail[FAKE_GYRO][1] = 1;
    CHECK(pad_motion_sdl_set_enabled(&s, gc_of(&p), 1) == 0);
    CHECK(state_clear(&s) && s.enabled == 0);
    CHECK(p.on[FAKE_ACCEL] == 0 && p.on[FAKE_GYRO] == 0);
    CHECK(p.n_set[FAKE_ACCEL][1] == 1 && p.n_set[FAKE_ACCEL][0] == 1);
    CHECK(p.n_set[FAKE_GYRO][1] == 1 && p.n_set[FAKE_GYRO][0] == 0);
    CHECK(pad_motion_sdl_poll(&s, gc_of(&p)) == 0);
    CHECK(get_calls(&p) == 0);

    fake_init(&p);
    p.set_fail[FAKE_ACCEL][1] = 1;
    CHECK(pad_motion_sdl_set_enabled(&s, gc_of(&p), 1) == 0);
    CHECK(p.n_set[FAKE_GYRO][1] == 0 && p.n_set[FAKE_ACCEL][0] == 0 && p.n_set[FAKE_GYRO][0] == 0);
    CHECK(p.on[FAKE_ACCEL] == 0 && p.on[FAKE_GYRO] == 0);
    CHECK(state_clear(&s) && s.enabled == 0);

    /* Failed enable over previously valid data clears it. */
    setup_valid(&p, &s);
    p.on[FAKE_ACCEL] = 0;
    p.set_fail[FAKE_ACCEL][1] = 1;
    CHECK(pad_motion_sdl_set_enabled(&s, gc_of(&p), 1) == 0);
    CHECK(state_clear(&s) && s.enabled == 0);
}

static void test_is_enabled_failure(void)
{
    FakePad p;
    PadMotionSDL s;

    fake_init(&p);
    memset(&s, 0, sizeof s);
    p.is_hide[FAKE_GYRO] = 1;
    CHECK(pad_motion_sdl_set_enabled(&s, gc_of(&p), 1) == 0);
    CHECK(state_clear(&s) && s.enabled == 0);
    CHECK(p.on[FAKE_ACCEL] == 0 && p.on[FAKE_GYRO] == 0);
    CHECK(p.n_set[FAKE_ACCEL][0] == 1 && p.n_set[FAKE_GYRO][0] == 1);

    p.is_hide[FAKE_GYRO] = 0;
    CHECK(pad_motion_sdl_set_enabled(&s, gc_of(&p), 1) == 1);
    CHECK(s.enabled == 1 && state_clear(&s));
    CHECK(pad_motion_sdl_poll(&s, gc_of(&p)) == 1);
}

static void test_disable(void)
{
    FakePad p;
    PadMotionSDL s;
    int calls;

    setup_valid(&p, &s);
    CHECK(pad_motion_sdl_set_enabled(&s, gc_of(&p), 0) == 1);
    CHECK(state_clear(&s) && s.enabled == 0);
    CHECK(p.on[FAKE_ACCEL] == 0 && p.on[FAKE_GYRO] == 0);
    calls = all_calls(&p);
    CHECK(pad_motion_sdl_poll(&s, gc_of(&p)) == 0);
    CHECK(all_calls(&p) == calls);

    setup_valid(&p, &s);
    p.set_fail[FAKE_GYRO][0] = 1;
    CHECK(pad_motion_sdl_set_enabled(&s, gc_of(&p), 0) == 0);
    CHECK(state_clear(&s) && s.enabled == 0);
    CHECK(p.on[FAKE_ACCEL] == 0 && p.on[FAKE_GYRO] == 1);
    CHECK(p.n_set[FAKE_ACCEL][0] == 1 && p.n_set[FAKE_GYRO][0] == 1);
    calls = get_calls(&p);
    CHECK(pad_motion_sdl_poll(&s, gc_of(&p)) == 0);
    CHECK(get_calls(&p) == calls && state_clear(&s));

    setup_valid(&p, &s);
    p.set_fail[FAKE_ACCEL][0] = 1;
    CHECK(pad_motion_sdl_set_enabled(&s, gc_of(&p), 0) == 0);
    CHECK(state_clear(&s) && s.enabled == 0);
    CHECK(p.n_set[FAKE_GYRO][0] == 1 && p.on[FAKE_GYRO] == 0);

    setup_valid(&p, &s);
    CHECK(pad_motion_sdl_set_enabled(&s, NULL, 0) == 1);
    CHECK(state_clear(&s) && s.enabled == 0);

    setup_valid(&p, &s);
    p.attached = 0;
    calls = set_calls(&p);
    CHECK(pad_motion_sdl_set_enabled(&s, gc_of(&p), 0) == 1);
    CHECK(state_clear(&s) && s.enabled == 0);
    CHECK(set_calls(&p) == calls);
}

static void test_two_adapters(void)
{
    FakePad pa, pb;
    PadMotionSDL sa, sb;

    fake_init(&pa);
    fake_init(&pb);
    memset(&sa, 0, sizeof sa);
    memset(&sb, 0, sizeof sb);
    set_data(&pa, 0.0f, G, 0.0f, 0.0f, 0.0f, 0.0f);
    set_data(&pb, 0.0f, -G, 0.0f, 0.0f, HALF_PI, 0.0f);
    pa.ts[FAKE_ACCEL] = pa.ts[FAKE_GYRO] = 10;
    pb.ts[FAKE_ACCEL] = pb.ts[FAKE_GYRO] = 20;

    CHECK(pad_motion_sdl_set_enabled(&sa, gc_of(&pa), 1) == 1);
    CHECK(pad_motion_sdl_set_enabled(&sb, gc_of(&pb), 1) == 1);
    CHECK(pad_motion_sdl_poll(&sa, gc_of(&pa)) == 1);
    CHECK(pad_motion_sdl_poll(&sb, gc_of(&pb)) == 1);
    CHECK(sample_is(&sa, 512, 399, 512, 512) && sa.timestamp == 10);
    CHECK(sample_is(&sb, 512, 625, 512, 635) && sb.timestamp == 20);

    pb.get_rc[FAKE_GYRO] = -1;
    CHECK(pad_motion_sdl_poll(&sb, gc_of(&pb)) == 0);
    CHECK(state_clear(&sb) && sb.enabled == 1);
    CHECK(pad_motion_sdl_poll(&sa, gc_of(&pa)) == 1);
    CHECK(sa.valid == 1 && sample_is(&sa, 512, 399, 512, 512));

    CHECK(pad_motion_sdl_set_enabled(&sa, gc_of(&pa), 0) == 1);
    CHECK(pa.on[FAKE_ACCEL] == 0 && pa.on[FAKE_GYRO] == 0);
    CHECK(pb.on[FAKE_ACCEL] == 1 && pb.on[FAKE_GYRO] == 1);
    CHECK(sb.enabled == 1);
    pb.get_rc[FAKE_GYRO] = 0;
    CHECK(pad_motion_sdl_poll(&sb, gc_of(&pb)) == 1);
    CHECK(sample_is(&sb, 512, 625, 512, 635));
    CHECK(state_clear(&sa) && sa.enabled == 0);
    CHECK(pa.bad_args == 0 && pb.bad_args == 0);
}

static void test_freshness(void)
{
    FakePad p;
    PadMotionSDL s;
    fake_now_ms=100;
    setup_valid(&p,&s);
    fake_now_ms=350;
    CHECK(pad_motion_sdl_poll(&s,gc_of(&p))==1);
    fake_now_ms=351;
    CHECK(pad_motion_sdl_poll(&s,gc_of(&p))==0 && state_clear(&s));
    p.ts[FAKE_GYRO]++;
    CHECK(pad_motion_sdl_poll(&s,gc_of(&p))==0 && state_clear(&s));
    p.ts[FAKE_ACCEL]++;
    CHECK(pad_motion_sdl_poll(&s,gc_of(&p))==1);
    fake_now_ms=602;
    p.ts[FAKE_ACCEL]++;
    CHECK(pad_motion_sdl_poll(&s,gc_of(&p))==0 && state_clear(&s));
    p.ts[FAKE_GYRO]++;
    CHECK(pad_motion_sdl_poll(&s,gc_of(&p))==1);
    /* Missing metadata is allowed even during long stationary intervals. */
    p.ts[FAKE_ACCEL]=p.ts[FAKE_GYRO]=0;
    fake_now_ms=10000;
    CHECK(pad_motion_sdl_poll(&s,gc_of(&p))==1 && s.timestamp==0);
    fake_now_ms=20000;
    CHECK(pad_motion_sdl_poll(&s,gc_of(&p))==1);
    p.ts[FAKE_ACCEL]=p.ts[FAKE_GYRO]=10;
    CHECK(pad_motion_sdl_poll(&s,gc_of(&p))==1);
    CHECK(pad_motion_sdl_set_enabled(&s,gc_of(&p),0)==1);
    fake_now_ms=30000;
    CHECK(pad_motion_sdl_set_enabled(&s,gc_of(&p),1)==1);
    CHECK(pad_motion_sdl_poll(&s,gc_of(&p))==1);
}

int main(void)
{
    test_capable();
    test_invalid_args();
    test_disabled_poll();
    test_enable_and_conversion();
    test_saturation();
    test_non_finite();
    test_read_errors();
    test_timestamps();
    test_disconnect_and_capability_loss();
    test_enable_lost();
    test_already_enabled();
    test_partial_enable_rollback();
    test_is_enabled_failure();
    test_disable();
    test_two_adapters();
    test_freshness();
    printf("pad_motion_sdl: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
