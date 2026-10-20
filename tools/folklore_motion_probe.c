/* folklore_motion_probe: standalone SDL2 controller motion-sensor diagnostic.
 * Original tooling. Reads sensors only through normal SDL sensor enabling and
 * the production adapter in libs/input/pad_motion_sdl.h. Writes no files except
 * an optional, exclusively created CSV log. */
#define _CRT_SECURE_NO_WARNINGS
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#endif
#include "../libs/input/pad_motion_sdl.h"

#if !SDL_VERSION_ATLEAST(2, 26, 0)
#error "folklore_motion_probe requires SDL >= 2.26"
#endif

#define STALE_MS 500u

static void usage(FILE *out)
{
    fprintf(out,
            "usage: folklore_motion_probe [--seconds N] [--index N] [--csv PATH] [--help]\n"
            "  --seconds N  poll duration, 1..60 (default 10)\n"
            "  --index N    SDL joystick device index, 0..15 (default 0)\n"
            "  --csv PATH   write samples to a NEW file (existing files are never overwritten)\n"
            "exit: 0 ok, 1 error/no verified samples, 2 bad arguments or CSV exists,\n"
            "      3 device absent or without sensors, 4 device disconnected\n");
}

static int bad_arg(const char *msg)
{
    fprintf(stderr, "error: %s\n", msg);
    usage(stderr);
    return 2;
}

static int parse_int(const char *s, int lo, int hi, int *out)
{
    char *end = NULL;
    long v;
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno || end == s || *end || v < lo || v > hi)
        return 0;
    *out = (int)v;
    return 1;
}

static FILE *open_exclusive(const char *path)
{
#ifdef _WIN32
    FILE *f;
    int fd = _open(path, _O_WRONLY | _O_CREAT | _O_EXCL | _O_TEXT, _S_IREAD | _S_IWRITE);
    if (fd < 0)
        return NULL;
    f = _fdopen(fd, "w");
    if (!f)
        _close(fd);
    return f;
#else
    return fopen(path, "wx");
#endif
}

