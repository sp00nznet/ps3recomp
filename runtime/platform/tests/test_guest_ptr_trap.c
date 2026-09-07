/*
 * ps3recomp - which signal an inaccessible guest window faults with
 *
 * runtime/ppu/ppu_loader.cpp reserves the low 4 GB PROT_NONE so that an HLE
 * function which dereferences a guest pointer WITHOUT translating it through
 * vm_base faults on a page nothing else can ever own, and then reports the
 * guest address and the guest caller from a signal handler. Which signal that
 * fault arrives on is the host's choice, and the two hosts disagree, so the
 * handler has to be installed for both. This is the evidence for that, and the
 * regression net for the reasoning behind it.
 *
 * ppu_loader.cpp itself cannot be linked into a test. It includes the per-game
 * ppu_recomp.h the lifter generates, so nothing without a game can build it,
 * and its handler deliberately does not swallow the fault -- it chains to
 * whatever was installed before, which by design lets the process die. Driving
 * it from the boot smoke title would mean arming a recovering handler
 * underneath it and reserving the low 4 GB out from under the smoke title's own
 * guest memory, to prove a property that has nothing to do with the smoke
 * title. So this rebuilds the mechanism in miniature instead: the same
 * PROT_NONE reservation, the same SA_SIGINFO handler on both signals, the same
 * si_addr decode, and the same chain to a predecessor. What it pins is the
 * platform behaviour the loader's choice of signals rests on.
 *
 * The output line is worth reading as well as the exit status: it says which
 * signal this host produces and whether a low guest window can be reserved at
 * all, and those two together are why the trap needs both signals rather than
 * whichever one the machine in front of you happens to raise.
 *
 * Build (any POSIX host):
 *   cc -std=gnu17 -Wall -Wextra \
 *      -o test_guest_ptr_trap runtime/platform/tests/test_guest_ptr_trap.c
 *
 * Exit status is the number of failed checks, so it works as a CI step.
 */
/* glibc gates MAP_ANONYMOUS and MAP_NORESERVE behind _DEFAULT_SOURCE, which
 * -std=gnu17 supplies and -std=c17 would not, and it has to be set before the
 * first system header. Setting it here means the file does not depend on which
 * -std whoever builds it happens to pick. Darwin needs none of it. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE
#endif
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int g_pass, g_fail;
#define CHECK(cond) do { if (cond) g_pass++; else { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* Everything a handler reports is volatile. Control comes back out of the
 * handler through siglongjmp, into the middle of a function the optimiser has
 * already satisfied itself nothing can reach. */
static volatile sig_atomic_t g_segv_hits;
static volatile sig_atomic_t g_bus_hits;
static volatile sig_atomic_t g_chain_hits;
static volatile sig_atomic_t g_chain_sig;
static volatile sig_atomic_t g_code;
static void* volatile        g_addr;
static sigjmp_buf            g_escape;

/* The signal a PROT_NONE page faults with here, which is the whole question.
 * Darwin turns a Mach EXC_BAD_ACCESS into SIGBUS whenever the page WAS mapped
 * and its protection refused the access, and into SIGSEGV only when nothing was
 * mapped at the address at all. Linux reports both as SIGSEGV. */
#if defined(__APPLE__)
#  define PS3_RESERVED_FAULT_SIGNAL SIGBUS
#else
#  define PS3_RESERVED_FAULT_SIGNAL SIGSEGV
#endif

static void record_and_escape(int sig, siginfo_t* si, void* uctx)
{
    (void)uctx;
    if (sig == SIGBUS) g_bus_hits++; else g_segv_hits++;
    g_code = si ? si->si_code : -1;
    g_addr = si ? si->si_addr : NULL;
    siglongjmp(g_escape, 1);
}

static void install(int sig, void (*fn)(int, siginfo_t*, void*), struct sigaction* prev)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fn;
    sa.sa_flags     = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, prev);
}

static void restore(int sig)
{
    struct sigaction dfl;
    memset(&dfl, 0, sizeof dfl);
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(sig, &dfl, NULL);
}

static void clear_hits(void)
{
    g_segv_hits = 0; g_bus_hits = 0; g_chain_hits = 0; g_chain_sig = 0;
    g_code = -1; g_addr = NULL;
}

/* Returns 1 if the access faulted and the handler escaped, 0 if it went
 * through -- which for a PROT_NONE page would itself be the failure. */
