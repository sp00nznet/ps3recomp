/*
 * ps3recomp - guest write watch. See vm_watch.h.
 *
 * Per host page of the 4 GB guest space: a state byte and the epoch of its
 * last recorded write. The state is WATCHED (write-protected by us), BUSY
 * (a thread is releasing it), EVER (watched at least once; now writable),
 * PINNED (the runtime made it non-writable: never watched). One global epoch
 * counter orders watches and writes.
 *
 * The fault handler only touches atomics and the protection of the faulting
 * page, so it is safe on a thread that holds runtime locks. A thread whose
 * fault lands just after another thread released the same page finds it no
 * longer watched but EVER; it retries the write (at most a few times for the
 * same address, so a page protected for another reason still reaches the
 * previous handler).
 */
#include "vm_watch.h"
#include "vm_watch_hooks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <sched.h>
  #include <signal.h>
  #include <sys/mman.h>
  #include <unistd.h>
#endif

extern uint8_t* vm_base;

enum { WATCHED = 1, BUSY = 2, EVER = 4, PINNED = 8 };
#define SPACE UINT64_C(0x100000000)

static int s_active;
static unsigned s_shift;          /* log2(host page size) */
static uint64_t s_page;
static size_t s_pages;
static uint8_t* s_state;
static uint64_t* s_epoch;
static uint64_t s_global = 1;
static vm_watch_stats_t s_stats;
static int s_strict;

#define LOAD8(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define STORE8(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define CAS8(p, e, d) __atomic_compare_exchange_n((p), (e), (d), 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)
#define BUMP() __atomic_add_fetch(&s_global, 1, __ATOMIC_ACQ_REL)
#define COUNT(f, n) __atomic_add_fetch(&s_stats.f, (n), __ATOMIC_RELAXED)

static uintptr_t host_of(uint64_t ea) { return (uintptr_t)vm_base + (uintptr_t)ea; }

static int protect(size_t first, size_t count, int writable)
{
    void* at = (void*)host_of((uint64_t)first << s_shift);
    size_t bytes = count << s_shift;
#ifdef _WIN32
    DWORD old;
    return VirtualProtect(at, bytes, writable ? PAGE_READWRITE : PAGE_READONLY, &old) ? 0 : -1;
#else
    return mprotect(at, bytes, writable ? (PROT_READ | PROT_WRITE) : PROT_READ);
#endif
}

/* Releases watched page `p`: writable, a new write epoch, no longer watched.
 * Returns 1 when this thread released it. */
static int release(size_t p)
{
    uint8_t st = LOAD8(&s_state[p]);
    while (st & WATCHED) {
        if (CAS8(&s_state[p], &st, (uint8_t)((st & ~WATCHED) | BUSY | EVER))) {
            protect(p, 1, 1);
            __atomic_store_n(&s_epoch[p], BUMP(), __ATOMIC_RELEASE);
            STORE8(&s_state[p], (uint8_t)((st & ~(WATCHED | BUSY)) | EVER));
            return 1;
        }
    }
    return 0;
}

static void wait_not_busy(size_t p)
{
    while (LOAD8(&s_state[p]) & BUSY) {
#ifdef _WIN32
        YieldProcessor();
#else
        sched_yield();
#endif
    }
}

#ifdef _WIN32
static __declspec(thread) uintptr_t t_last_fault;
static __declspec(thread) unsigned t_retries;
#else
static __thread uintptr_t t_last_fault;
static __thread unsigned t_retries;
#endif

/* Whether a write fault at host address `at` was ours (the write may retry). */
static int claim(uintptr_t at)
{
    if (!s_active) return 0;
    uintptr_t base = (uintptr_t)vm_base;
    if (at < base || (uint64_t)(at - base) >= SPACE) return 0;
    size_t p = (size_t)((uint64_t)(at - base) >> s_shift);
    if (release(p)) {
        COUNT(faults, 1);
        t_last_fault = 0;
        return 1;
    }
    wait_not_busy(p);
    uint8_t st = LOAD8(&s_state[p]);
    if (st & WATCHED) {
        /* Watched again meanwhile: this write is still a write. */
        release(p);
        return 1;
    }
    if (!(st & EVER) || (st & PINNED)) return 0;
    /* Released by another thread after this fault: retry, but only a few
     * times for one address. */
    if (t_last_fault != at) {
        t_last_fault = at;
        t_retries = 0;
    }
    return ++t_retries <= 4;
}

#ifdef _WIN32
static LONG CALLBACK veh(PEXCEPTION_POINTERS ep)
{
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2 &&
        er->ExceptionInformation[0] == 1 && claim((uintptr_t)er->ExceptionInformation[1]))
        return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}
