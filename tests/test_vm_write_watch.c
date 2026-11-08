/* The guest write watch (runtime/memory/vm_watch.h) on synthetic guest memory:
 * a user-space write to a watched page is recorded and proceeds; an announced
 * kernel write proceeds; an unannounced one fails with an access error (which
 * is why the inventory of OS writers announces itself); a fault on a page the
 * watch does not own reaches the previous handler; runtime protection changes
 * release (or pin) watched pages; concurrent writers to one page all
 * complete. Default off: without PS3RECOMP_VM_WRITE_WATCH=1 nothing is
 * watched. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../runtime/memory/vm.h"
#include "../runtime/memory/vm_watch.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <signal.h>
#ifdef _WIN32
  #include <windows.h>
  #include <process.h>
#else
  #include <errno.h>
  #include <pthread.h>
  #include <setjmp.h>
  #include <signal.h>
  #include <sys/mman.h>
  #include <unistd.h>
#endif

uint8_t* vm_base;
uint32_t ppu_vm_size;

static uint64_t page;
#define REGION (UINT64_C(64) << 20) /* guest [0, 64 MB) */

static void set_env(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

static vm_watch_stats_t stats(void)
{
    vm_watch_stats_t s;
    vm_watch_stats(&s);
    return s;
}

/* A kernel write into guest memory: 0 on success, 1 on an access error. */
static int kernel_write(uint32_t ea, size_t n)
{
#ifdef _WIN32
    char path[MAX_PATH], dir[MAX_PATH];
    GetTempPathA(sizeof dir, dir);
    GetTempFileNameA(dir, "vmw", 0, path);
    HANDLE w = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    char src[64];
    memset(src, 0x5a, sizeof src);
    DWORD done = 0;
    WriteFile(w, src, (DWORD)n, &done, NULL);
    CloseHandle(w);
    HANDLE r = CreateFileA(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_FLAG_DELETE_ON_CLOSE, NULL);
    BOOL ok = ReadFile(r, vm_base + ea, (DWORD)n, &done, NULL);
    DWORD err = GetLastError();
    CloseHandle(r);
    return !ok && err == ERROR_NOACCESS;
#else
    int fds[2];
    assert(pipe(fds) == 0);
    char src[64];
    memset(src, 0x5a, sizeof src);
    assert(write(fds[1], src, n) == (ssize_t)n);
    ssize_t got = read(fds[0], vm_base + ea, n);
    int access = got < 0 && errno == EFAULT;
    close(fds[0]);
    close(fds[1]);
    return access;
#endif
}

#ifndef _WIN32
static sigjmp_buf s_jump;
static volatile sig_atomic_t s_previous_ran;
static void previous(int sig, siginfo_t* si, void* uctx)
{
    (void)sig;
    (void)si;
    (void)uctx;
    s_previous_ran = 1;
    siglongjmp(s_jump, 1);
}

struct writer {
    volatile uint8_t* at;
    uint8_t value;
};
static void* write_many(void* arg)
{
    struct writer* w = (struct writer*)arg;
    for (int i = 0; i < 1000; ++i) w->at[i % 64] = w->value;
    return NULL;
}
#endif

static void aborted(int sig)
{
    (void)sig;
    printf("vm_write_watch strict: aborted as expected\n");
    fflush(stdout);
    _exit(3);
}

int main(int argc, char** argv)
{
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    page = si.dwPageSize;
    vm_base = (uint8_t*)VirtualAlloc(NULL, (SIZE_T)REGION, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(vm_base);
#else
    page = (uint64_t)sysconf(_SC_PAGESIZE);
    vm_base = (uint8_t*)mmap(NULL, (size_t)REGION, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(vm_base != MAP_FAILED);
#endif
    ppu_vm_size = (uint32_t)REGION;

#ifndef _WIN32
    /* A handler installed before the watch: it must still receive faults the
     * watch does not own. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = previous;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
#endif

    /* Default off (run as `test_vm_write_watch off`, without the option). */
    if (argc > 1 && !strcmp(argv[1], "off")) {
        assert(!getenv("PS3RECOMP_VM_WRITE_WATCH"));
        assert(!vm_watch_enable() && !vm_watch_active());
        assert(vm_watch(0x100000, 16) == 0);
        /* Nothing is installed: the hooks stay empty, so announcing writes
         * and protection changes is a no-op. */
        assert(!ps3_vm_watch_hook.forget && !ps3_vm_watch_hook.forget_host &&
               !ps3_vm_watch_hook.note_write && !ps3_vm_watch_hook.note_host_write &&
               !ps3_vm_watch_hook.note_write_check);
        vm_note_write(0x100000, 16);
        vm_note_write_check(1, "off test");
        vm_base[0x100000] = 1; /* nothing protected */
        printf("vm_write_watch off: inactive\n");
        return 0;
    }
    set_env("PS3RECOMP_VM_WRITE_WATCH", "1");
    /* Strict (`test_vm_write_watch strict`): an OS write that failed with an
     * access error is fatal; this mode is expected to abort. */
    if (argc > 1 && !strcmp(argv[1], "strict")) {
        set_env("PS3RECOMP_VM_WRITE_WATCH_STRICT", "1");
        /* The abort becomes an ordinary failing exit for the test runner. */
        signal(SIGABRT, aborted);
        assert(vm_watch_enable());
        vm_note_write_check(0, "strict test: success");
        vm_note_write_check(1, "strict test: unannounced writer");
        printf("vm_write_watch strict: did not abort\n");
        return 0;
    }
    assert(vm_watch_enable() && vm_watch_active());
    const uint32_t a = 0x100000, b = (uint32_t)(0x100000 + 4 * page);
    memset(vm_base + a, 1, 64);

    /* 1. A user-space write is recorded and proceeds. */
    uint64_t e = vm_watch(a, 64);
    assert(e && vm_clean_since(a, 64, e));
    assert(vm_base[a] == 1); /* reads never fault */
    vm_base[a + 1] = 7;
    assert(vm_base[a + 1] == 7);
    assert(!vm_clean_since(a, 64, e));
    assert(stats().faults == 1);
    /* Watching again gives a later epoch, clean until the next write. */
    uint64_t e2 = vm_watch(a, 64);
    assert(e2 > e && vm_clean_since(a, 64, e2) && !vm_clean_since(a, 64, e));

    /* 2. An unannounced kernel write fails with an access error; announced,
     * it succeeds and dirties the page. */
    uint64_t eb = vm_watch(b, 64);
    assert(eb);
    assert(kernel_write(b, 32) == 1);
    assert(vm_clean_since(b, 64, eb));
    vm_note_write(b, 32);
    assert(kernel_write(b, 32) == 0 && vm_base[b] == 0x5a);
    assert(!vm_clean_since(b, 64, eb));
    assert(stats().notes == 1);
    vm_note_write_check(0, "test"); /* not strict, and no failure: no-op */

    /* 3. Runtime protection changes: a commit releases the page (writable,
     * dirty, no fault); a read-only protection pins it (never watched). */
    const uint32_t c = (uint32_t)(0x100000 + 8 * page);
    uint64_t ec = vm_watch(c, 16);
    assert(ec);
    assert(vm_commit(c, 16) == CELL_OK);
    assert(!vm_clean_since(c, 16, ec));
    const uint64_t faults = stats().faults;
    vm_base[c] = 3;
    assert(stats().faults == faults && stats().forgotten >= 1);
    assert(vm_protect(c, 16, 1, 0, 0) == CELL_OK);
    const uint64_t refused = stats().refused;
    assert(vm_watch(c, 16) == 0);
    assert(stats().refused == refused + 1);
    /* An SDK component protecting a page itself pins it the same way. */
    const uint32_t g = (uint32_t)(0x100000 + 20 * page);
    assert(vm_watch(g, 16));
    vm_watch_forget_host(vm_base + g, 16, 0);
    assert(vm_watch(g, 16) == 0 && stats().refused == refused + 2);
    vm_base[g] = 4; /* released (writable) when pinned */
    assert(vm_commit(c, 16) == CELL_OK);
    assert(vm_watch(c, 16) != 0);

#ifndef _WIN32
    /* 4. A fault the watch does not own reaches the previous handler. */
    const uint32_t d = (uint32_t)(0x100000 + 12 * page);
    assert(mprotect(vm_base + d, (size_t)page, PROT_READ) == 0);
    s_previous_ran = 0;
    if (sigsetjmp(s_jump, 1) == 0) {
        vm_base[d] = 9;
        assert(!"the write must fault");
    }
    assert(s_previous_ran);
    assert(mprotect(vm_base + d, (size_t)page, PROT_READ | PROT_WRITE) == 0);

    /* 5. Many threads writing one watched page all complete. */
    const uint32_t f = (uint32_t)(0x100000 + 16 * page);
    for (int round = 0; round < 20; ++round) {
        uint64_t ef = vm_watch(f, 64);
        assert(ef);
        pthread_t t[8];
        struct writer w[8];
        for (int i = 0; i < 8; ++i) {
            w[i].at = vm_base + f;
            w[i].value = (uint8_t)i;
            assert(pthread_create(&t[i], NULL, write_many, &w[i]) == 0);
        }
        for (int i = 0; i < 8; ++i) pthread_join(t[i], NULL);
        assert(!vm_clean_since(f, 64, ef));
    }
#endif

    /* 6. Ranges outside guest memory and empty ranges are refused. */
    assert(vm_watch(0, 0) == 0);
    assert(!vm_clean_since(a, 0, e2));
    vm_note_host_write((const void*)&page, sizeof page); /* not guest: ignored */

    vm_watch_stats_t s = stats();
    printf("vm_write_watch: watches=%llu pages=%llu faults=%llu notes=%llu forgotten=%llu\n",
           (unsigned long long)s.watches, (unsigned long long)s.protected_pages,
           (unsigned long long)s.faults, (unsigned long long)s.notes,
           (unsigned long long)s.forgotten);
    return 0;
}
