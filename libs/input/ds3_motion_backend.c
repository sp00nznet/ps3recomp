#include "ds3_motion_backend.h"
#include <string.h>

#if defined(_WIN32) && defined(DS3_WITH_SDL_HIDAPI)

#include <SDL2/SDL.h>
#include <SDL2/SDL_hidapi.h>
#include <stdatomic.h>

enum { DS3_REPORT_BYTES = 64, DS3_POLL_MS = 10 };
static const uint16_t DS3_VID = 0x054c;
static const uint16_t DS3_PID = 0x0268;
static const uint16_t DS3_USAGE_PAGE = 0x0001;
static const uint16_t DS3_USAGE = 0x0004;
static const uint64_t DS3_STALE_US = 100000;

/* Published once and retained for concurrent pollers for process lifetime. */
static _Atomic(SDL_mutex *) g_mutex;
static SDL_Thread *g_thread;
static atomic_int g_stop;
static atomic_int g_completed = 1;
static ds3_motion_state g_state = DS3_MOTION_STOPPED;
static ds3_motion_sample g_latest;

/* Independently implemented semantic reference: RPCS3 ds3_pad_handler.h/.cpp
 * (Windows DS3_HID_OFFSET=1, packed 49-byte report), and DsHidMini
 * setup-v3.27.0 SXS HID/feature handlers. Live read-only framing verification
 * confirmed F2/00:00:3F, 63 returned bytes from a 64-byte request, and LE16
 * X/Z/Y/gyro at 42/44/46/48. No RPCS3 implementation is copied here.
 * https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Input/ds3_pad_handler.h
 * https://github.com/nefarius/DsHidMini/blob/setup-v3.27.0/driver/HID.FeatureReport.c
 */
static uint16_t unsigned_le16(const unsigned char *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int decode(const unsigned char *data, size_t size, ds3_motion_sample *out) {
    static const size_t offsets[4] = {42, 46, 44, 48};
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!data || size < 50 || size > DS3_REPORT_BYTES ||
        (data[0] != 0xf2 && data[0] != 0x00) || data[1] != 0x00 || data[2] != 0x3f)
        return 0;
    ds3_motion_sample sample = {0};
    for (int i = 0; i < 4; ++i) {
        sample.sensor[i] = unsigned_le16(data + offsets[i]);
        if (sample.sensor[i] > 1023) return 0;
    }
    sample.fresh_valid = 1;
    *out = sample;
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
            d->usage_page != DS3_USAGE_PAGE || d->usage != DS3_USAGE || !d->path) continue;
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
static void *real_open(const char *path) { return SDL_hid_open_path(path, 0); }
static int real_get_feature(void *device, unsigned char *data, size_t capacity) {
    return SDL_hid_get_feature_report((SDL_hid_device *)device, data, capacity);
}
static void real_close(void *device) { SDL_hid_close((SDL_hid_device *)device); }
static const ds3_motion_io g_real_io = {
    real_select, real_open, real_get_feature, real_close, real_now_us
};
static ds3_motion_io g_io;
static uint16_t g_vid, g_pid;

static int ensure_mutex(void) {
    if (atomic_load_explicit(&g_mutex, memory_order_acquire)) return 1;
    SDL_mutex *mutex = SDL_CreateMutex();
    if (!mutex) return 0;
    atomic_store_explicit(&g_mutex, mutex, memory_order_release);
    return 1;
}

static void set_state(ds3_motion_state state, int clear) {
    SDL_LockMutex(g_mutex);
    g_state = atomic_load_explicit(&g_stop, memory_order_acquire) ?
              DS3_MOTION_STOPPED : state;
    if (clear) memset(&g_latest, 0, sizeof(g_latest));
    SDL_UnlockMutex(g_mutex);
}

/* Driver calls occur only here. In particular no sample mutex is held during
 * get_feature, select, open, close, or hid initialization/cleanup. */
static int read_sample(void *device, unsigned char id, ds3_motion_sample *sample) {
    unsigned char data[DS3_REPORT_BYTES] = {0};
    data[0] = id;
    int count = g_io.get_feature(device, data, sizeof(data));
    if (count < 0 || count > DS3_REPORT_BYTES || data[0] != id) return 0;
    return decode(data, (size_t)count, sample);
}