#else
static struct sigaction s_prev[2];
static int s_prev_valid[2];
static void fault(int sig, siginfo_t* si, void* uctx)
{
    if (si && claim((uintptr_t)si->si_addr)) return;
    const int slot = sig == SIGBUS;
    if (s_prev_valid[slot]) {
        const struct sigaction* prev = &s_prev[slot];
        if ((prev->sa_flags & SA_SIGINFO) && prev->sa_sigaction) {
            prev->sa_sigaction(sig, si, uctx);
            return;
        }
        if (prev->sa_handler && prev->sa_handler != SIG_DFL && prev->sa_handler != SIG_IGN) {
            prev->sa_handler(sig);
            return;
        }
    }
    /* Default action: the faulting instruction re-runs and ends the process. */
    struct sigaction dfl;
    memset(&dfl, 0, sizeof dfl);
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(sig, &dfl, NULL);
}
#endif

static void forget(uint32_t addr, uint32_t size, int writable);
static void forget_host(const void* host, size_t size, int writable);
static void note_write(uint32_t ea, size_t len);
static void note_host_write(const void* host, size_t len);
static void note_write_check(int failed_access, const char* site);

static int flag(const char* name)
{
    const char* v = getenv(name);
    return v && v[0] == '1' && !v[1];
}

int vm_watch_enable(void)
{
    static int s_tried;
    if (__atomic_load_n(&s_active, __ATOMIC_ACQUIRE)) return 1;
    if (__atomic_exchange_n(&s_tried, 1, __ATOMIC_ACQ_REL)) return __atomic_load_n(&s_active, __ATOMIC_ACQUIRE);
    if (!flag("PS3RECOMP_VM_WRITE_WATCH")) return 0;
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    s_page = si.dwPageSize;
#else
    long v = sysconf(_SC_PAGESIZE);
    s_page = v > 0 ? (uint64_t)v : 4096;
#endif
    for (s_shift = 0; (UINT64_C(1) << s_shift) < s_page; ++s_shift) {}
    s_pages = (size_t)(SPACE >> s_shift);
    s_state = (uint8_t*)calloc(s_pages, 1);
    s_epoch = (uint64_t*)calloc(s_pages, sizeof *s_epoch);
    if (!s_state || !s_epoch) {
        free(s_state);
        free(s_epoch);
        return 0;
    }
    s_strict = flag("PS3RECOMP_VM_WRITE_WATCH_STRICT");
#ifdef _WIN32
    if (!AddVectoredExceptionHandler(1, veh)) return 0;
#else
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fault;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    /* Protection faults arrive as SIGSEGV on Linux and SIGBUS on macOS. */
    s_prev_valid[0] = sigaction(SIGSEGV, &sa, &s_prev[0]) == 0;
    s_prev_valid[1] = sigaction(SIGBUS, &sa, &s_prev[1]) == 0;
#endif
    ps3_vm_watch_hook.forget = forget;
    ps3_vm_watch_hook.forget_host = forget_host;
    ps3_vm_watch_hook.note_write = note_write;
    ps3_vm_watch_hook.note_host_write = note_host_write;
    ps3_vm_watch_hook.note_write_check = note_write_check;
    __atomic_store_n(&s_active, 1, __ATOMIC_RELEASE);
    fprintf(stderr, "[vm-watch] guest write watch active (host page %llu bytes%s)\n",
            (unsigned long long)s_page, s_strict ? ", strict" : "");
    return 1;
}

int vm_watch_active(void) { return __atomic_load_n(&s_active, __ATOMIC_ACQUIRE); }

/* Host pages [first, last] covering [ea, ea+len). */
static int span(uint64_t ea, uint64_t len, size_t* first, size_t* last)
{
    if (!len || ea >= SPACE || len > SPACE - ea) return 0;
    *first = (size_t)(ea >> s_shift);
    *last = (size_t)((ea + len - 1) >> s_shift);
    return 1;
}

