#include "g7_motion_backend.h"
#include <string.h>

#if defined(_WIN32) && defined(G7_WITH_SDL_HIDAPI)

#include <SDL2/SDL.h>
#include <SDL2/SDL_hidapi.h>
#include <stdatomic.h>

enum { G7_REPORT_BYTES = 64, G7_DRAIN_LIMIT = 32, G7_READ_TIMEOUT_MS = 10 };
static const uint16_t G7_VID = 0x3537;
static const uint16_t G7_WIRED_PID = 0x10c5;
static const uint16_t G7_DONGLE_PID = 0x10c6;
static const uint16_t G7_USAGE_PAGE = 0xfff0;
static const uint16_t G7_USAGE = 0x0040;
static const uint64_t G7_HEARTBEAT_US = 500000;
static const uint64_t G7_STALE_US = 100000;

/* Published once and retained for concurrent pollers for process lifetime. */
static _Atomic(SDL_mutex *) g_mutex;
static SDL_Thread *g_thread;
static atomic_int g_stop;
static atomic_int g_completed = 1;
static g7_motion_state g_state = G7_MOTION_STOPPED;
static g7_motion_sample g_latest;

static int16_t signed_le16(const unsigned char *p) {
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int decode(const unsigned char *data, size_t size, g7_motion_sample *out) {
    if (!data || !out || size != G7_REPORT_BYTES || data[0] != 0x12) return 0;
    memset(out, 0, sizeof(*out));
    out->gyro[0] = signed_le16(data + 13);
    out->gyro[1] = signed_le16(data + 15);
    out->gyro[2] = signed_le16(data + 17);
    out->accel[0] = signed_le16(data + 19);
    out->accel[1] = signed_le16(data + 21);
    out->accel[2] = signed_le16(data + 23);
    out->fresh_valid = 1;
    return 1;
}

static uint64_t real_now_us(void) {
    const uint64_t frequency = SDL_GetPerformanceFrequency();
    const uint64_t ticks = SDL_GetPerformanceCounter();
    return frequency ? (ticks / frequency) * 1000000u +
           ((ticks % frequency) * 1000000u) / frequency : 0;
}

static int real_select(uint16_t vid, uint16_t pid, char *path, size_t capacity) {
    SDL_hid_device_info *list = SDL_hid_enumerate(vid, pid);
    int matches = 0;
    for (SDL_hid_device_info *d = list; d; d = d->next) {
        if (d->vendor_id != vid || d->product_id != pid ||
            d->usage_page != G7_USAGE_PAGE || d->usage != G7_USAGE || !d->path) continue;
        /* Identical paths in an enumeration are one collection. */
        if (matches && strcmp(path, d->path) == 0) continue;
        size_t length = strlen(d->path);
        if (length >= capacity) { matches = -1; break; }
        if (matches) { matches = -2; break; }
        memcpy(path, d->path, length + 1);
        matches = 1;
    }
    SDL_hid_free_enumeration(list);
    return matches;
}
#include "g7_win32_transport.inc"
static const g7_motion_io g_real_io = {
    real_select, real_open, real_read, real_write, real_close, real_now_us
};
static g7_motion_io g_io;
static uint16_t g_vid, g_pid;

static int ensure_mutex(void) {
    if (atomic_load_explicit(&g_mutex, memory_order_acquire)) return 1;
    SDL_mutex *mutex = SDL_CreateMutex();
    if (!mutex) return 0;
    atomic_store_explicit(&g_mutex, mutex, memory_order_release);
    return 1;
}

static void set_state(g7_motion_state state, int clear) {
    SDL_LockMutex(g_mutex);
    g_state = atomic_load_explicit(&g_stop, memory_order_acquire) ?
              G7_MOTION_STOPPED : state;
    if (clear) memset(&g_latest, 0, sizeof(g_latest));
    SDL_UnlockMutex(g_mutex);
}

static int worker(void *unused) {
    (void)unused;
    char path[1024] = {0};
    void *device = NULL;
    int using_real = g_io.select == real_select;
    int hid_initialized = 0;
    if (atomic_load_explicit(&g_stop, memory_order_acquire)) goto done;
    if (using_real && SDL_hid_init() != 0) {
        set_state(G7_MOTION_IO_ERROR, 1); goto done;
    }
    hid_initialized = using_real;
    int selected = g_io.select(g_vid, g_pid, path, sizeof(path));
    if (atomic_load_explicit(&g_stop, memory_order_acquire)) goto done;
    if (selected != 1) {
        set_state(selected == 0 ? G7_MOTION_NO_DEVICE :
                  selected == -2 ? G7_MOTION_AMBIGUOUS : G7_MOTION_IO_ERROR, 1);
        goto done;
    }
    device = g_io.open(path);
    if (atomic_load_explicit(&g_stop, memory_order_acquire)) goto done;
    if (!device) { set_state(G7_MOTION_IO_ERROR, 1); goto done; }
    set_state(G7_MOTION_RUNNING, 1);
    uint64_t next_heartbeat = 0;
    while (!atomic_load_explicit(&g_stop, memory_order_acquire)) {
        uint64_t now = g_io.now_us();
        if (now >= next_heartbeat) {
            unsigned char heartbeat[G7_REPORT_BYTES] = {0};
            heartbeat[0] = 0x0f;
            heartbeat[1] = 0xf2;
            if (atomic_load_explicit(&g_stop, memory_order_acquire)) break;
            if (g_io.write(device, heartbeat, sizeof(heartbeat)) != G7_REPORT_BYTES) {
                set_state(G7_MOTION_IO_ERROR, 1); break;
            }
            next_heartbeat = now + G7_HEARTBEAT_US;
        }
        for (int i = 0; i < G7_DRAIN_LIMIT &&
             !atomic_load_explicit(&g_stop, memory_order_acquire); ++i) {
            unsigned char bytes[G7_REPORT_BYTES];
            int count = g_io.read_timeout(device, bytes, sizeof(bytes),
                                          i == 0 ? G7_READ_TIMEOUT_MS : 0);
            if (atomic_load_explicit(&g_stop, memory_order_acquire)) goto done;
            if (count < 0) { set_state(G7_MOTION_IO_ERROR, 1); goto done; }
            if (count == 0) break;
            g7_motion_sample sample;
            if (!decode(bytes, (size_t)count, &sample)) continue;
            sample.monotonic_us = g_io.now_us();
            SDL_LockMutex(g_mutex);
            if (!atomic_load_explicit(&g_stop, memory_order_acquire) &&
                g_state == G7_MOTION_RUNNING)
                g_latest = sample;
            SDL_UnlockMutex(g_mutex);
        }
    }
done:
    if (device) g_io.close(device);
    if (hid_initialized) SDL_hid_exit();
    if (atomic_load_explicit(&g_stop, memory_order_acquire))
        set_state(G7_MOTION_STOPPED, 1);
    /* No I/O, mutex use, or state writes may follow this publication. */
    atomic_store_explicit(&g_completed, 1, memory_order_release);
    return 0;
}

int g7_motion_service(void) {
    if (!g_thread) return 1;
    if (!atomic_load_explicit(&g_completed, memory_order_acquire)) return 0;
    SDL_WaitThread(g_thread, NULL);
    g_thread = NULL;
    return 1;
}

int g7_motion_start(uint16_t vendor_id, uint16_t product_id) {
    if (!g7_motion_service()) return 0;
    if (vendor_id != G7_VID ||
        (product_id != G7_WIRED_PID && product_id != G7_DONGLE_PID))
        return 0;
    if (!ensure_mutex()) return 0;
    SDL_LockMutex(g_mutex);
    if (!g_io.select) g_io = g_real_io;
    g_vid = vendor_id; g_pid = product_id;
    atomic_store_explicit(&g_stop, 0, memory_order_release);
    g_state = G7_MOTION_STARTING;
    memset(&g_latest, 0, sizeof(g_latest));
    SDL_UnlockMutex(g_mutex);
    atomic_store_explicit(&g_completed, 0, memory_order_release);
    g_thread = SDL_CreateThread(worker, "g7-motion-hid", NULL);
    if (!g_thread) {
        set_state(G7_MOTION_IO_ERROR, 1);
        atomic_store_explicit(&g_completed, 1, memory_order_release);
        return 0;
    }
    return 1;
}

void g7_motion_stop(void) {
    atomic_store_explicit(&g_stop, 1, memory_order_release);
    if (atomic_load_explicit(&g_mutex, memory_order_acquire))
        set_state(G7_MOTION_STOPPED, 1);
    (void)g7_motion_service();
}

int g7_motion_get_raw_sample(g7_motion_sample *out) {
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!atomic_load_explicit(&g_mutex, memory_order_acquire)) {
        out->state = G7_MOTION_STOPPED; return 0;
    }
    SDL_LockMutex(g_mutex);
    uint64_t (*now_us)(void) = g_io.now_us;
    SDL_UnlockMutex(g_mutex);
    uint64_t now = now_us ? now_us() : 0;
    SDL_LockMutex(g_mutex);
    if (atomic_load_explicit(&g_stop, memory_order_acquire)) {
        out->state = G7_MOTION_STOPPED;
    } else if (g_state == G7_MOTION_RUNNING && g_latest.fresh_valid &&
        now >= g_latest.monotonic_us && now - g_latest.monotonic_us <= G7_STALE_US) {
        *out = g_latest;
        out->state = g_state;
    } else {
        if (g_latest.fresh_valid &&
            (now < g_latest.monotonic_us || now - g_latest.monotonic_us > G7_STALE_US))
            memset(&g_latest, 0, sizeof(g_latest));
        out->state = g_state;
    }
    SDL_UnlockMutex(g_mutex);
    return out->fresh_valid != 0;
}

