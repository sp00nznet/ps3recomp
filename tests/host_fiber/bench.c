/*
 * ps3recomp - tests/host_fiber/bench.c
 *
 * Resume+yield through the AArch64 switch against swapcontext on the same
 * host, plus the raw two-sided switch underneath them. Batch means, not a gate:
 * the absolute numbers depend on the core and its power state.
 */
#define _XOPEN_SOURCE 600
#define _DARWIN_C_SOURCE 1
#include "host_fiber.h"
#include <errno.h>
#include <pthread/qos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <time.h>
#include <ucontext.h>
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#define STACK (4u * 1024u * 1024u)
static ps3_host_ctx raw_runner, raw_fiber;
static ps3_host_runner runner;
static ps3_host_fiber fiber;
static ucontext_t uc_runner, uc_fiber;
static void raw_entry(void* arg)
{ (void)arg; for (;;) ps3_host_ctx_switch(&raw_fiber, &raw_runner); }
static void full_entry(void* arg)
{ (void)arg; for (;;) ps3_host_fiber_yield(&fiber); }
static void uc_entry(void)
{ for (;;) if (swapcontext(&uc_fiber, &uc_runner)) abort(); }
static void pair(unsigned variant)
{
    if (!variant) ps3_host_ctx_switch(&raw_runner, &raw_fiber);
    else if (variant == 1) ps3_host_fiber_resume(&runner, &fiber);
    else if (swapcontext(&uc_runner, &uc_fiber)) abort();
}
static int compare(const void* a, const void* b)
{ const double x = *(const double*)a, y = *(const double*)b; return (x > y) - (x < y); }
static size_t number(const char* s)
{
    char* end; errno = 0; unsigned long long n = strtoull(s, &end, 10);
    if (errno || !*s || *end || !n || n > SIZE_MAX) { fprintf(stderr, "invalid count: %s\n", s); exit(2); }
    return (size_t)n;
}
int main(int argc, char** argv)
{
    size_t warmup = 200000, runs = 5, batches = 2000, batch_size = 1000;
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 == argc) return 2;
        size_t n = number(argv[i + 1]);
        if (!strcmp(argv[i], "--warmup")) warmup = n;
        else if (!strcmp(argv[i], "--runs")) runs = n;
        else if (!strcmp(argv[i], "--batches")) batches = n;
        else if (!strcmp(argv[i], "--batch-size")) batch_size = n;
        else return 2;
    }
    if (runs > SIZE_MAX / batches || runs * batches > SIZE_MAX / sizeof(double)) return 2;
    size_t count = runs * batches;
    double* samples = malloc(count * sizeof *samples);
    unsigned char* raw_stack = malloc(STACK);
    unsigned char* uc_stack = malloc(STACK);
    if (!samples || !raw_stack || !uc_stack) return 1;
    uint64_t* top = (uint64_t*)(raw_stack + STACK) - 2; top[0] = top[1] = 0;
    raw_fiber.sp = raw_fiber.fp = (uintptr_t)top;
    raw_fiber.lr = (uintptr_t)raw_entry;
    if (ps3_host_fiber_create(&fiber, STACK, full_entry, NULL, 0)) return 1;
    if (getcontext(&uc_fiber)) return 1;
    uc_fiber.uc_stack.ss_sp = uc_stack; uc_fiber.uc_stack.ss_size = STACK; uc_fiber.uc_link = NULL;
    makecontext(&uc_fiber, uc_entry, 0);
    int qos_error = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    struct utsname env; if (uname(&env)) return 1;
    char cpu[128] = {0}; size_t size = sizeof cpu;
    if (sysctlbyname("machdep.cpu.brand_string", cpu, &size, NULL, 0)) strcpy(cpu, "unknown");
    printf("{\"environment\":{\"os\":\"%s\",\"release\":\"%s\",\"arch\":\"%s\",\"cpu\":\"%s\","
           "\"compiler\":\"%s\",\"qos_error\":%d,\"clock\":\"CLOCK_UPTIME_RAW\"},"
           "\"warmup\":%zu,\"runs\":%zu,\"batches\":%zu,\"batch_size\":%zu,\"variants\":[",
           env.sysname, env.release, env.machine, cpu, __VERSION__, qos_error, warmup, runs, batches, batch_size);
    const char* names[] = {"raw", "resume_yield", "swapcontext"};
    for (unsigned variant = 0; variant < 3; ++variant) {
        for (size_t i = 0; i < warmup; ++i) pair(variant);
        for (size_t i = 0; i < count; ++i) {
            uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            for (size_t j = 0; j < batch_size; ++j) pair(variant);
            samples[i] = (double)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start) / (double)batch_size;
        }
        qsort(samples, count, sizeof *samples, compare);
        printf("%s{\"name\":\"%s\",\"p50_ns\":%.3f,\"p99_ns\":%.3f}",
               variant ? "," : "", names[variant], samples[count / 2], samples[(count - 1) * 99 / 100]);
    }
    puts("]}");
    /* Suspended bench contexts intentionally live until process exit. */
    free(samples);
    return 0;
}
