/*
 * ps3_tprof -- an in-process sampling profile of chosen host threads.
 *
 * PS3_TPROF=<path> samples every thread that called ps3_tprof_thread() once
 * per PS3_TPROF_INTERVAL_US (default 1000) for PS3_TPROF_SECONDS (default
 * 10), starting when PS3_TPROF_TRIGGER=<file> exists (immediately when
 * unset). For hosts where an external sampler cannot attach to the process.
 *
 * A sampler thread does what a debugger would: thread_suspend, read the
 * registers, walk the frame-pointer chain, thread_resume. Nothing runs on the
 * sampled thread, so no wait it is in gets interrupted -- a SIGPROF-based
 * sampler wedged the SPURS workers inside pthread_rwlock waits. The sampler
 * neither allocates nor takes a lock while a thread is stopped, since that
 * thread may hold the allocator's.
 *
 * <path> gets one sample per line:
 *   <kind> <who> <t us> <pc> <lr> <return addresses...>
 * kind is S (SPU, who = SPU id) or P (PPU, who = thread id); PPU threads are
 * sampled only with PS3_TPROF_PPU=1. Main-image addresses are written
 * image-relative in hex for offline symbolizing; frames in other images are
 * named "@symbol" with dladdr. Times are on the PS3_FTRACE clock, relative to
 * its base when it is recording, so samples join with the job spans of the
 * same run.
 *
 * Header-only and weak-shared like ps3_ftrace.h, so the hooked files keep
 * building on their own in self-contained tests. Off (the default),
 * ps3_tprof_thread() reads the environment once and returns. macOS arm64;
 * elsewhere it compiles to nothing.
 */
#ifndef PS3EMU_PS3_TPROF_H
#define PS3EMU_PS3_TPROF_H

#include <stdint.h>
#include "ps3emu/ps3_ftrace.h"

#if defined(__APPLE__) && defined(__aarch64__)
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <dlfcn.h>
#include <mach/mach_types.h>
#include <mach/thread_status.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Declared here rather than through <mach/mach.h>, whose vm_map.h collides
 * with the runtime's own vm_protect. */
extern kern_return_t thread_suspend(thread_act_t target_act);
extern kern_return_t thread_resume(thread_act_t target_act);
extern kern_return_t thread_get_state(thread_act_t target_act, thread_state_flavor_t flavor,
                                      thread_state_t old_state, mach_msg_type_number_t* old_stateCnt);

enum { PS3_TPROF_THREADS = 64, PS3_TPROF_DEPTH = 40 };

typedef struct {
    uint64_t t;
    uint32_t who;
    uint8_t kind, depth;
    uintptr_t pc[PS3_TPROF_DEPTH];
} ps3_tprof_rec;

typedef struct {
    int on;                       /* 0 unknown, 1 initialising, 2 armed, 3 off/done */
    int nthreads;                 /* slots claimed; a slot is sampled once ready */
    uint32_t cap, n;
    ps3_tprof_rec* buf;
    /* ready: 0 not sampled, 1 sampled, 2 being sampled right now */
    struct { mach_port_t port; uintptr_t lo, hi; uint32_t who; uint8_t kind; int ready; } th[PS3_TPROF_THREADS];
} ps3_tprof_state;

PS3_FT_SHARED ps3_tprof_state g_ps3_tprof = { 0 };

/* One sample of a stopped thread. Reads only its registers and its own
 * stack, bounded to the stack's range. */
static inline int ps3_tprof_take(int k, ps3_tprof_rec* r)
{
    ps3_tprof_state* s = &g_ps3_tprof;
    const mach_port_t port = s->th[k].port;
    if (thread_suspend(port) != KERN_SUCCESS) return 0;
    arm_thread_state64_t st;
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    unsigned d = 0;
    if (thread_get_state(port, ARM_THREAD_STATE64, (thread_state_t)&st, &count) == KERN_SUCCESS) {
        r->pc[d++] = (uintptr_t)__darwin_arm_thread_state64_get_pc(st);
        r->pc[d++] = (uintptr_t)__darwin_arm_thread_state64_get_lr(st);   /* a frameless leaf's caller */
        uintptr_t fp = (uintptr_t)__darwin_arm_thread_state64_get_fp(st);
        const uintptr_t lo = s->th[k].lo, hi = s->th[k].hi;
        while (d < PS3_TPROF_DEPTH && fp >= lo && fp + 16 <= hi && !(fp & 7u)) {
            const uintptr_t next = ((const uintptr_t*)fp)[0];
            const uintptr_t ret = ((const uintptr_t*)fp)[1] & 0x0000FFFFFFFFFFFFull;   /* strip a PAC */
            if (!ret) break;
            r->pc[d++] = ret;
            if (next <= fp) break;
            fp = next;
        }
    }
    thread_resume(port);
    r->depth = (uint8_t)d;
    r->who = s->th[k].who;
    r->kind = s->th[k].kind;
    r->t = ps3_ft_now();
    return d != 0;
}