#ifdef G7_MOTION_TESTING
int g7_motion_test_install_io(const g7_motion_io *io) {
    if (!g7_motion_service()) return 0;
    if (io && (!io->select || !io->open || !io->read_timeout || !io->write ||
        !io->close || !io->now_us)) return 0;
    if (!ensure_mutex()) return 0;
    SDL_LockMutex(g_mutex);
    g_io = io ? *io : g_real_io;
    SDL_UnlockMutex(g_mutex);
    return 1;
}
int g7_motion_test_decode(const unsigned char *data, size_t size,
                          g7_motion_sample *out) { return decode(data, size, out); }
#endif

#else

int g7_motion_start(uint16_t vendor_id, uint16_t product_id) {
    (void)vendor_id; (void)product_id; return 0;
}
void g7_motion_stop(void) {}
int g7_motion_service(void) { return 1; }
int g7_motion_get_raw_sample(g7_motion_sample *out) {
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    out->state = G7_MOTION_UNSUPPORTED;
    return 0;
}
#ifdef G7_MOTION_TESTING
int g7_motion_test_install_io(const g7_motion_io *io) { (void)io; return 0; }
int g7_motion_test_decode(const unsigned char *data, size_t size,
                          g7_motion_sample *out) { (void)data; (void)size; (void)out; return 0; }
#endif
#endif