int main(int argc, char **argv)
{
    int seconds = 10, index = 0, rc = 0, gone = 0, write_err = 0, i, k;
    const char *csv_path = NULL;
    FILE *csv = NULL;
    SDL_GameController *gc = NULL;
    PadMotionSDL st;
    SDL_version cv, lv;
    SDL_JoystickID iid = -1;
    unsigned long polls = 0, valid = 0, fresh = 0, changes = 0, stale_events = 0;
    float lo[6], hi[6];
    unsigned ps_lo[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}, ps_hi[4] = {0, 0, 0, 0};
    Uint64 start, end, last_change, last_ts = 0;
    int any_ts = 0, stale = 0;
    static const char *axis[6] = {"accel x", "accel y", "accel z", "gyro x", "gyro y", "gyro z"};

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help")) {
            usage(stdout);
            return 0;
        } else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
            if (!parse_int(argv[++i], 1, 60, &seconds))
                return bad_arg("--seconds must be an integer 1..60");
        } else if (!strcmp(argv[i], "--index") && i + 1 < argc) {
            if (!parse_int(argv[++i], 0, 15, &index))
                return bad_arg("--index must be an integer 0..15");
        } else if (!strcmp(argv[i], "--csv") && i + 1 < argc) {
            csv_path = argv[++i];
            if (!*csv_path)
                return bad_arg("--csv needs a non-empty path");
        } else {
            fprintf(stderr, "error: unknown or incomplete argument '%s'\n", argv[i]);
            usage(stderr);
            return 2;
        }
    }

    if (csv_path) {
        csv = open_exclusive(csv_path);
        if (!csv) {
            fprintf(stderr, "error: cannot create CSV '%s' exclusively (%s); existing files are never overwritten\n",
                    csv_path, strerror(errno));
            return 2;
        }
        if (fprintf(csv, "poll_ms,sensor_ts_us,raw_accel_ts_us,ax,ay,az,gx,gy,gz,ps3_0,ps3_1,ps3_2,ps3_3,valid\n") < 0)
            write_err = 1;
    }

    memset(&st, 0, sizeof st);
    for (k = 0; k < 6; k++) {
        lo[k] = INFINITY;
        hi[k] = -INFINITY;
    }

    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "error: SDL_Init failed: %s\n", SDL_GetError());
        if (csv)
            fclose(csv);
        return 1;
    }
    SDL_VERSION(&cv);
    SDL_GetVersion(&lv);
    printf("SDL compiled %d.%d.%d, linked %d.%d.%d\n", cv.major, cv.minor, cv.patch, lv.major, lv.minor, lv.patch);

    k = SDL_NumJoysticks();
    printf("devices: %d\n", k < 0 ? 0 : k);
    for (i = 0; i < k; i++) {
        const char *name = SDL_GameControllerNameForIndex(i);
        SDL_GameController *c;
        if (!SDL_IsGameController(i)) {
            printf("  [%d] not a game controller\n", i);
            continue;
        }
        c = SDL_GameControllerOpen(i);
        printf("  [%d] name=%s vendor=0x%04x product=0x%04x attached=%s accel=%s gyro=%s\n", i,
               name ? name : "(unknown)", SDL_JoystickGetDeviceVendor(i), SDL_JoystickGetDeviceProduct(i),
               c && SDL_GameControllerGetAttached(c) ? "yes" : "no",
               c && SDL_GameControllerHasSensor(c, SDL_SENSOR_ACCEL) ? "yes" : "no",
               c && SDL_GameControllerHasSensor(c, SDL_SENSOR_GYRO) ? "yes" : "no");
        if (c && i == index)
            gc = c;
        else if (c)
            SDL_GameControllerClose(c);
    }

    if (!gc) {
        fprintf(stderr, "error: target index %d absent or not an openable game controller\n", index);
        rc = 3;
        goto cleanup;
    }
    if (!SDL_GameControllerHasSensor(gc, SDL_SENSOR_ACCEL) || !SDL_GameControllerHasSensor(gc, SDL_SENSOR_GYRO) ||
        !pad_motion_sdl_capable(gc)) {
        fprintf(stderr, "error: device %d lacks accelerometer and/or gyroscope\n", index);
        rc = 3;
        goto cleanup;
    }
    iid = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc));
    pad_motion_sdl_set_enabled(&st, gc, 1);
    if (!SDL_GameControllerIsSensorEnabled(gc, SDL_SENSOR_ACCEL) ||
        !SDL_GameControllerIsSensorEnabled(gc, SDL_SENSOR_GYRO)) {
        fprintf(stderr, "error: could not enable sensors on device %d: %s\n", index, SDL_GetError());
        rc = 3;
        goto cleanup;
    }

    printf("READY index=%d seconds=%d (~100 Hz)\n", index, seconds);
    fflush(stdout);
    start = SDL_GetTicks64();
    end = start + (Uint64)seconds * 1000u;
    last_change = start;
    while (SDL_GetTicks64() < end) {
        SDL_Event ev;
        float a[3] = {0, 0, 0}, g[3] = {0, 0, 0};
        Uint64 now, ats = 0, gts = 0, ts;
        int ok;
        while (SDL_PollEvent(&ev))
            if (ev.type == SDL_CONTROLLERDEVICEREMOVED && ev.cdevice.which == iid)
                gone = 1;
        if (gone || !SDL_GameControllerGetAttached(gc)) {
            gone = 1;
            memset(&st, 0, sizeof st);
            fprintf(stderr, "error: device %d disconnected; aborting\n", index);
            rc = 4;
            break;
        }
        pad_motion_sdl_poll(&st, gc);
        now = SDL_GetTicks64();
        /* Raw readings are fetched separately for logging and may differ slightly
         * from the snapshot the production helper converted. */
        ok = SDL_GameControllerGetSensorDataWithTimestamp(gc, SDL_SENSOR_ACCEL, &ats, a, 3) == 0 &&
             SDL_GameControllerGetSensorDataWithTimestamp(gc, SDL_SENSOR_GYRO, &gts, g, 3) == 0 && st.valid;
        for (k = 0; k < 3 && ok; k++)
            ok = isfinite(a[k]) && isfinite(g[k]);
        polls++;
        ts = (Uint64)st.timestamp;
        if (ok) {
            valid++;
            for (k = 0; k < 6; k++) {
                float v = k < 3 ? a[k] : g[k - 3];
                if (v < lo[k]) lo[k] = v;
                if (v > hi[k]) hi[k] = v;
            }
            for (k = 0; k < 4; k++) {
                if (st.sample[k] < ps_lo[k]) ps_lo[k] = st.sample[k];
                if (st.sample[k] > ps_hi[k]) ps_hi[k] = st.sample[k];
            }
            if (ts) {
                any_ts = 1;
                if (ts != last_ts) {
                    changes++;
                    last_ts = ts;
                    last_change = now;
                    stale = 0;
                } else if (now - last_change > STALE_MS && !stale) {
                    stale = 1;
                    stale_events++;
                    fprintf(stderr, "warning: sensor timestamp unchanged for >%u ms (stale)\n", STALE_MS);
                }
            }
            if (!stale)
                fresh++;
        }
        if (csv && fprintf(csv, "%" SDL_PRIu64 ",%" SDL_PRIu64 ",%" SDL_PRIu64 ",%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%u,%u,%u,%u,%d\n",
                           now - start, ts, ats, a[0], a[1], a[2], g[0], g[1], g[2], (unsigned)st.sample[0],
                           (unsigned)st.sample[1], (unsigned)st.sample[2], (unsigned)st.sample[3],
                           ok && !stale) < 0)
            write_err = 1;
        SDL_Delay(10);
    }

    printf("== Folklore motion probe: sensor transport %s ==\n", fresh > 0 ? "verified" : "NOT verified");
    printf("gameplay: not tested\n");
    printf("polls %lu, valid %lu, fresh %lu, stale episodes %lu (timestamp unchanged >%u ms)\n", polls, valid,
           fresh, stale_events, STALE_MS);
    if (any_ts)
        printf("sensor timestamp: %lu changes (repeats are normal when polling faster than the sample rate)\n", changes);
    else
        printf("sensor timestamp: unavailable (reported as 0)\n");
    for (k = 0; k < 6; k++) {
        if (valid)
            printf("%s %s: min %.4f max %.4f\n", axis[k], k < 3 ? "m/s2" : "rad/s", lo[k], hi[k]);
        else
            printf("%s: n/a\n", axis[k]);
    }
    for (k = 0; k < 4; k++) {
        if (valid)
            printf("ps3 sensor[%d]: min %u max %u\n", k, ps_lo[k], ps_hi[k]);
        else
            printf("ps3 sensor[%d]: n/a\n", k);
    }
    printf("note: raw values are read separately and may differ slightly from the production helper snapshot\n");
    printf("note: ranges are as measured; no claim is made that physical motion occurred\n");
    if (!rc && fresh == 0)
        rc = 1;

cleanup:
    if (gc) {
        if (!gone)
            pad_motion_sdl_set_enabled(&st, gc, 0);
        SDL_GameControllerClose(gc);
    }
    memset(&st, 0, sizeof st);
    SDL_Quit();
    if (csv) {
        if (fclose(csv) != 0)
            write_err = 1;
        if (write_err) {
            fprintf(stderr, "error: failed writing CSV '%s'\n", csv_path);
            if (!rc)
                rc = 1;
        }
    }
    return rc;
}
