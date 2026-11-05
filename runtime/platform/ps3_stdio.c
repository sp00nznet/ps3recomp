/*
 * ps3recomp - buffered host stdout/stderr, flushed on a timer and on the way out
 * (see ps3_stdio.h for why)
 *
 * Win32-shaped on purpose: on Windows this is the real API, elsewhere it is
 * win32_compat.h, whose SetUnhandledExceptionFilter is called from the same
 * SIGSEGV/SIGBUS/SIGILL/SIGFPE dispatcher as the runtime's vectored handlers.
 * A fault one of those handlers resolves (demand commit, the guard pages) never
 * reaches the filter, so the crash flush runs only for a real crash.
 *
 * Atomics are the __atomic builtins, as in win32_compat.h and milestone.c.
 */
#include "ps3_stdio.h"
#include "win32_compat.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#define PS3_STDIO_BUF_BYTES        (64u * 1024u)
#define PS3_STDIO_FLUSH_MS_DEFAULT 50u
#define PS3_STDIO_FLUSH_MS_MAX     10000u
#define PS3_STDIO_CRASH_WAIT_MS    1000u

static char s_out_buf[PS3_STDIO_BUF_BYTES];
static char s_err_buf[PS3_STDIO_BUF_BYTES];

static int      s_init_done;            /* first ps3_stdio_init() call wins */
static int      s_buffered;             /* the flush thread is running */
static int      s_stop;                 /* exit has begun: the thread stops flushing */
static unsigned s_flush_ms = PS3_STDIO_FLUSH_MS_DEFAULT;
static HANDLE   s_wake;                 /* auto-reset: a crash flush wakes the thread early */
static long     s_flush_req;            /* crash flushes asked for */
static long     s_flush_done;           /* value of s_flush_req the last flush covered */

static LPTOP_LEVEL_EXCEPTION_FILTER s_prev_filter;
static void (*s_prev_abort)(int);

static DWORD WINAPI ps3_stdio_flusher(LPVOID arg)
{
    (void)arg;
    for (;;) {
        WaitForSingleObject(s_wake, s_flush_ms);
        if (__atomic_load_n(&s_stop, __ATOMIC_ACQUIRE)) return 0;
        long want = __atomic_load_n(&s_flush_req, __ATOMIC_ACQUIRE);
        fflush(stdout);
        fflush(stderr);
        __atomic_store_n(&s_flush_done, want, __ATOMIC_RELEASE);
    }
}

void ps3_stdio_flush(void)
{
    fflush(stdout);
    fflush(stderr);
}

void ps3_stdio_flush_crash(void)
{
    if (!__atomic_load_n(&s_buffered, __ATOMIC_ACQUIRE)) return;   /* nothing buffered */
    if (__atomic_load_n(&s_stop, __ATOMIC_ACQUIRE)) return;        /* exit is flushing */
    long want = __atomic_add_fetch(&s_flush_req, 1, __ATOMIC_ACQ_REL);
    SetEvent(s_wake);
    for (unsigned waited = 0; waited < PS3_STDIO_CRASH_WAIT_MS; waited += 5) {
        if (__atomic_load_n(&s_flush_done, __ATOMIC_ACQUIRE) >= want) return;
        Sleep(5);
    }
}

static void ps3_stdio_at_exit(void)
{
    __atomic_store_n(&s_stop, 1, __ATOMIC_RELEASE);
    ps3_stdio_flush();
}

/* abort(): flush, then whatever was installed before. Returning lets abort()
 * end the process as it would have. */
static void ps3_stdio_on_abort(int sig)
{
    ps3_stdio_flush_crash();
    if (s_prev_abort && s_prev_abort != SIG_DFL && s_prev_abort != SIG_IGN)
        s_prev_abort(sig);
}

/* A crash: flush what the run printed before it, let the previous filter
 * report, then flush its report too (unless it resolved the exception). */
static LONG WINAPI ps3_stdio_on_crash(EXCEPTION_POINTERS* ep)
{
    ps3_stdio_flush_crash();
    LONG r = s_prev_filter ? s_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
    if (r != EXCEPTION_CONTINUE_EXECUTION) ps3_stdio_flush_crash();
    return r;
}

static unsigned ps3_stdio_env_ms(const char* name, unsigned def)
{
    const char* s = getenv(name);
    if (!s || !*s) return def;
    char* end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (*end || v < 1 || v > PS3_STDIO_FLUSH_MS_MAX) {
        fprintf(stderr, "[stdio] %s=%s ignored (1..%u), using %u\n",
                name, s, PS3_STDIO_FLUSH_MS_MAX, def);
        return def;
    }
    return (unsigned)v;
}

void ps3_stdio_init(void)
{
    if (__atomic_exchange_n(&s_init_done, 1, __ATOMIC_ACQ_REL)) return;

    const char* unbuf = getenv("PS3_STDIO_UNBUFFERED");
    if (unbuf && unbuf[0] == '1') {
        setvbuf(stdout, NULL, _IONBF, 0);
        return;
    }

    s_flush_ms = ps3_stdio_env_ms("PS3_STDIO_FLUSH_MS", PS3_STDIO_FLUSH_MS_DEFAULT);
    s_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!s_wake) { setvbuf(stdout, NULL, _IONBF, 0); return; }

    /* setvbuf on a stream that has been written to: flush first, which is
     * what the C libraries this builds against need to switch modes cleanly
     * (the old _IONBF call here relied on the same). */
    fflush(stdout);
    fflush(stderr);
    setvbuf(stdout, s_out_buf, _IOFBF, sizeof s_out_buf);
    setvbuf(stderr, s_err_buf, _IOFBF, sizeof s_err_buf);

    HANDLE t = CreateThread(NULL, 0, ps3_stdio_flusher, NULL, 0, NULL);
    if (!t) {
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
        fprintf(stderr, "[stdio] flush thread failed to start; stdout/stderr unbuffered\n");
        return;
    }
    CloseHandle(t);
    __atomic_store_n(&s_buffered, 1, __ATOMIC_RELEASE);

    atexit(ps3_stdio_at_exit);
    void (*prev)(int) = signal(SIGABRT, ps3_stdio_on_abort);
    s_prev_abort = (prev == SIG_ERR) ? NULL : prev;
    s_prev_filter = SetUnhandledExceptionFilter(ps3_stdio_on_crash);

    fprintf(stderr, "[stdio] stdout/stderr buffered (%u KB each), flushed every %u ms"
                    " (PS3_STDIO_UNBUFFERED=1: unbuffered)\n",
            PS3_STDIO_BUF_BYTES / 1024u, s_flush_ms);
}
