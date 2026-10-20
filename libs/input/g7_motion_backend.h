#ifndef G7_MOTION_BACKEND_H
#define G7_MOTION_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Raw counts only. No SIXAXIS units, axis remapping, or synthetic neutral. */
typedef enum g7_motion_state {
    G7_MOTION_STOPPED = 0,
    G7_MOTION_STARTING = 1,
    G7_MOTION_RUNNING = 2,
    G7_MOTION_NO_DEVICE = 3,
    G7_MOTION_AMBIGUOUS = 4,
    G7_MOTION_IO_ERROR = 5,
    G7_MOTION_UNSUPPORTED = 6
} g7_motion_state;

typedef struct g7_motion_sample {
    int16_t gyro[3];
    int16_t accel[3];
    uint64_t monotonic_us;
    uint8_t fresh_valid;
    g7_motion_state state;
} g7_motion_sample;

/* Start launches one worker for the already matched VID/PID. The owner must
   serialize start, stop, service, and install_io; polling may run concurrently.
   Start reaps completed workers and rejects any previous active worker,
   including one with a pending stop. Returns 1 for launch, 0 for rejection or
   failure. Terminal errors persist until explicit stop or restart.
   The backend never retries automatically. */
int g7_motion_start(uint16_t vendor_id, uint16_t product_id);
/* Immediately invalidate samples and request stop, without waiting on I/O.
   A stalled worker retains its resources until it finishes. */
void g7_motion_stop(void);
/* Reap only a worker proven finished after all I/O cleanup and state writes.
   Returns 1 when no worker remains, 0 while any worker is active. Never waits
   for running/stalled I/O. Call again later before an explicit restart. */
int g7_motion_service(void);
/* Returns 1 only for a valid report no older than 100 ms. Always fills out.
   A stale/failed sample contains zero raw fields and fresh_valid=0. */
int g7_motion_get_raw_sample(g7_motion_sample *out);

/* I/O seam for isolated fixtures; production uses the lifecycle APIs above. */
typedef struct g7_motion_io {
    /* select: 1 unique, 0 absent, -2 ambiguous, -1 I/O error. */
    int (*select)(uint16_t, uint16_t, char *, size_t);
    void *(*open)(const char *);
    int (*read_timeout)(void *, unsigned char *, size_t, int);
    int (*write)(void *, const unsigned char *, size_t);
    void (*close)(void *);
    uint64_t (*now_us)(void);
} g7_motion_io;
#ifdef G7_MOTION_TESTING
/* Install only when service reports idle (completed workers may be reaped).
   Rejected while any worker is active, including after stop. CPU-only fixtures. */
int g7_motion_test_install_io(const g7_motion_io *io);
int g7_motion_test_decode(const unsigned char *data, size_t size,
                          g7_motion_sample *out);
#endif

#ifdef __cplusplus
}
#endif
#endif
