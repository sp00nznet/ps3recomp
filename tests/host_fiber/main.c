/*
 * ps3recomp - tests/host_fiber/main.c
 *
 * The AArch64 switch on synthetic stacks with no runtime library: a two-sided
 * assembly ABI probe, stack layout and frame walking, lifecycle and thread
 * pinning, and every fail-closed check. The aborts are checked in a child
 * process, because aborting is the contract.
 */
#include "host_fiber.h"
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/resource.h>
#include <dlfcn.h>
#include <sys/mman.h>
#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
#define HF_TEST_ASAN 1
#endif
#if __has_feature(thread_sanitizer) || defined(__SANITIZE_THREAD__)
#define HF_TEST_TSAN 1
#include <sanitizer/tsan_interface.h>
#endif
#define STACK (4u * 1024u * 1024u)
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
/* The backend's own mprotect call binds to this definition (same executable),
 * so one split of a stack mapping can be made to fail. Any other call, and the
 * sanitizer runtimes' own, reach libc. */
static int hf_mprotect_errno;
static void* hf_mprotect_addr;
int mprotect(void* addr, size_t length, int prot)
{
    if (hf_mprotect_errno) {
        hf_mprotect_addr = addr; errno = hf_mprotect_errno; hf_mprotect_errno = 0;
        return -1;
    }
    static int (*real)(void*, size_t, int);
    if (!real) *(void**)&real = dlsym(RTLD_NEXT, "mprotect");
    return real(addr, length, prot);
}
ps3_host_ctx hf_abi_runner, hf_abi_runner2, hf_abi_peer, hf_abi_peer2, hf_abi_a, hf_abi_b, hf_abi_c;
ps3_host_ctx hf_abi_got_a, hf_abi_got_b, hf_abi_got_c;
uintptr_t hf_abi_home;
extern void hf_abi_run(void);
extern void hf_abi_peer_entry(void);
static uint64_t frames[3][2];
static void pattern(ps3_host_ctx* p, uint64_t seed, unsigned frame)
{
    for (unsigned i = 0; i < 10; ++i) p->x19_x28[i] = seed + i;
    for (unsigned i = 0; i < 8; ++i) p->d8_d15[i] = seed + 0x100 + i;
    p->fp = (uintptr_t)frames[frame];
}
static void same(const ps3_host_ctx* a, const ps3_host_ctx* b)
{
    CHECK(!memcmp(a->x19_x28, b->x19_x28, sizeof a->x19_x28));
    CHECK(a->fp == b->fp && a->sp == b->sp && !(b->sp & 15));
    CHECK(!memcmp(a->d8_d15, b->d8_d15, sizeof a->d8_d15));
}
static void abi(void)
{
    /* Assembly alone runs on the alternate stack, so no sanitizer C frames
     * exist there. It records registers before any C prologue can hide them. */
    void* stack = malloc(STACK); CHECK(stack);
    hf_abi_peer.sp = ((uintptr_t)stack + STACK) & ~(uintptr_t)15;
    hf_abi_peer.lr = (uintptr_t)hf_abi_peer_entry;
    pattern(&hf_abi_a, 0xA123456780000000ull, 0);
    pattern(&hf_abi_b, 0xB234567890000000ull, 1);
    pattern(&hf_abi_c, 0xC3456789A0000000ull, 2);
    hf_abi_b.sp = hf_abi_peer.sp;
    hf_abi_run();
    same(&hf_abi_a, &hf_abi_got_a); same(&hf_abi_b, &hf_abi_got_b);
    same(&hf_abi_c, &hf_abi_got_c);
    /* x30 is the only register whose loss shows up as a crash. Each switch saved
     * its own return address, so the value in force after the matching return
     * must equal what that switch recorded; a lost x30 fails here by name. */
    CHECK(hf_abi_runner.lr && hf_abi_runner2.lr && hf_abi_runner.lr != hf_abi_runner2.lr);
    CHECK(hf_abi_got_a.lr == hf_abi_runner.lr);
    CHECK(hf_abi_got_c.lr == hf_abi_runner2.lr);
    CHECK(hf_abi_peer.lr && hf_abi_peer2.lr && hf_abi_got_b.lr == hf_abi_peer.lr);
    free(stack);
}
typedef struct Case {
    ps3_host_fiber f; ps3_host_runner r; unsigned step, id;
    uintptr_t runner_frame; void* runner_fiber;
} Case;
static void create(Case* c, void (*entry)(void*))
{ CHECK(ps3_host_fiber_create(&c->f, STACK, entry, c, 0x1000 + c->id) == 0); }
static void lifecycle_entry(void* arg)
{
    Case* c = arg; CHECK(c->step == 0 && c->f.state == PS3_HOST_FIBER_RUNNING);
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(c->step == 2 * i); ++c->step;
        volatile unsigned sentinel = 0xC001 + i;
        ps3_host_fiber_yield(&c->f);
        CHECK(sentinel == 0xC001 + i && c->step == 2 * i + 2);
    }
    ++c->step; ps3_host_fiber_exit(&c->f);
}
static void lifecycle(void)
{
    for (unsigned round = 0; round < 10000; ++round) {
        Case c = {0}; create(&c, lifecycle_entry);
        CHECK(c.f.state == PS3_HOST_FIBER_NEW);
        for (unsigned i = 0; i < 3; ++i) {
            ps3_host_fiber_resume(&c.r, &c.f);
            CHECK(c.step == 2 * i + 1 && c.f.state == PS3_HOST_FIBER_SUSPENDED);
            ++c.step;
        }
        ps3_host_fiber_resume(&c.r, &c.f);
        CHECK(c.step == 7 && c.f.state == PS3_HOST_FIBER_EXITED);
        ps3_host_fiber_destroy(&c.f); ps3_host_fiber_destroy(&c.f);
    }
}
static unsigned sequence;
static void many_entry(void* arg)
{
    Case* c = arg;
    for (unsigned i = 0; i < 100; ++i) {
        CHECK(sequence++ == i * 64 + c->id);
        ps3_host_fiber_yield(&c->f);
    }
    ps3_host_fiber_exit(&c->f);
}
static void many(void)
{
    Case c[64] = {0};
    for (unsigned i = 0; i < 64; ++i) { c[i].id = i; create(&c[i], many_entry); }
    for (unsigned j = 0; j <= 100; ++j)
        for (unsigned i = 0; i < 64; ++i) ps3_host_fiber_resume(&c[i].r, &c[i].f);
    CHECK(sequence == 6400);
    for (unsigned i = 0; i < 64; ++i) ps3_host_fiber_destroy(&c[i].f);
}
static void frame_entry(void* arg)
{
    Case* c = arg; uintptr_t sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    uintptr_t low = (uintptr_t)c->f.map + c->f.guard;
    uintptr_t high = (uintptr_t)c->f.map + c->f.map_size;
    CHECK(!(sp & 15) && sp >= low && sp < high);
    uintptr_t* fp = __builtin_frame_address(0);
    unsigned depth = 0;
    while (fp[0]) {
        CHECK(++depth < 100 && (uintptr_t)fp >= low && (uintptr_t)fp + 16 <= high);
        CHECK(fp[0] > (uintptr_t)fp && fp[0] + 16 <= high);
        fp = (uintptr_t*)fp[0];
    }
    CHECK((uintptr_t)fp == high - 16 && fp[1] == 0);
    ps3_host_fiber_exit(&c->f);
}
__attribute__((noinline)) static unsigned deep_frames(Case* c, unsigned depth)
{
    volatile unsigned char bytes[16384];
    for (unsigned i = 0; i < sizeof bytes; ++i) bytes[i] = (unsigned char)(i + depth);
    if (depth) CHECK(deep_frames(c, depth - 1) == depth - 1);
    else ps3_host_fiber_yield(&c->f);
    for (unsigned i = 0; i < sizeof bytes; ++i) CHECK(bytes[i] == (unsigned char)(i + depth));
    return depth;
}
static void deep_entry(void* arg)
{ Case* c = arg; CHECK(deep_frames(c, 223) == 223); ps3_host_fiber_exit(&c->f); }
static void exit_entry(void* arg) { Case* c = arg; ++c->step; ps3_host_fiber_exit(&c->f); }
static void yield_entry(void* arg) { Case* c = arg; ps3_host_fiber_yield(&c->f); ps3_host_fiber_exit(&c->f); }
static void returned_entry(void* arg) { (void)arg; }
static void nested_entry(void* arg) { Case* c = arg; ps3_host_fiber_resume(&c->r, &c->f); }
/* A second fiber, resumed from inside the first. */
static Case nested_other;
static void nested_runner_entry(void* arg)
{ Case* c = arg; ps3_host_fiber_resume(&c->r, &nested_other.f); }
static void nested_distinct_entry(void* arg)
{
    Case* c = arg;
    ps3_host_fiber_resume(&nested_other.r, &nested_other.f);
    CHECK(nested_other.f.state == PS3_HOST_FIBER_EXITED);
    ++c->step; ps3_host_fiber_exit(&c->f);
}
/* Nesting is legal while each level has its own runner. */
static void nested_distinct(void)
{
    Case c = {0}; nested_other = (Case){0}; nested_other.id = 1;
    create(&c, nested_distinct_entry); create(&nested_other, exit_entry);
    ps3_host_fiber_resume(&c.r, &c.f);
    CHECK(c.step == 1 && nested_other.step == 1 && c.f.state == PS3_HOST_FIBER_EXITED);
    ps3_host_fiber_destroy(&c.f); ps3_host_fiber_destroy(&nested_other.f);
}
static void* foreign(void* arg) { Case* c = arg; ps3_host_runner r = {0}; ps3_host_fiber_resume(&r, &c->f); return NULL; }
/* One row per stop: the expected machine line, field by field. The ea differs
 * per row, so two stops that share op and reason cannot satisfy each other's
 * test. A fiber made by the case carries ea 0x1000 + its id. */
