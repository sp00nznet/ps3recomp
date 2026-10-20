#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "g7_motion_backend.h"

/* CPU-only: install the complete mock before every start. Never enumerate HID.
 * The owner serializes lifecycle calls; a separate thread polls samples only.
 * All synchronization waits have a deadline, including deliberately stuck I/O. */
int g7_motion_service(void);
enum { WAIT_MS = 2500, PROMPT_STOP_MS = 300 };
static atomic_int selection, read_error, report_pending, block_read;
static atomic_int entered_read, release_read, writes, closes, bad_io;
static atomic_int block_select, entered_select, release_select;
static atomic_int poll_quit, poll_done, poll_failed, stopped_visible, stop_returned;
static atomic_uint_fast64_t clock_us;
static unsigned char report[64];

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); exit(1); \
} } while (0)

static int wait_value(atomic_int *value, int expected, Uint32 timeout) {
    Uint32 start = SDL_GetTicks();
    do {
        if (atomic_load(value) == expected) return 1;
        SDL_Delay(1);
    } while ((Uint32)(SDL_GetTicks() - start) < timeout);
    return atomic_load(value) == expected;
}

static int mock_select(uint16_t vid, uint16_t pid, char *path, size_t cap) {
    if (vid != 0x3537 || (pid != 0x10c5 && pid != 0x10c6) || cap < 5) {
        atomic_store(&bad_io, 1);
        return -1;
    }
    memcpy(path, "mock", 5);
    if (atomic_load(&block_select)) {
        atomic_store(&entered_select, 1);
        if (!wait_value(&release_select, 1, WAIT_MS)) {
            atomic_store(&bad_io, 1);
            return -1;
        }
    }
    return atomic_load(&selection);
}
static void *mock_open(const char *path) {
    if (strcmp(path, "mock") != 0) atomic_store(&bad_io, 1);
    return &report;
}
static int mock_read(void *device, unsigned char *bytes, size_t cap, int timeout) {
    (void)timeout;
    if (device != &report || cap < sizeof(report)) atomic_store(&bad_io, 1);
    if (atomic_load(&block_read)) {
        atomic_store(&entered_read, 1);
        if (!wait_value(&release_read, 1, WAIT_MS)) {
            atomic_store(&bad_io, 1);
            return -1;
        }
        /* Deliberately return a valid report after stop has been requested. */
        atomic_store(&block_read, 0);
        memcpy(bytes, report, sizeof(report));
        return (int)sizeof(report);
    }
    if (atomic_load(&read_error)) return -1;
    if (atomic_exchange(&report_pending, 0)) {
        memcpy(bytes, report, sizeof(report));
        return (int)sizeof(report);
    }
    SDL_Delay(1);
    return 0;
}
static int mock_write(void *device, const unsigned char *bytes, size_t size) {
    if (device != &report || size != 64 || bytes[0] != 0x0f || bytes[1] != 0xf2)
        atomic_store(&bad_io, 1);
    for (size_t i = 2; i < size; ++i)
        if (bytes[i] != 0) atomic_store(&bad_io, 1);
    atomic_fetch_add(&writes, 1);
    return (int)size;
}
static void mock_close(void *device) {
    if (device != &report) atomic_store(&bad_io, 1);
    atomic_fetch_add(&closes, 1);
}
static uint64_t mock_now(void) { return atomic_load(&clock_us); }
static const g7_motion_io mock_io = {
    mock_select, mock_open, mock_read, mock_write, mock_close, mock_now
};

static void check_empty(g7_motion_state state) {
    g7_motion_sample sample;
    memset(&sample, 0x55, sizeof(sample));
    CHECK(g7_motion_get_raw_sample(&sample) == 0);
    CHECK(sample.state == state && sample.fresh_valid == 0);
    CHECK(sample.monotonic_us == 0);
    for (int i = 0; i < 3; ++i)
        CHECK(sample.gyro[i] == 0 && sample.accel[i] == 0);
}
static int wait_state(g7_motion_state state) {
    Uint32 start = SDL_GetTicks();
    do {
        g7_motion_sample sample;
        (void)g7_motion_get_raw_sample(&sample);
        if (sample.state == state) return 1;
        SDL_Delay(1);
    } while ((Uint32)(SDL_GetTicks() - start) < WAIT_MS);
    return 0;
}
static int wait_sample(g7_motion_sample *sample) {
    Uint32 start = SDL_GetTicks();
    do {
        if (g7_motion_get_raw_sample(sample)) return 1;
        SDL_Delay(1);
    } while ((Uint32)(SDL_GetTicks() - start) < WAIT_MS);
    return 0;
}
static void reap(void) {
    Uint32 start = SDL_GetTicks();
    do {
        if (g7_motion_service()) return;
        SDL_Delay(1);
    } while ((Uint32)(SDL_GetTicks() - start) < WAIT_MS);
    CHECK(0 && "worker did not complete within deadline");
}
static void reset_mock(void) {
    CHECK(g7_motion_service() == 1);
    CHECK(g7_motion_test_install_io(&mock_io) == 1);
    atomic_store(&selection, 1);
    atomic_store(&read_error, 0);
    atomic_store(&report_pending, 0);
    atomic_store(&block_read, 0);
    atomic_store(&entered_read, 0);
    atomic_store(&release_read, 0);
    atomic_store(&block_select, 0);
    atomic_store(&entered_select, 0);
    atomic_store(&release_select, 0);
    atomic_store(&writes, 0);
    atomic_store(&closes, 0);
    atomic_store(&bad_io, 0);
    atomic_store(&clock_us, 1000000);
}
static void stop_reap(void) {
    g7_motion_stop();
    check_empty(G7_MOTION_STOPPED);
    reap();
    CHECK(atomic_load(&bad_io) == 0);
}

