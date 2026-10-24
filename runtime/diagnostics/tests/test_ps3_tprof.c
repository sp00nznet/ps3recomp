/* PS3_TPROF: two registered threads (an SPU-kind one spinning in a named
 * function, a PPU-kind one sleeping) and one unregistered spinner. The
 * profile must hold samples of the registered threads only -- the PPU one
 * only with PS3_TPROF_PPU=1 -- and nothing at all when PS3_TPROF is unset.
 * Build:
 *   cc -O1 -pthread -Iinclude runtime/diagnostics/tests/test_ps3_tprof.c */
#include "ps3emu/ps3_tprof.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_stop, s_ready;

__attribute__((noinline)) static void tprof_test_spin(void)
{
    volatile unsigned x = 0;
    while (!__atomic_load_n(&s_stop, __ATOMIC_ACQUIRE)) x++;
}
static void* spinner(void* arg)
{
    ps3_tprof_thread('S', 7);
    __atomic_fetch_add(&s_ready, 1, __ATOMIC_RELEASE);
    tprof_test_spin();
    ps3_tprof_thread_exit();
    return arg;
}
static void* sleeper(void* arg)
{
    ps3_tprof_thread('P', 3);
    __atomic_fetch_add(&s_ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&s_stop, __ATOMIC_ACQUIRE)) usleep(1000);
    ps3_tprof_thread_exit();
    return arg;
}
static void* stranger(void* arg)
{
    __atomic_fetch_add(&s_ready, 1, __ATOMIC_RELEASE);
    volatile unsigned x = 0;
    while (!__atomic_load_n(&s_stop, __ATOMIC_ACQUIRE)) x++;
    return arg;
}

int main(int argc, char** argv)
{
#if defined(__APPLE__) && defined(__aarch64__)
    const char* out = getenv("PS3_TPROF");
    const char* trigger = getenv("PS3_TPROF_TRIGGER");
    if (out && trigger) unlink(trigger);
    pthread_t a, b, c;
    pthread_create(&a, NULL, spinner, NULL);
    pthread_create(&b, NULL, sleeper, NULL);
    pthread_create(&c, NULL, stranger, NULL);
    /* Register both kinds before releasing the sampler. Collect to its cap,
     * rather than requiring a hosted runner to meet a fixed sampling rate. */
    const uint64_t deadline = ps3_ft_now() + 8000000000ull;
    while (__atomic_load_n(&s_ready, __ATOMIC_ACQUIRE) != 3) {
        if (ps3_ft_now() >= deadline) { printf("FAIL: thread registration timeout\n"); return 1; }
        usleep(1000);
    }
    if (out && trigger) {
        FILE* go = fopen(trigger, "w");
        if (!go) { printf("FAIL: cannot release sampler\n"); return 1; }
        fclose(go);
    }
    while (out && __atomic_load_n(&g_ps3_tprof.on, __ATOMIC_ACQUIRE) != 3) {
        if (ps3_ft_now() >= deadline) { printf("FAIL: sampler completion timeout\n"); return 1; }
        usleep(1000);
    }
    __atomic_store_n(&s_stop, 1, __ATOMIC_RELEASE);
    pthread_join(a, NULL); pthread_join(b, NULL); pthread_join(c, NULL);
    if (!out) {
        if (g_ps3_tprof.buf || g_ps3_tprof.nthreads) { printf("FAIL: armed while off\n"); return 1; }
        printf("PASS off\n");
        return 0;
    }
    FILE* f = fopen(out, "r");
    if (!f) { printf("FAIL: no profile\n"); return 1; }
    char line[4096]; unsigned s7 = 0, p3 = 0, other = 0, header = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') { header++; continue; }
        if (!strncmp(line, "S 7 ", 4)) s7++;
        else if (!strncmp(line, "P 3 ", 4)) p3++;
        else other++;
    }
    fclose(f);
    printf("header=%u S7=%u P3=%u other=%u\n", header, s7, p3, other);
    const int want_ppu = getenv("PS3_TPROF_PPU") && !strcmp(getenv("PS3_TPROF_PPU"), "1");
    if (header != 1 || s7 < 100 || (want_ppu ? p3 < 100 : p3 != 0) || other) { printf("FAIL\n"); return 1; }
    printf("PASS on\n");
    (void)argc; (void)argv;
#endif
    return 0;
}
