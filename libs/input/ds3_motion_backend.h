#ifndef DS3_MOTION_BACKEND_H
#define DS3_MOTION_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Original PS3 sensor counts (0..1023), ordered X, Y, Z, single yaw gyro.
   Windows SXS already supplies the PS3 X convention; do not invert again.
   Feature reads only: this backend sends no output, pairing, firmware, or
   calibration commands. It accepts only a unique 054C:0268 SXS collection. */
typedef enum ds3_motion_state {
    DS3_MOTION_STOPPED = 0,
    DS3_MOTION_STARTING = 1,
    DS3_MOTION_RUNNING = 2,
    DS3_MOTION_NO_DEVICE = 3,
    DS3_MOTION_AMBIGUOUS = 4,
    DS3_MOTION_IO_ERROR = 5,
    DS3_MOTION_UNSUPPORTED = 6
} ds3_motion_state;

typedef struct ds3_motion_sample {
    uint16_t sensor[4];
    uint64_t monotonic_us;
    uint8_t fresh_valid;
    ds3_motion_state state;
} ds3_motion_sample;

/* Start launches one worker for the already matched VID/PID. The owner must
   serialize start, stop, service, and install_io; polling may run concurrently.
   Start reaps completed workers and rejects any previous active worker,
   including one with a pending stop. Returns 1 for launch, 0 for rejection or
   failure. Terminal errors persist until explicit stop or restart.
   The backend never retries automatically. */
int ds3_motion_start(uint16_t vendor_id, uint16_t product_id);
/* Immediately invalidate samples and request stop, without waiting on I/O.
   A stalled worker retains its resources until it finishes. */
void ds3_motion_stop(void);
/* Reap only a worker proven finished after all I/O cleanup and state writes.
   Returns 1 when no worker remains, 0 while any worker is active. Never waits
   for running/stalled I/O. Call again later before an explicit restart. */
int ds3_motion_service(void);
/* Returns 1 only for a valid report no older than 100 ms. Always fills out.
   A stale/failed sample contains zero raw fields and fresh_valid=0. */
int ds3_motion_get_sample(ds3_motion_sample *out);

/* I/O seam for isolated fixtures; production uses the lifecycle APIs above. */
typedef struct ds3_motion_io {
    /* select: 1 unique, 0 absent, -2 ambiguous, -1 I/O error. */
    int (*select)(uint16_t, uint16_t, char *, size_t);
    void *(*open)(const char *);
    /* SDL_hid_get_feature_report semantics; buffer[0] is the requested ID. */
    int (*get_feature)(void *, unsigned char *, size_t);
    void (*close)(void *);
    uint64_t (*now_us)(void);
} ds3_motion_io;
#ifdef DS3_MOTION_TESTING
/* Install only when service reports idle (completed workers may be reaped).
   Rejected while any worker is active, including after stop. CPU-only fixtures. */
int ds3_motion_test_install_io(const ds3_motion_io *io);
/* Requires 50..64 bytes, F2/00:00:3F header, and all counts in 0..1023.
   Decoder clears output on malformed input; it does not infer physical motion. */
int ds3_motion_test_decode(const unsigned char *data, size_t size,
                          ds3_motion_sample *out);
#endif

#ifdef __cplusplus
}
#endif
#endif