typedef struct StopRow { const char* mode, *op, *reason; uint32_t ea; unsigned error; } StopRow;
static const StopRow stops[] = {
    {"stop_uncreated", "resume", "state", 0, 0},
    {"stop_size", "create", "size", 0x1001, 0},
    {"stop_entry_returned", "start", "entry-returned", 0x1002, 0},
    {"stop_resume_exited", "resume", "state", 0x1003, 0},
    {"stop_resume_running", "resume", "state", 0x1004, 0},
    {"stop_yield_foreign_stack", "yield", "foreign-stack", 0x1005, 0},
    {"stop_destroy_suspended", "destroy", "state", 0x1006, 0},
    {"stop_foreign_thread", "resume", "foreign-thread", 0x1007, 0},
    {"stop_resume_destroyed", "resume", "state", 0x1008, 0},
    {"stop_resume_nested", "resume", "nested", 0x100A, 0},
    {"stop_mprotect", "create", "mprotect", 0x1009, EACCES},
};
static const StopRow* stop_row(const char* mode)
{
    for (size_t i = 0; i < sizeof stops / sizeof *stops; ++i)
        if (!strcmp(stops[i].mode, mode)) return &stops[i];
    fprintf(stderr, "FAIL no stop row for %s\n", mode); exit(1);
}
static void stop_mode(const char* mode)
{
    const StopRow* row = stop_row(mode);
    Case c = {0}; c.id = row->ea ? row->ea - 0x1000 : 0;
    if (!strcmp(mode, "stop_uncreated")) ps3_host_fiber_resume(&c.r, &c.f);
    if (!strcmp(mode, "stop_size")) {
        (void)ps3_host_fiber_create(&c.f, 1, exit_entry, &c, row->ea); CHECK(0);
    }
    if (!strcmp(mode, "stop_mprotect")) {
        hf_mprotect_errno = EACCES;
        (void)ps3_host_fiber_create(&c.f, STACK, exit_entry, &c, row->ea); CHECK(0);
    }
    if (!strcmp(mode, "stop_resume_nested")) {
        nested_other.id = c.id; c.id = 0x20; create(&nested_other, exit_entry);
    }
    create(&c, !strcmp(mode, "stop_entry_returned") ? returned_entry :
               !strcmp(mode, "stop_resume_running") ? nested_entry :
               !strcmp(mode, "stop_resume_nested") ? nested_runner_entry :
               !strcmp(mode, "stop_resume_exited") ? exit_entry : yield_entry);
    if (!strcmp(mode, "stop_resume_destroyed")) {
        ps3_host_fiber_destroy(&c.f); ps3_host_fiber_resume(&c.r, &c.f);
    }
    ps3_host_fiber_resume(&c.r, &c.f);
    if (!strcmp(mode, "stop_resume_exited")) ps3_host_fiber_resume(&c.r, &c.f);
    if (!strcmp(mode, "stop_destroy_suspended")) ps3_host_fiber_destroy(&c.f);
    if (!strcmp(mode, "stop_yield_foreign_stack")) {
        c.f.state = PS3_HOST_FIBER_RUNNING; ps3_host_fiber_yield(&c.f);
    }
    if (!strcmp(mode, "stop_foreign_thread")) {
        pthread_attr_t attr; CHECK(!pthread_attr_init(&attr));
        CHECK(!pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED));
        pthread_t t; CHECK(!pthread_create(&t, &attr, foreign, &c));
        CHECK(!pthread_attr_destroy(&attr));
        for (;;) pause();
    }
    CHECK(0);
}
/* Runs this executable in `mode` with stderr captured; returns the wait status. */
static int run_child(const char* executable, const char* mode, char* output, size_t capacity)
{
    int pipes[2]; CHECK(!pipe(pipes));
    posix_spawn_file_actions_t actions; CHECK(!posix_spawn_file_actions_init(&actions));
    CHECK(!posix_spawn_file_actions_adddup2(&actions, pipes[1], STDERR_FILENO));
    CHECK(!posix_spawn_file_actions_addclose(&actions, pipes[0]));
    CHECK(!posix_spawn_file_actions_addclose(&actions, pipes[1]));
    extern char** environ;
    char* args[] = {(char*)executable, (char*)mode, NULL};
    pid_t pid; CHECK(!posix_spawn(&pid, executable, &actions, NULL, args, environ));
    CHECK(!posix_spawn_file_actions_destroy(&actions));
    close(pipes[1]); size_t used = 0; ssize_t n;
    char buffer[1024];
    while ((n = read(pipes[0], buffer, sizeof buffer)) > 0) {
        size_t keep = (size_t)n < capacity - 1 - used ? (size_t)n : capacity - 1 - used;
        memcpy(output + used, buffer, keep); used += keep;
    }
    close(pipes[0]); output[used] = 0;
    int status; CHECK(waitpid(pid, &status, 0) == pid);
    return status;
}
/* A stop test passes only by the stop: SIGABRT and exactly one named line that
 * equals the row. A wrong cause with the right reason cannot pass. The process
 * fails closed on these invariants, so the abort is the contract. */