static int worker(void *unused) {
    (void)unused;
    char path[1024] = {0};
    void *device = NULL;
    int using_real = g_io.select == real_select;
    int hid_initialized = 0;
    unsigned char report_id = 0xf2;
    ds3_motion_sample sample;
    if (atomic_load_explicit(&g_stop, memory_order_acquire)) goto done;
    if (using_real && SDL_hid_init() != 0) {
        set_state(DS3_MOTION_IO_ERROR, 1); goto done;
    }
    hid_initialized = using_real;
    int selected = g_io.select(g_vid, g_pid, path, sizeof(path));
    if (atomic_load_explicit(&g_stop, memory_order_acquire)) goto done;
    if (selected != 1) {
        set_state(selected == 0 ? DS3_MOTION_NO_DEVICE :
                  selected == -2 ? DS3_MOTION_AMBIGUOUS : DS3_MOTION_IO_ERROR, 1);
        goto done;
    }
    device = g_io.open(path);
    if (atomic_load_explicit(&g_stop, memory_order_acquire)) goto done;
    if (!device) { set_state(DS3_MOTION_IO_ERROR, 1); goto done; }
    int valid = read_sample(device, report_id, &sample);
    if (atomic_load_explicit(&g_stop, memory_order_acquire)) goto done;
    if (!valid) {
        report_id = 0;
        valid = read_sample(device, report_id, &sample);
        if (atomic_load_explicit(&g_stop, memory_order_acquire)) goto done;
    }
    if (!valid) { set_state(DS3_MOTION_IO_ERROR, 1); goto done; }
    /* A mode is accepted only after one complete, correctly framed report. */
    while (!atomic_load_explicit(&g_stop, memory_order_acquire)) {
        sample.monotonic_us = g_io.now_us();
        SDL_LockMutex(g_mutex);
        if (!atomic_load_explicit(&g_stop, memory_order_acquire)) {
            g_state = DS3_MOTION_RUNNING;
            g_latest = sample;
        }
        SDL_UnlockMutex(g_mutex);
        SDL_Delay(DS3_POLL_MS);
        if (atomic_load_explicit(&g_stop, memory_order_acquire)) break;
        if (!read_sample(device, report_id, &sample)) {
            set_state(DS3_MOTION_IO_ERROR, 1); break;
        }
    }
done:
    if (device) g_io.close(device);
    if (hid_initialized) SDL_hid_exit();
    if (atomic_load_explicit(&g_stop, memory_order_acquire))
        set_state(DS3_MOTION_STOPPED, 1);
    /* No I/O, mutex use, or state writes may follow this publication. */
    atomic_store_explicit(&g_completed, 1, memory_order_release);
    return 0;
}

int ds3_motion_service(void) {
    if (!g_thread) return 1;
    if (!atomic_load_explicit(&g_completed, memory_order_acquire)) return 0;
    SDL_WaitThread(g_thread, NULL);
    g_thread = NULL;
    return 1;
}

int ds3_motion_start(uint16_t vendor_id, uint16_t product_id) {
    if (!ds3_motion_service()) return 0;
    if (vendor_id != DS3_VID ||
        product_id != DS3_PID)
        return 0;
    if (!ensure_mutex()) return 0;
    SDL_LockMutex(g_mutex);
    if (!g_io.select) g_io = g_real_io;
    g_vid = vendor_id; g_pid = product_id;
    atomic_store_explicit(&g_stop, 0, memory_order_release);
    g_state = DS3_MOTION_STARTING;
    memset(&g_latest, 0, sizeof(g_latest));
    SDL_UnlockMutex(g_mutex);
    atomic_store_explicit(&g_completed, 0, memory_order_release);
    g_thread = SDL_CreateThread(worker, "ds3-motion-hid", NULL);
    if (!g_thread) {
        set_state(DS3_MOTION_IO_ERROR, 1);
        atomic_store_explicit(&g_completed, 1, memory_order_release);
        return 0;
    }
    return 1;
}

void ds3_motion_stop(void) {
    atomic_store_explicit(&g_stop, 1, memory_order_release);
    if (atomic_load_explicit(&g_mutex, memory_order_acquire))
        set_state(DS3_MOTION_STOPPED, 1);
    (void)ds3_motion_service();
}

int ds3_motion_get_sample(ds3_motion_sample *out) {
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!atomic_load_explicit(&g_mutex, memory_order_acquire)) {
        out->state = DS3_MOTION_STOPPED; return 0;
    }
    SDL_LockMutex(g_mutex);
    uint64_t (*now_us)(void) = g_io.now_us;
    SDL_UnlockMutex(g_mutex);
    uint64_t now = now_us ? now_us() : 0;
    SDL_LockMutex(g_mutex);
    if (atomic_load_explicit(&g_stop, memory_order_acquire)) {
        out->state = DS3_MOTION_STOPPED;
    } else if (g_state == DS3_MOTION_RUNNING && g_latest.fresh_valid &&
        now >= g_latest.monotonic_us && now - g_latest.monotonic_us <= DS3_STALE_US) {
        *out = g_latest;
        out->state = g_state;
    } else {
        if (g_latest.fresh_valid &&
            (now < g_latest.monotonic_us || now - g_latest.monotonic_us > DS3_STALE_US))
            memset(&g_latest, 0, sizeof(g_latest));
        out->state = g_state;
    }
    SDL_UnlockMutex(g_mutex);
    return out->fresh_valid != 0;
}

#ifdef DS3_MOTION_TESTING
int ds3_motion_test_install_io(const ds3_motion_io *io) {
    if (!ds3_motion_service()) return 0;
    if (io && (!io->select || !io->open || !io->get_feature ||
        !io->close || !io->now_us)) return 0;
    if (!ensure_mutex()) return 0;
    SDL_LockMutex(g_mutex);
    g_io = io ? *io : g_real_io;
    SDL_UnlockMutex(g_mutex);
    return 1;
}
int ds3_motion_test_decode(const unsigned char *data, size_t size,
                          ds3_motion_sample *out) { return decode(data, size, out); }
#endif

#else

int ds3_motion_start(uint16_t vendor_id, uint16_t product_id) {
    (void)vendor_id; (void)product_id; return 0;
}
void ds3_motion_stop(void) {}
int ds3_motion_service(void) { return 1; }
int ds3_motion_get_sample(ds3_motion_sample *out) {
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    out->state = DS3_MOTION_UNSUPPORTED;
    return 0;
}
#ifdef DS3_MOTION_TESTING
int ds3_motion_test_install_io(const ds3_motion_io *io) { (void)io; return 0; }
int ds3_motion_test_decode(const unsigned char *data, size_t size,
                          ds3_motion_sample *out) {
    (void)data; (void)size;
    if (out) memset(out, 0, sizeof(*out));
    return 0;
}
#endif
#endif