static inline void ps3_tprof_write(const char* out, uint32_t n)
{
    FILE* f = fopen(out, "w");
    if (!f) { fprintf(stderr, "[tprof] cannot write %s\n", out); return; }
    const intptr_t slide = _dyld_get_image_vmaddr_slide(0);
    /* The main image's text range is written as offsets without asking
     * dladdr, and the few other addresses go through a small cache: one
     * dladdr per frame made a 40k-sample dump take minutes. */
    const struct mach_header_64* mh = (const struct mach_header_64*)_dyld_get_image_header(0);
    uintptr_t text_size = 0;   /* the main image's __TEXT, from its load commands */
    {
        const struct load_command* lc = (const struct load_command*)(mh + 1);
        for (uint32_t i = 0; mh && i < mh->ncmds; i++) {
            if (lc->cmd == LC_SEGMENT_64 &&
                !strncmp(((const struct segment_command_64*)lc)->segname, "__TEXT", 16)) {
                text_size = (uintptr_t)((const struct segment_command_64*)lc)->vmsize;
                break;
            }
            lc = (const struct load_command*)((const char*)lc + lc->cmdsize);
        }
    }
    const uintptr_t self_lo = (uintptr_t)mh;
    const uintptr_t self_hi = self_lo + text_size;
    enum { CACHE = 4096 };
    static struct { uintptr_t pc; const char* name; } cache[CACHE];
    const uint64_t base = g_ps3_ft.base;
    fprintf(f, "# tprof samples %u slide 0x%llx ftrace_base %d image %s\n", n,
            (unsigned long long)slide, base != 0, _dyld_get_image_name(0));
    for (uint32_t i = 0; i < n; i++) {
        const ps3_tprof_rec* r = &g_ps3_tprof.buf[i];
        if (!r->depth) continue;
        fprintf(f, "%c %u %.3f", r->kind, r->who, (double)(r->t - base) / 1e3);
        for (unsigned d = 0; d < r->depth; d++) {
            const uintptr_t pc = r->pc[d];
            const char* sym = NULL;
            if (pc < self_lo || pc >= self_hi) {
                const unsigned h = (unsigned)((pc >> 2) * 0x9E3779B1u) % CACHE;
                if (cache[h].pc == pc) sym = cache[h].name;
                else {
                    Dl_info info;
                    sym = dladdr((const void*)pc, &info) && info.dli_sname ? info.dli_sname : NULL;
                    cache[h].pc = pc; cache[h].name = sym;
                }
            }
            if (sym) fprintf(f, " @%s", sym);
            else fprintf(f, " %llx", (unsigned long long)(pc - (uintptr_t)slide));
        }
        fputc('\n', f);
    }
    fclose(f);
    fprintf(stderr, "[tprof] %u samples -> %s\n", n, out);
}