static void expect_stop(const char* executable, const char* mode)
{
    const StopRow* row = stop_row(mode);
    char output[65536];
    const int status = run_child(executable, mode, output, sizeof output);
    fprintf(stderr, "%s child status=%d\n%s", mode, status, output);
    char want[256];
    snprintf(want, sizeof want, "[host_fiber] backend=aarch64 "
             "op=%s reason=%s ea=0x%X error=%u error_domain=%s\n", row->op, row->reason, row->ea,
             row->error, row->error ? "posix" : "none");
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    const char* line = strstr(output, "[host_fiber] ");
    CHECK(line && !strstr(line + 1, "[host_fiber] "));
    CHECK(!strcmp(line, want));
}
static void handoff_entry(void* arg)
{
    Case* c = arg;
    for (unsigned i = 0; i < 10000; ++i) {
        CHECK(c->step == 2 * i); ++c->step; ps3_host_fiber_yield(&c->f);
    }
    ps3_host_fiber_exit(&c->f);
}
static void handoff(void)
{
    Case c = {0}; create(&c, handoff_entry);
    for (unsigned i = 0; i < 10000; ++i) {
        ps3_host_fiber_resume(&c.r, &c.f); CHECK(c.step == 2 * i + 1); ++c.step;
    }
    ps3_host_fiber_resume(&c.r, &c.f); ps3_host_fiber_destroy(&c.f);
}
/* Removing an annotation leaves every switch working, so only the state the
 * sanitizer runtimes expose can tell a missing annotation from a present one.
 * __builtin_frame_address: under detect_stack_use_after_return a local may sit
 * in ASan's fake stack, outside the real runner stack the bounds describe. */