static void test_decode(void) {
    const int16_t expected[6] = {0x1234, -2, INT16_MIN, INT16_MAX, -1, 0};
    g7_motion_sample sample;
    memset(report, 0, sizeof(report));
    report[0] = 0x12;
    for (int i = 0; i < 6; ++i) {
        uint16_t value = (uint16_t)expected[i];
        report[13 + 2 * i] = (unsigned char)value;
        report[14 + 2 * i] = (unsigned char)(value >> 8);
    }
    CHECK(g7_motion_test_decode(report, sizeof(report), &sample) == 1);
    for (int i = 0; i < 3; ++i) {
        CHECK(sample.gyro[i] == expected[i]);
        CHECK(sample.accel[i] == expected[i + 3]);
    }
    CHECK(g7_motion_test_decode(NULL, 64, &sample) == 0);
    CHECK(g7_motion_test_decode(report, 64, NULL) == 0);
    CHECK(g7_motion_test_decode(report, 0, &sample) == 0);
    CHECK(g7_motion_test_decode(report, 63, &sample) == 0);
    CHECK(g7_motion_test_decode(report, 65, &sample) == 0);
    report[0] = 0x11;
    CHECK(g7_motion_test_decode(report, 64, &sample) == 0);
    report[0] = 0x12;
}
static void test_ids_and_idle(void) {
    reset_mock();
    CHECK(g7_motion_start(0x0000, 0x10c5) == 0);
    CHECK(g7_motion_start(0x3537, 0x0000) == 0);
    CHECK(g7_motion_service() == 1);
    CHECK(atomic_load(&writes) == 0);
    g7_motion_stop();
    g7_motion_stop();
    check_empty(G7_MOTION_STOPPED);
    CHECK(g7_motion_get_raw_sample(NULL) == 0);
}
static void test_samples_and_stale(void) {
    reset_mock();
    atomic_store(&report_pending, 1);
    CHECK(g7_motion_start(0x3537, 0x10c5) == 1);
    g7_motion_sample sample;
    CHECK(wait_sample(&sample));
    CHECK(sample.state == G7_MOTION_RUNNING && sample.gyro[0] == 0x1234);
    CHECK(sample.monotonic_us == 1000000);
    CHECK(g7_motion_service() == 0);
    CHECK(g7_motion_start(0x3537, 0x10c6) == 0);
    CHECK(g7_motion_test_install_io(&mock_io) == 0);
    atomic_store(&clock_us, 1100000);
    CHECK(g7_motion_get_raw_sample(&sample) == 1);
    atomic_store(&clock_us, 1100001);
    check_empty(G7_MOTION_RUNNING);
    atomic_store(&report_pending, 1);
    CHECK(wait_sample(&sample));
    CHECK(sample.monotonic_us == 1100001);
    CHECK(atomic_load(&writes) >= 1);
    stop_reap();
    CHECK(atomic_load(&closes) == 1);
}
static void test_terminal_retry(void) {
    const int results[] = {0, -2, -1};
    const g7_motion_state states[] = {
        G7_MOTION_NO_DEVICE, G7_MOTION_AMBIGUOUS, G7_MOTION_IO_ERROR
    };
    for (int i = 0; i < 3; ++i) {
        reset_mock();
        atomic_store(&selection, results[i]);
        CHECK(g7_motion_start(0x3537, 0x10c6) == 1);
        CHECK(wait_state(states[i]));
        reap();
        check_empty(states[i]);
        CHECK(g7_motion_service() == 1);
        check_empty(states[i]);
        atomic_store(&selection, 1);
        CHECK(g7_motion_start(0x3537, 0x10c6) == 1);
        CHECK(wait_state(G7_MOTION_RUNNING));
        stop_reap();
    }
    reset_mock();
    atomic_store(&read_error, 1);
    CHECK(g7_motion_start(0x3537, 0x10c5) == 1);
    CHECK(wait_state(G7_MOTION_IO_ERROR));
    reap();
    check_empty(G7_MOTION_IO_ERROR);
    g7_motion_stop();
    g7_motion_stop();
    check_empty(G7_MOTION_STOPPED);
    atomic_store(&read_error, 0);
    CHECK(g7_motion_start(0x3537, 0x10c5) == 1);
    CHECK(wait_state(G7_MOTION_RUNNING));
    stop_reap();
}
static int poll_samples(void *unused) {
    (void)unused;
    while (!atomic_load(&poll_quit)) {
        int stopped = atomic_load(&stopped_visible);
        g7_motion_sample sample;
        int valid = g7_motion_get_raw_sample(&sample);
        if (stopped && (valid || sample.fresh_valid ||
                        sample.state != G7_MOTION_STOPPED))
            atomic_store(&poll_failed, 1);
        SDL_Delay(1);
    }
    atomic_store(&poll_done, 1);
    return 0;
}
static int stop_owner(void *unused) {
    (void)unused;
    g7_motion_stop();
    atomic_store(&stop_returned, 1);
    return 0;
}
static void test_blocked_select_stop(void) {
    reset_mock();
    atomic_store(&block_select, 1);
    atomic_store(&stop_returned, 0);
    CHECK(g7_motion_start(0x3537, 0x10c6) == 1);
    CHECK(wait_value(&entered_select, 1, WAIT_MS));
    SDL_Thread *stopper = SDL_CreateThread(stop_owner, "select-stop-owner", NULL);
    CHECK(stopper != NULL);
    int prompt = wait_value(&stop_returned, 1, PROMPT_STOP_MS);
    if (!prompt) {
        atomic_store(&release_select, 1);
        CHECK(wait_value(&stop_returned, 1, WAIT_MS));
        SDL_WaitThread(stopper, NULL);
        CHECK(prompt);
    }
    SDL_WaitThread(stopper, NULL);
    check_empty(G7_MOTION_STOPPED);
    CHECK(g7_motion_service() == 0);
    CHECK(g7_motion_start(0x3537, 0x10c5) == 0);
    CHECK(g7_motion_test_install_io(&mock_io) == 0);
    atomic_store(&release_select, 1);
    reap();
    check_empty(G7_MOTION_STOPPED);
    CHECK(atomic_load(&writes) == 0);
    CHECK(atomic_load(&bad_io) == 0);
}
static void test_blocked_read_stop(void) {
    reset_mock();
    atomic_store(&poll_quit, 0);
    atomic_store(&poll_done, 0);
    atomic_store(&poll_failed, 0);
    atomic_store(&stopped_visible, 0);
    atomic_store(&stop_returned, 0);
    atomic_store(&block_read, 1);
    CHECK(g7_motion_start(0x3537, 0x10c5) == 1);
    CHECK(wait_value(&entered_read, 1, WAIT_MS));
    SDL_Thread *poller = SDL_CreateThread(poll_samples, "sample-poller", NULL);
    CHECK(poller != NULL);
    SDL_Thread *stopper = SDL_CreateThread(stop_owner, "stop-owner", NULL);
    CHECK(stopper != NULL);
    int prompt = wait_value(&stop_returned, 1, PROMPT_STOP_MS);
    if (!prompt) {
        /* Unstick a faulty implementation before reporting the bounded failure. */
        atomic_store(&release_read, 1);
        CHECK(wait_value(&stop_returned, 1, WAIT_MS));
        SDL_WaitThread(stopper, NULL);
        CHECK(prompt);
    }
    SDL_WaitThread(stopper, NULL); /* confirmed returned before joining */
    atomic_store(&stopped_visible, 1);
    check_empty(G7_MOTION_STOPPED);
    CHECK(atomic_load(&release_read) == 0);
    CHECK(g7_motion_service() == 0);
    CHECK(g7_motion_start(0x3537, 0x10c5) == 0);
    CHECK(g7_motion_test_install_io(&mock_io) == 0);
    g7_motion_stop();
    check_empty(G7_MOTION_STOPPED);
    atomic_store(&release_read, 1);
    reap();
    check_empty(G7_MOTION_STOPPED); /* late report must never be published */
    atomic_store(&poll_quit, 1);
    CHECK(wait_value(&poll_done, 1, WAIT_MS));
    SDL_WaitThread(poller, NULL);
    CHECK(atomic_load(&poll_failed) == 0);
    CHECK(atomic_load(&bad_io) == 0);
    CHECK(atomic_load(&closes) == 1);
    CHECK(g7_motion_start(0x3537, 0x10c6) == 1);
    CHECK(wait_state(G7_MOTION_RUNNING));
    stop_reap();
}
int main(void) {
    SDL_SetMainReady();
    CHECK(SDL_Init(SDL_INIT_TIMER) == 0);
    CHECK(g7_motion_test_install_io(&mock_io) == 1);
    test_decode();
    test_ids_and_idle();
    test_samples_and_stale();
    test_terminal_retry();
    test_blocked_select_stop();
    test_blocked_read_stop();
    CHECK(g7_motion_service() == 1);
    SDL_Quit();
    puts("PASS: CPU-only G7 decode, samples, freshness, terminal retry, prompt stop, late publication, lifecycle service");
    return 0;
}
