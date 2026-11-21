/*
 * ps3recomp - Apple AArch64 host fiber backend
 *
 * Stacks are mmap'd with an inaccessible guard page below them, so an overflow
 * faults at the edge instead of walking into whatever precedes the mapping.
 * Broken lifecycle invariants cannot be recovered from -- switching into a
 * frame the caller did not intend corrupts the runner -- so each one names
 * itself on stderr and aborts. The sanitizer fiber handoffs are annotated when
 * the file is compiled into an ASan or TSan build.
 */
#include "host_fiber.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
#define HF_ASAN 1
#include <sanitizer/asan_interface.h>
#include <sanitizer/common_interface_defs.h>
#else
#define HF_ASAN 0
#endif
#if __has_feature(thread_sanitizer) || defined(__SANITIZE_THREAD__)
#define HF_TSAN 1
#include <sanitizer/tsan_interface.h>
#else
#define HF_TSAN 0
#endif

extern void ps3_host_fiber_trampoline(void);
/* No stop facility in this tree: name the invariant, then fail closed. */
static _Noreturn __attribute__((cold, noinline))
void broke(uint32_t ea, const char* op, const char* reason, int err)
{
    fprintf(stderr,
            "[host_fiber] backend=aarch64 op=%s reason=%s ea=0x%X "
            "error=%u error_domain=%s\n",
            op, reason, ea, (unsigned)err, err ? "posix" : "none");
    abort();
}
__attribute__((visibility("hidden"))) __attribute__((noreturn))
void ps3_host_fiber_start(ps3_host_fiber* f)
{
#if HF_ASAN
    __sanitizer_finish_switch_fiber(NULL, &f->runner_bottom, &f->runner_size);
#endif
    f->entry(f->arg);
    broke(f->ea, "start", "entry-returned", 0);
}
int ps3_host_fiber_create(ps3_host_fiber* f, size_t stack_size,
                          void (*entry)(void*), void* arg, uint32_t ea)
{
    const size_t page = (size_t)getpagesize();
    if (f->map) broke(ea, "create", "state", 0);
    if (!entry || stack_size < page || stack_size > SIZE_MAX / 2)
        broke(ea, "create", "size", 0);
    const size_t stack = (stack_size + page - 1) & ~(page - 1);
    unsigned char* map = mmap(NULL, stack + page, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANON, -1, 0);
    if (map == MAP_FAILED) {
        if (errno == ENOMEM) return -1;
        broke(ea, "create", "mmap", errno);
    }
    if (mprotect(map, page, PROT_NONE) != 0) {
        /* Splitting the mapping can exhaust map resources exactly as mmap can. */
        const int err = errno;
        munmap(map, stack + page);
        if (err == ENOMEM) { errno = ENOMEM; return -1; }
        broke(ea, "create", "mprotect", err);
    }
    memset(f, 0, sizeof *f);
    f->map = map; f->map_size = stack + page; f->guard = page;
    f->entry = entry; f->arg = arg; f->ea = ea;
    uint64_t* record = (uint64_t*)(map + f->map_size) - 2;
    record[0] = record[1] = 0;
    f->ctx.sp = f->ctx.fp = (uint64_t)(uintptr_t)record;
    f->ctx.lr = (uint64_t)(uintptr_t)ps3_host_fiber_trampoline;
    f->ctx.x19_x28[0] = (uint64_t)(uintptr_t)f;
#if HF_ASAN
    /* Reused mappings may retain shadow from abandoned frames. */
    __asan_unpoison_memory_region(map, f->map_size);
#endif
#if HF_TSAN
    f->tsan = __tsan_create_fiber(0);
#endif
    f->state = PS3_HOST_FIBER_NEW;
    return 0;
}
void ps3_host_fiber_resume(ps3_host_runner* r, ps3_host_fiber* f)
{
    if (__builtin_expect(f->state != PS3_HOST_FIBER_NEW &&
                         f->state != PS3_HOST_FIBER_SUSPENDED, 0))
        broke(f->ea, "resume", "state", 0);
    /* r->ctx holds the runner's resume point: a second resume through it would
     * overwrite that, and the later yield or exit would land in the wrong frame. */
    if (__builtin_expect(r->active != NULL, 0)) broke(f->ea, "resume", "nested", 0);
    const pthread_t self = pthread_self();
    if (f->state == PS3_HOST_FIBER_NEW) f->owner = self;
    else if (__builtin_expect(!pthread_equal(f->owner, self), 0))
        broke(f->ea, "resume", "foreign-thread", 0);
    f->runner = r; r->active = f; f->state = PS3_HOST_FIBER_RUNNING;
#if HF_ASAN
    __sanitizer_start_switch_fiber(&r->asan_fake, f->map + f->guard,
                                    f->map_size - f->guard);
#endif
#if HF_TSAN
    r->tsan_self = __tsan_get_current_fiber();
    __tsan_switch_to_fiber(f->tsan, 0);
#endif
    ps3_host_ctx_switch(&r->ctx, &f->ctx);
    r->active = NULL;
#if HF_ASAN
    __sanitizer_finish_switch_fiber(r->asan_fake, NULL, NULL);
#endif
    if (__builtin_expect(f->state == PS3_HOST_FIBER_RUNNING, 0))
        broke(f->ea, "resume", "state", 0);
}
static inline void on_own_stack(const ps3_host_fiber* f, const char* op)
{
    /* The frame lies in another object than the mapping on a foreign stack, so
     * a pointer comparison would be undefined; compare the addresses. */
    const uintptr_t fp = (uintptr_t)__builtin_frame_address(0);
    const uintptr_t low = (uintptr_t)f->map + f->guard;
    const uintptr_t high = (uintptr_t)f->map + f->map_size;
    if (__builtin_expect(f->state != PS3_HOST_FIBER_RUNNING, 0))
        broke(f->ea, op, "state", 0);
    if (__builtin_expect(fp < low || fp >= high, 0))
        broke(f->ea, op, "foreign-stack", 0);
}
void ps3_host_fiber_yield(ps3_host_fiber* f)
{
    on_own_stack(f, "yield");
    ps3_host_runner* r = f->runner;
    f->state = PS3_HOST_FIBER_SUSPENDED;
#if HF_ASAN
    __sanitizer_start_switch_fiber(&f->asan_fake, f->runner_bottom, f->runner_size);
#endif
#if HF_TSAN
    __tsan_switch_to_fiber(r->tsan_self, 0);
#endif
    ps3_host_ctx_switch(&f->ctx, &r->ctx);
#if HF_ASAN
    __sanitizer_finish_switch_fiber(f->asan_fake, &f->runner_bottom, &f->runner_size);
#endif
}
void ps3_host_fiber_exit(ps3_host_fiber* f)
{
    on_own_stack(f, "exit");
    ps3_host_runner* r = f->runner;
    f->state = PS3_HOST_FIBER_EXITED;
#if HF_ASAN
    /* NULL discards the fake stack: these frames never return. */
    __sanitizer_start_switch_fiber(NULL, f->runner_bottom, f->runner_size);
#endif
#if HF_TSAN
    __tsan_switch_to_fiber(r->tsan_self, 0);
#endif
    ps3_host_ctx_switch(&f->ctx, &r->ctx);
    broke(f->ea, "exit", "resumed-after-exit", 0);
}
void ps3_host_fiber_destroy(ps3_host_fiber* f)
{
    if (!f->map) return;
    if (f->state != PS3_HOST_FIBER_NEW && f->state != PS3_HOST_FIBER_EXITED)
        broke(f->ea, "destroy", "state", 0);
#if HF_TSAN
    __tsan_destroy_fiber(f->tsan); f->tsan = NULL;
#endif
#if HF_ASAN
    __asan_unpoison_memory_region(f->map, f->map_size);
#endif
    if (munmap(f->map, f->map_size) != 0) broke(f->ea, "destroy", "munmap", errno);
    /* A destroyed record has no stack: leave NEW and EXITED so a later resume
     * stops by name instead of switching into the unmapped stack. */
    f->map = NULL; f->map_size = 0; f->state = 0;
}