static void annot_check(const Case* c)
{
#ifdef HF_TEST_ASAN
    const uintptr_t lo = (uintptr_t)c->f.runner_bottom, hi = lo + c->f.runner_size;
    const uintptr_t mine = (uintptr_t)__builtin_frame_address(0);
    CHECK(lo && c->runner_frame >= lo && c->runner_frame < hi);
    CHECK(mine < lo || mine >= hi);
#endif
#ifdef HF_TEST_TSAN
    CHECK(c->f.tsan && __tsan_get_current_fiber() == c->f.tsan);
    CHECK(c->runner_fiber != c->f.tsan && c->r.tsan_self == c->runner_fiber);
#endif
    (void)c;
}
static void annot_entry(void* arg)
{
    Case* c = arg;
    for (unsigned i = 0; i < 3; ++i) { annot_check(c); ps3_host_fiber_yield(&c->f); }
    annot_check(c); ps3_host_fiber_exit(&c->f);
}
static void annot(void)
{
    Case c = {0}; create(&c, annot_entry);
    c.runner_frame = (uintptr_t)__builtin_frame_address(0);
#ifdef HF_TEST_TSAN
    c.runner_fiber = __tsan_get_current_fiber();
#endif
    for (unsigned i = 0; i < 4; ++i) {
        ps3_host_fiber_resume(&c.r, &c.f);
#ifdef HF_TEST_TSAN
        CHECK(__tsan_get_current_fiber() == c.runner_fiber);
#endif
    }
    CHECK(c.f.state == PS3_HOST_FIBER_EXITED); ps3_host_fiber_destroy(&c.f);
}
/* ASan drops a no-return unwind it cannot place on the current stack and
 * prints a warning; a fiber whose bounds are right prints nothing. The test
 * fails on that text, and exits 0 from the fiber otherwise. */