static int touch_read(const volatile unsigned char* at)
{
    if (sigsetjmp(g_escape, 1) != 0) return 1;
    unsigned char v = *at;
    (void)v;
    return 0;
}

static int touch_write(volatile unsigned char* at)
{
    if (sigsetjmp(g_escape, 1) != 0) return 1;
    *at = 0x5A;
    return 0;
}

/* The loader's own predicate: above the first page, because a plain NULL deref
 * is someone else's bug, and below 4 GB, because that is the whole of the guest
 * address space. */
static int is_guest_ptr_fault(uintptr_t at)
{
    return at >= 0x10000u && at < 0x100000000ull;
}

/* ---- a reserved window faults, and says where ---------------------------- */
static void test_reserved_window(void)
{
    size_t pg  = (size_t)sysconf(_SC_PAGESIZE);
    size_t len = pg * 4;
    unsigned char* w = (unsigned char*)mmap(NULL, len, PROT_NONE,
                                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                                            -1, 0);
    CHECK(w != MAP_FAILED);
    if (w == MAP_FAILED) return;

    install(SIGSEGV, record_and_escape, NULL);
    install(SIGBUS,  record_and_escape, NULL);

    /* A read of a reserved page. The signal is the host's choice; si_addr is
     * the faulting byte on either, which is the only thing the loader's report
     * reads out of the frame. */
    clear_hits();
    CHECK(touch_read(w + 0x18));
    CHECK(g_segv_hits + g_bus_hits == 1);
    CHECK(g_addr == (void*)(w + 0x18));
    if (PS3_RESERVED_FAULT_SIGNAL == SIGBUS) {
        CHECK(g_bus_hits == 1 && g_segv_hits == 0);   /* a SIGSEGV-only handler misses it */
    } else {
        CHECK(g_segv_hits == 1 && g_bus_hits == 0);
    }

    /* A write of the same page arrives the same way: the direction does not
     * change which signal the host picks, which is why the loader's report
     * cannot name read or write from the signal alone. */
    clear_hits();
    CHECK(touch_write(w + 0x28));
    CHECK(g_segv_hits + g_bus_hits == 1);
    CHECK(g_addr == (void*)(w + 0x28));
    CHECK((PS3_RESERVED_FAULT_SIGNAL == SIGBUS ? g_bus_hits : g_segv_hits) == 1);

    /* si_code does not discriminate on Darwin: a protection fault reports 1,
     * the value that spells BUS_ADRALN, for an access that is perfectly
     * aligned. Nothing may key off it. */
#if defined(__APPLE__)
    CHECK(g_code == 1);
#endif

    /* Nothing mapped at all is the OTHER case, and it is SIGSEGV on both
     * hosts -- so swapping SIGSEGV for SIGBUS would be as wrong as the reverse.
     * Both are needed, which is the point. */
    CHECK(munmap(w, len) == 0);
    clear_hits();
    CHECK(touch_read(w + 0x18));
    CHECK(g_segv_hits == 1 && g_bus_hits == 0);
    CHECK(g_addr == (void*)(w + 0x18));

    restore(SIGSEGV);
    restore(SIGBUS);
}

/* ---- the address decodes to a guest EA ----------------------------------- */
/* The loader reserves at fixed low addresses and reports (uint32_t)si_addr as
 * the guest pointer that was never translated, so an untranslated dereference
 * has to fault at the guest EA itself and arrive with that address intact.
 *
 * Whether the reservation lands is the host's business, and this is where the
 * two really part company. Linux hands out the low addresses. macOS does not
 * hand out ANY of them: a 64-bit Mach-O carries a __PAGEZERO segment over
 * 0 .. 0x100000000 with no protections, mmap refuses to place anything inside
 * it, and the address hint is silently ignored -- so the guest window on macOS
 * is __PAGEZERO, and the fault is a SIGSEGV against an address that reads as
 * unmapped rather than a SIGBUS against a reservation. Neither call here passes
 * MAP_FIXED: evicting a live mapping to make a diagnostic work would be worse
 * than not having the diagnostic. Either way the address decodes, and that is
 * what this asserts. */