uint64_t vm_watch(uint32_t ea, uint32_t len)
{
    size_t first, last;
    if (!vm_watch_active() || !span(ea, len, &first, &last)) return 0;
    for (size_t p = first; p <= last; ++p)
        if (LOAD8(&s_state[p]) & PINNED) {
            COUNT(refused, 1);
            return 0;
        }
    /* The epoch precedes the protection: a write after it is a later epoch. */
    const uint64_t epoch = BUMP();
    size_t run = 0, protected_pages = 0;
    for (size_t p = first; p <= last + 1; ++p) {
        int take = 0;
        if (p <= last) {
            wait_not_busy(p);
            uint8_t st = LOAD8(&s_state[p]);
            while (!(st & (WATCHED | PINNED | BUSY))) {
                if (CAS8(&s_state[p], &st, (uint8_t)(st | WATCHED | EVER))) {
                    take = 1;
                    break;
                }
            }
        }
        if (take) {
            ++run;
            continue;
        }
        if (run) {
            const size_t start = p - run;
            if (protect(start, run, 0) != 0) {
                for (size_t q = start; q < p; ++q) {
                    uint8_t st = LOAD8(&s_state[q]);
                    STORE8(&s_state[q], (uint8_t)(st & ~WATCHED));
                }
                return 0;
            }
            protected_pages += run;
            run = 0;
        }
    }
    if (protected_pages) {
        COUNT(watches, 1);
        COUNT(protected_pages, protected_pages);
    }
    for (size_t p = first; p <= last; ++p)
        if (!(LOAD8(&s_state[p]) & WATCHED)) return 0;
    return epoch;
}

int vm_clean_since(uint32_t ea, uint32_t len, uint64_t epoch)
{
    size_t first, last;
    if (!epoch || !vm_watch_active() || !span(ea, len, &first, &last)) return 0;
    for (size_t p = first; p <= last; ++p) {
        if ((LOAD8(&s_state[p]) & (WATCHED | BUSY)) != WATCHED) return 0;
        if (__atomic_load_n(&s_epoch[p], __ATOMIC_ACQUIRE) > epoch) return 0;
    }
    return 1;
}

static void note_write(uint32_t ea, size_t len)
{
    size_t first, last;
    if (!vm_watch_active() || !span(ea, len, &first, &last)) return;
    size_t released = 0;
    for (size_t p = first; p <= last; ++p) {
        wait_not_busy(p);
        released += (size_t)release(p);
    }
    if (released) COUNT(notes, released);
}

static void note_host_write(const void* host, size_t len)
{
    if (!vm_watch_active()) return;
    const uintptr_t at = (uintptr_t)host, base = (uintptr_t)vm_base;
    if (at < base || (uint64_t)(at - base) >= SPACE) return;
    note_write((uint32_t)(at - base), len);
}

static void note_write_check(int failed_access, const char* site)
{
    if (!failed_access || !s_strict || !vm_watch_active()) return;
    fprintf(stderr,
            "[vm-watch] FATAL: an OS write into guest memory failed with an access error at %s; "
            "a kernel writer did not announce itself with vm_note_write\n",
            site ? site : "?");
    fflush(stderr);
    abort();
}

/* The runtime changes the protection of [addr, addr+size) itself. */
static void forget(uint32_t addr, uint32_t size, int writable)
{
    size_t first, last;
    if (!span(addr, size, &first, &last)) return;
    size_t forgotten = 0;
    for (size_t p = first; p <= last; ++p) {
        wait_not_busy(p);
        uint8_t st = LOAD8(&s_state[p]);
        uint8_t next;
        do {
            next = (uint8_t)(st & ~(WATCHED | PINNED));
            if (!writable) next |= PINNED;
        } while (!CAS8(&s_state[p], &st, next));
        if (st & WATCHED) {
            /* Our read-only protection must not outlive the watch: undo it,
             * and let the caller's own protection change follow. */
            protect(p, 1, 1);
            __atomic_store_n(&s_epoch[p], BUMP(), __ATOMIC_RELEASE);
            ++forgotten;
        }
    }
    if (forgotten) COUNT(forgotten, forgotten);
}

static void forget_host(const void* host, size_t size, int writable)
{
    const uintptr_t at = (uintptr_t)host, base = (uintptr_t)vm_base;
    if (!size || at < base || (uint64_t)(at - base) >= SPACE) return;
    uint64_t ea = (uint64_t)(at - base), len = size;
    if (len > SPACE - ea) len = SPACE - ea;
    /* forget() takes a 32-bit size: split a range of 4 GB. */
    while (len) {
        const uint32_t part = len > UINT32_MAX ? UINT32_MAX - 0xFFFFu : (uint32_t)len;
        forget((uint32_t)ea, part, writable);
        ea += part;
        len -= part;
    }
}

void vm_watch_stats(vm_watch_stats_t* out)
{
    if (!out) return;
    out->watches = __atomic_load_n(&s_stats.watches, __ATOMIC_RELAXED);
    out->protected_pages = __atomic_load_n(&s_stats.protected_pages, __ATOMIC_RELAXED);
    out->faults = __atomic_load_n(&s_stats.faults, __ATOMIC_RELAXED);
    out->notes = __atomic_load_n(&s_stats.notes, __ATOMIC_RELAXED);
    out->forgotten = __atomic_load_n(&s_stats.forgotten, __ATOMIC_RELAXED);
    out->refused = __atomic_load_n(&s_stats.refused, __ATOMIC_RELAXED);
}