static inline void* ps3_tprof_sampler(void* arg)
{
    (void)arg;
    ps3_tprof_state* s = &g_ps3_tprof;
    const char* out = getenv("PS3_TPROF");
    const char* trig = getenv("PS3_TPROF_TRIGGER");
    const char* se = getenv("PS3_TPROF_SECONDS");
    const char* ie = getenv("PS3_TPROF_INTERVAL_US");
    const double secs = se && atof(se) > 0 ? atof(se) : 10.0;
    const long iv_us = ie && atol(ie) > 0 ? atol(ie) : 1000;
    struct timespec poll = {0, 100000000}, tick = {iv_us / 1000000, (iv_us % 1000000) * 1000};
    while (trig && *trig && access(trig, F_OK) != 0) nanosleep(&poll, NULL);
    const uint64_t end = ps3_ft_now() + (uint64_t)(secs * 1e9);
    fprintf(stderr, "[tprof] sampling %d threads every %ld us for %.1f s\n",
            __atomic_load_n(&s->nthreads, __ATOMIC_ACQUIRE), iv_us, secs);
    uint32_t n = 0;
    while (ps3_ft_now() < end && n < s->cap) {
        const int nt = __atomic_load_n(&s->nthreads, __ATOMIC_ACQUIRE);
        for (int k = 0; k < nt && k < PS3_TPROF_THREADS && n < s->cap; k++) {
            /* Claim the slot so ps3_tprof_thread_exit waits for this sample:
             * the thread cannot end, and its port and stack cannot go away,
             * while it is suspended or its stack is being read. */
            int ready = 1;
            if (!__atomic_compare_exchange_n(&s->th[k].ready, &ready, 2, 0,
                                             __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) continue;
            if (ps3_tprof_take(k, &s->buf[n])) n++;
            __atomic_store_n(&s->th[k].ready, 1, __ATOMIC_RELEASE);
        }
        nanosleep(&tick, NULL);
    }
    __atomic_store_n(&s->n, n, __ATOMIC_RELEASE);
    ps3_tprof_write(out, n);
    __atomic_store_n(&s->on, 3, __ATOMIC_RELEASE);
    return NULL;
}

static inline int ps3_tprof_init(void)
{
    ps3_tprof_state* s = &g_ps3_tprof;
    int expect = 0;
    if (!__atomic_compare_exchange_n(&s->on, &expect, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        while (expect == 1) expect = __atomic_load_n(&s->on, __ATOMIC_ACQUIRE);
        return expect == 2;
    }
    int on = 3;
    const char* out = getenv("PS3_TPROF");
    if (out && *out) {
        const char* c = getenv("PS3_TPROF_CAP");
        s->cap = c && atol(c) > 0 ? (uint32_t)atol(c) : 400000u;
        s->buf = (ps3_tprof_rec*)calloc(s->cap, sizeof(ps3_tprof_rec));
        if (s->buf) {
            /* Armed before the sampler starts, so it never sees "unknown". */
            __atomic_store_n(&s->on, 2, __ATOMIC_RELEASE);
            pthread_t t;
            if (!pthread_create(&t, NULL, ps3_tprof_sampler, NULL)) { pthread_detach(t); on = 2; }
        }
        if (on != 2) fprintf(stderr, "[tprof] could not start; profile disabled\n");
    }
    __atomic_store_n(&s->on, on, __ATOMIC_RELEASE);
    return on == 2;
}

/* Add the calling thread to the sampled set: kind 'S' or 'P', who its id.
 * A registered thread calls ps3_tprof_thread_exit() before it ends. */
static inline void ps3_tprof_thread(char kind, uint32_t who)
{
    ps3_tprof_state* s = &g_ps3_tprof;
    int on = __atomic_load_n(&s->on, __ATOMIC_ACQUIRE);
    if (__builtin_expect(on == 3, 1)) return;
    if (on != 2 && !ps3_tprof_init()) return;
    if (kind == 'P') {
        const char* ppu = getenv("PS3_TPROF_PPU");
        if (!ppu || strcmp(ppu, "1")) return;
    }
    const pthread_t self = pthread_self();
    /* Each header copy has its own statics, so slots are claimed atomically
     * from the shared state rather than under a lock. */
    const int k = __atomic_fetch_add(&s->nthreads, 1, __ATOMIC_ACQ_REL);
    if (k >= PS3_TPROF_THREADS) return;
    s->th[k].port = pthread_mach_thread_np(self);
    s->th[k].hi = (uintptr_t)pthread_get_stackaddr_np(self);
    s->th[k].lo = s->th[k].hi - pthread_get_stacksize_np(self);
    s->th[k].who = who;
    s->th[k].kind = (uint8_t)kind;
    __atomic_store_n(&s->th[k].ready, 1, __ATOMIC_RELEASE);
}

/* Remove the calling thread before it ends; the sampler skips its slot. A
 * sample in progress is waited out (the sampler holds the slot at 2), so the
 * thread is never suspended or its stack read after it has gone. */
static inline void ps3_tprof_thread_exit(void)
{
    ps3_tprof_state* s = &g_ps3_tprof;
    if (__builtin_expect(__atomic_load_n(&s->on, __ATOMIC_ACQUIRE) != 2, 1)) return;
    const mach_port_t self = pthread_mach_thread_np(pthread_self());
    const int nt = __atomic_load_n(&s->nthreads, __ATOMIC_ACQUIRE);
    for (int k = 0; k < nt && k < PS3_TPROF_THREADS; k++)
        if (s->th[k].port == self) {
            for (;;) {
                int ready = 1;
                if (__atomic_compare_exchange_n(&s->th[k].ready, &ready, 0, 0,
                                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) || ready == 0) break;
                sched_yield();   /* 2: the sampler is reading this thread */
            }
        }
}

#ifdef __cplusplus
}
#endif

#else
static inline void ps3_tprof_thread(char kind, uint32_t who) { (void)kind; (void)who; }
static inline void ps3_tprof_thread_exit(void) {}
#endif

#endif