static void noret_entry(void* arg)
{ (void)arg; puts("PASS noret"); fflush(stdout); exit(0); }
static void noret(void)
{ Case c = {0}; create(&c, noret_entry); ps3_host_fiber_resume(&c.r, &c.f); CHECK(0); }
/* An un-destroyed __tsan_create_fiber state costs tens of KiB, and nothing
 * else observes it: the peak-RSS growth over many rounds is the signal. */
static void tsan_destroy(void)
{
    struct rusage before, after;
    CHECK(!getrusage(RUSAGE_SELF, &before));
    for (unsigned round = 0; round < 6000; ++round) {
        Case c = {0}; create(&c, exit_entry);
        ps3_host_fiber_resume(&c.r, &c.f); ps3_host_fiber_destroy(&c.f);
    }
    CHECK(!getrusage(RUSAGE_SELF, &after));
    const long grown = after.ru_maxrss - before.ru_maxrss; /* bytes on Apple */
    fprintf(stderr, "peak RSS grew %ld bytes\n", grown);
    CHECK(grown < 48l * 1024 * 1024);
}
static void guard_entry(void* arg)
{
    Case* c = arg;
    (void)deep_frames(c, 1024);
    CHECK(0);
}
static void guard(const char* executable)
{
    char output[65536];
    const int status = run_child(executable, "guard_child", output, sizeof output);
    fprintf(stderr, "guard child status=%d\n%s", status, output);
    CHECK((WIFSIGNALED(status) && (WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGBUS)) ||
          ((strstr(output, "AddressSanitizer: stack-overflow") ||
            strstr(output, "ThreadSanitizer: stack-overflow")) &&
           ((WIFEXITED(status) && WEXITSTATUS(status) != 0) ||
            (WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT))));
}
int main(int argc, char** argv)
{
    CHECK(argc == 2); const char* mode = argv[1];
    if (!strncmp(mode, "expect_", 7)) expect_stop(argv[0], mode + 7);
    else if (!strncmp(mode, "stop_", 5)) stop_mode(mode);
    else if (!strcmp(mode, "abi")) abi();
    else if (!strcmp(mode, "lifecycle")) lifecycle();
    else if (!strcmp(mode, "many")) many();
    else if (!strcmp(mode, "tsan_handoff")) handoff();
    else if (!strcmp(mode, "annot")) annot();
    else if (!strcmp(mode, "noret")) noret();
    else if (!strcmp(mode, "tsan_destroy")) tsan_destroy();
    else if (!strcmp(mode, "nested_distinct")) nested_distinct();
    else if (!strcmp(mode, "frame") || !strcmp(mode, "deep")) {
        Case c = {0}; create(&c, !strcmp(mode, "frame") ? frame_entry : deep_entry);
        ps3_host_fiber_resume(&c.r, &c.f);
        if (c.f.state == PS3_HOST_FIBER_SUSPENDED) ps3_host_fiber_resume(&c.r, &c.f);
        CHECK(c.f.state == PS3_HOST_FIBER_EXITED); ps3_host_fiber_destroy(&c.f);
    } else if (!strcmp(mode, "enomem")) {
        Case c = {0}; ps3_host_fiber before = c.f; errno = 0;
        CHECK(ps3_host_fiber_create(&c.f, SIZE_MAX / 4, exit_entry, &c, 0) == -1);
        CHECK(errno == ENOMEM && !memcmp(&before, &c.f, sizeof before));
    } else if (!strcmp(mode, "enomem_mprotect")) {
        /* The mapping must be gone too: a leak would be invisible otherwise. */
        Case c = {0}; ps3_host_fiber before = c.f; errno = 0; hf_mprotect_errno = ENOMEM;
        CHECK(ps3_host_fiber_create(&c.f, STACK, exit_entry, &c, 0) == -1);
        CHECK(errno == ENOMEM && !memcmp(&before, &c.f, sizeof before));
        /* msync fails with ENOMEM on an unmapped page; mincore does not on Apple. */
        CHECK(hf_mprotect_addr && msync(hf_mprotect_addr, 1, MS_ASYNC) == -1 && errno == ENOMEM);
    } else if (!strcmp(mode, "guard")) guard(argv[0]);
    else if (!strcmp(mode, "guard_child")) {
        Case c = {0}; create(&c, guard_entry); ps3_host_fiber_resume(&c.r, &c.f);
        CHECK(0);
    }
    else CHECK(0);
    printf("PASS %s\n", mode); return 0;
}
