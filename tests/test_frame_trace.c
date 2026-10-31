/*
 * ps3_trace (PS3RECOMP_TRACE) through its API, with synthetic threads.
 *
 *   test_frame_trace off   PS3RECOMP_TRACE unset: hooks stay off, nothing written
 *   test_frame_trace on    PS3RECOMP_TRACE=<file>: four threads record nested
 *                          spans, instants, poll runs and async waits; prints
 *                          the expected per-event counts as one JSON line
 *
 * tests/test_frame_trace.py runs both and checks the file: that it loads as
 * JSON, that the counts match, that every thread's spans nest, that async
 * pairs match, and that records reached the file before exit (the flush every
 * PS3RECOMP_TRACE_FLUSH_MB, which the driver sets small).
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L   /* clock_gettime, nanosleep under -std=c17 */
#endif
#include "ps3emu/ps3_trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  include <windows.h>
#else
#  include <pthread.h>
#  include <sys/stat.h>
#endif

enum { THREADS = 4, ITERS = 700, WAITS = 3, POLL_RUNS = 5, POLLS = 10, ASYNC = 6 };

/* Spin until the clock has moved on, so no two span boundaries on a thread
 * share a timestamp and parent/child order is never a tie. */
static void tick(void)
{
    const uint64_t t = ps3_trace_now();
    while (ps3_trace_now() - t < 1000) { }
}

static void one_job(int thread, int iter)
{
    PS3_TRACE_SCOPE(PS3_TR_SPU_JOB, 0x10000u + (uint32_t)iter, (uint32_t)thread);
    tick();
    const uint64_t run = ps3_trace_begin();
    tick();
    for (int k = 0; k < WAITS; k++) {
        const uint64_t w = ps3_trace_begin();
        tick();
        ps3_trace_end(w, PS3_TR_SPU_MBOX_WAIT, 29, 0x100u * (uint32_t)k);
        tick();
    }
    ps3_trace_end(run, PS3_TR_SPU_RUN, 7, (uint32_t)thread);
    tick();
    {
        PS3_TRACE_SCOPE(PS3_TR_SPU_LOCKLINE_RETRY, 0x2000080u, 3);
        tick();
    }
    tick();
}

static void worker(int thread)
{
    char name[32];
    snprintf(name, sizeof name, "synthetic %d", thread);
    ps3_trace_thread_name(name);
    for (int i = 0; i < ITERS; i++) {
        one_job(thread, i);
        ps3_trace_instant(PS3_TR_GCM_FLIP, (uint32_t)i & 1u, 0);
    }
    /* Guest spins seen as repeated calls: each run of POLLS calls is one span. */
    for (int r = 0; r < POLL_RUNS; r++) {
        for (int p = 0; p < POLLS; p++) { ps3_trace_poll(PS3_TR_PPU_GCM_FLIP_POLL, 0); tick(); }
        ps3_trace_poll_end(PS3_TR_PPU_GCM_FLIP_POLL);
        tick();
    }
    /* Waits that outlive the pass they began in: async pairs. */
    for (int k = 0; k < ASYNC; k++) {
        const uint64_t t0 = ps3_trace_now();
        tick();
        ps3_trace_span(PS3_TR_RSX_SEMA_WAIT, t0, ps3_trace_now(), 0x40u + (uint32_t)thread, (uint32_t)k);
    }
}

#ifdef _WIN32
static DWORD WINAPI thread_main(LPVOID p) { worker((int)(intptr_t)p); return 0; }
#else
static void* thread_main(void* p) { worker((int)(intptr_t)p); return NULL; }
#endif

static long file_size(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fclose(f);
    return n;
}

int main(int argc, char** argv)
{
    const int on = argc > 1 && strcmp(argv[1], "on") == 0;
    if (!on) {
        PS3_TRACE_SCOPE(PS3_TR_SPU_JOB, 1, 2);
        ps3_trace_instant(PS3_TR_GCM_FLIP, 0, 0);
        if (ps3_trace_on() || g_ps3_tr.on != 1 || g_ps3_tr.nrings != 0) {
            printf("FAIL: tracing is on with PS3RECOMP_TRACE unset\n");
            return 1;
        }
        printf("off ok\n");
        return 0;
    }
    if (!ps3_trace_on()) { printf("FAIL: PS3RECOMP_TRACE set but tracing is off\n"); return 1; }

#ifdef _WIN32
    HANDLE th[THREADS];
    for (int i = 0; i < THREADS; i++)
        th[i] = CreateThread(NULL, 0, thread_main, (LPVOID)(intptr_t)i, 0, NULL);
    WaitForMultipleObjects(THREADS, th, TRUE, INFINITE);
#else
    pthread_t th[THREADS];
    for (int i = 0; i < THREADS; i++)
        pthread_create(&th[i], NULL, thread_main, (void*)(intptr_t)i);
    for (int i = 0; i < THREADS; i++) pthread_join(th[i], NULL);
#endif

    /* The flusher writes once PS3RECOMP_TRACE_FLUSH_MB is pending, without
     * waiting for exit: give it a moment, then measure before shutting down. */
    long mid = 0;
    for (int i = 0; i < 400 && (mid = file_size(g_ps3_tr.path)) < 4096; i++) {
#ifdef _WIN32
        Sleep(5);
#else
        struct timespec d = { 0, 5000000 };
        nanosleep(&d, NULL);
#endif
    }
    ps3_trace_shutdown();

    printf("{\"midrun_bytes\":%ld,\"threads\":%d,\"counts\":{"
           "\"spu.job\":%d,\"spu.run\":%d,\"spu.mbox_wait\":%d,\"spu.lockline_retry\":%d,"
           "\"gcm.flip\":%d,\"ppu.gcm_flip_poll\":%d,\"rsx.sema_wait\":%d}}\n",
           mid, THREADS, THREADS * ITERS, THREADS * ITERS, THREADS * ITERS * WAITS,
           THREADS * ITERS, THREADS * ITERS, THREADS * POLL_RUNS, THREADS * ASYNC);
    return 0;
}