static void test_address_decode(void)
{
    CHECK(!is_guest_ptr_fault(0));            /* a plain NULL deref is not this bug */
    CHECK(!is_guest_ptr_fault(0xFFFF));
    CHECK(is_guest_ptr_fault(0x10000));
    CHECK(is_guest_ptr_fault(0xFFFFFFFFull));
    CHECK(!is_guest_ptr_fault(0x100000000ull));

    const uintptr_t base = 0x30000000u;       /* guest-shaped, and out of the way */
    const uintptr_t ea   = base + 0x1234u;
    size_t len = 0x100000u;

    void* at = mmap((void*)base, len, PROT_NONE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    int reserved = (at != MAP_FAILED && (uintptr_t)at == base);
    if (at != MAP_FAILED && !reserved) { munmap(at, len); at = MAP_FAILED; }
    printf("guest window at 0x%08lX: %s\n", (unsigned long)base,
           reserved ? "reserved PROT_NONE"
                    : "unmappable, so the fault is against whatever owns it");

    install(SIGSEGV, record_and_escape, NULL);
    install(SIGBUS,  record_and_escape, NULL);
    clear_hits();
    CHECK(touch_read((const volatile unsigned char*)ea));
    CHECK(g_segv_hits + g_bus_hits == 1);
    CHECK((uintptr_t)g_addr == ea);                      /* the guest EA itself */
    CHECK(is_guest_ptr_fault((uintptr_t)g_addr));
    CHECK((uint32_t)(uintptr_t)g_addr == 0x30001234u);   /* what the report prints */
    if (reserved) CHECK(g_bus_hits + g_segv_hits == 1 &&
                        (PS3_RESERVED_FAULT_SIGNAL == SIGBUS ? g_bus_hits : g_segv_hits) == 1);
    else          CHECK(g_segv_hits == 1);               /* nothing mapped: SIGSEGV */
    restore(SIGSEGV);
    restore(SIGBUS);
    if (reserved) munmap(at, len);
}

/* ---- the chain to whoever held the signal before -------------------------- */
/* The loader reports and then hands the signal on, so a real bug still stops
 * the run and a debugger or a runner's own crash handler still sees it. Both
 * signals need their OWN saved predecessor: they are separate sigaction slots,
 * and chaining a SIGBUS to the handler that was registered for SIGSEGV runs a
 * handler for a signal it never agreed to take. */
static struct sigaction s_prev[2];            /* [0] SIGSEGV, [1] SIGBUS */

/* Records rather than checks: a handler that ran CHECK would be writing the
 * pass counters from a signal frame, and main reads them from ordinary code. */
static void chain_previous(int sig, siginfo_t* si, void* uctx)
{
    (void)si; (void)uctx;
    g_chain_hits++;
    g_chain_sig = sig;
    siglongjmp(g_escape, 1);
}

static void report_then_chain(int sig, siginfo_t* si, void* uctx)
{
    if (sig == SIGBUS) g_bus_hits++; else g_segv_hits++;
    g_addr = si ? si->si_addr : NULL;
    const struct sigaction* prev = &s_prev[sig == SIGBUS ? 1 : 0];
    if ((prev->sa_flags & SA_SIGINFO) && prev->sa_sigaction) {
        prev->sa_sigaction(sig, si, uctx);
        return;
    }
    restore(sig);
}

static void test_chaining(void)
{
    size_t pg = (size_t)sysconf(_SC_PAGESIZE);
    unsigned char* w = (unsigned char*)mmap(NULL, pg, PROT_NONE,
                                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                                            -1, 0);
    CHECK(w != MAP_FAILED);
    if (w == MAP_FAILED) return;

    /* Whoever was there first: a runner's crash handler, or the shim's
     * vectored-exception dispatcher. */
    install(SIGSEGV, chain_previous, NULL);
    install(SIGBUS,  chain_previous, NULL);
    /* ...and the trap on top, keeping each signal's predecessor separately. */
    install(SIGSEGV, report_then_chain, &s_prev[0]);
    install(SIGBUS,  report_then_chain, &s_prev[1]);

    clear_hits();
    CHECK(touch_read(w + 0x40));
    CHECK(g_segv_hits + g_bus_hits == 1);              /* reported once */
    CHECK(g_chain_hits == 1);                          /* and passed on once */
    CHECK(g_chain_sig == PS3_RESERVED_FAULT_SIGNAL);   /* to that signal's own predecessor */
    CHECK(g_addr == (void*)(w + 0x40));

    restore(SIGSEGV);
    restore(SIGBUS);
    munmap(w, pg);
}

int main(void)
{
    printf("host delivers a PROT_NONE fault as %s\n",
           PS3_RESERVED_FAULT_SIGNAL == SIGBUS ? "SIGBUS" : "SIGSEGV");
    test_reserved_window();
    test_address_decode();
    test_chaining();
    printf("guest-pointer trap tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
