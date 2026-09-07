/*
 * ps3recomp - self-contained test for runtime/platform/win32_compat.h
 *
 * Exercises the POSIX side of the Win32 shim: interlocked return conventions
 * and widths, critical sections, SRW locks in both modes, condition variables
 * against each lock kind, one-time initialisation, events (auto and manual
 * reset), semaphores, joinable threads with exit codes and the
 * CREATE_SUSPENDED gate, WaitForMultipleObjects in both modes, WaitOnAddress,
 * waitable timers, timing, virtual memory, stopping a running thread and
 * reading its registers, the address-space queries, vectored exception
 * handlers, the module-name and dbghelp symbol calls, directory creation, and
 * the MSVC CRT spellings.
 *
 * Build (any POSIX host):
 *   cc -std=gnu17 -Wall -Wextra -I runtime/platform \
 *      runtime/platform/tests/test_win32_compat.c runtime/platform/win32_compat.c -lpthread
 *
 * Exit status is the number of failed checks, so it works as a CI step.
 */
/* win32_backtrace.h resolves symbols through dladdr, whose Dl_info glibc
 * declares only under _GNU_SOURCE, and that has to be set before the first
 * system header of the translation unit. A C++ translation unit gets it from
 * the compiler; this one is C. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE
#endif
#include "../win32_compat.h"
#include "../win32_backtrace.h"
#include "../msvc_compat.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static int g_pass, g_fail;
#define CHECK(cond) do { if (cond) g_pass++; else { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static ULONGLONG now_ms(void) { return GetTickCount64(); }

/* ---- interlocked --------------------------------------------------------- */
static void test_interlocked(void)
{
    CHECK(sizeof(LONG) == 4 && sizeof(ULONG) == 4 && sizeof(DWORD) == 4);
    CHECK(sizeof(LONG64) == 8 && sizeof(LONGLONG) == 8 && sizeof(DWORD_PTR) == sizeof(void*));

    volatile LONG v = 5;
    CHECK(InterlockedIncrement(&v) == 6 && v == 6);
    CHECK(InterlockedDecrement(&v) == 5);
    CHECK(InterlockedExchange(&v, 9) == 5 && v == 9);
    CHECK(InterlockedExchangeAdd(&v, 3) == 9 && v == 12);
    CHECK(InterlockedAdd(&v, -2) == 10);
    CHECK(InterlockedCompareExchange(&v, 20, 10) == 10 && v == 20);
    CHECK(InterlockedCompareExchange(&v, 30, 10) == 20 && v == 20);
    CHECK(InterlockedOr(&v, 1) == 20 && v == 21);
    CHECK(InterlockedAnd(&v, 0x10) == 21 && v == 16);
    CHECK(InterlockedXor(&v, 0x11) == 16 && v == 1);
    CHECK(_InterlockedExchange(&v, 2) == 1 && v == 2);
    CHECK(_InterlockedCompareExchange(&v, 3, 2) == 2 && v == 3);

    volatile LONG64 w = 1LL << 40;
    CHECK(InterlockedIncrement64(&w) == (1LL << 40) + 1);
    CHECK(InterlockedExchange64(&w, 7) == (1LL << 40) + 1 && w == 7);
    CHECK(InterlockedCompareExchange64(&w, 8, 7) == 7 && w == 8);
    CHECK(InterlockedExchangeAdd64(&w, 2) == 8 && w == 10);
    CHECK(InterlockedAdd64(&w, 5) == 15);
    CHECK(InterlockedOr64(&w, 0x100) == 15 && w == 0x10F);

    int x = 0;
    PVOID volatile p = NULL;
    CHECK(InterlockedCompareExchangePointer(&p, &x, NULL) == NULL && p == &x);
    CHECK(InterlockedExchangePointer(&p, NULL) == &x && p == NULL);

    WriteRelease(&v, 77);
    CHECK(ReadAcquire(&v) == 77);
    MemoryBarrier(); _ReadWriteBarrier(); YieldProcessor();
}

/* ---- threads + critical sections ----------------------------------------- */
static volatile LONG    g_counter;
static CRITICAL_SECTION g_cs;
static long             g_cs_counter;

static DWORD WINAPI inc_worker(LPVOID arg)
{
    int n = (int)(intptr_t)arg;
    for (int i = 0; i < n; i++) {
        InterlockedIncrement(&g_counter);
        EnterCriticalSection(&g_cs);
        g_cs_counter++;
        LeaveCriticalSection(&g_cs);
    }
    return 42;
}

static void test_threads_and_cs(void)
{
    InitializeCriticalSection(&g_cs);
    EnterCriticalSection(&g_cs);
    EnterCriticalSection(&g_cs);              /* recursive, as on Windows */
    CHECK(TryEnterCriticalSection(&g_cs));
    LeaveCriticalSection(&g_cs); LeaveCriticalSection(&g_cs); LeaveCriticalSection(&g_cs);

    HANDLE th[4];
    for (int i = 0; i < 4; i++) {
        th[i] = CreateThread(NULL, 0, inc_worker, (LPVOID)(intptr_t)20000, 0, NULL);
        CHECK(th[i] != NULL);
    }
    CHECK(WaitForMultipleObjects(4, th, TRUE, 20000) == WAIT_OBJECT_0);
    CHECK(g_counter == 80000);
    CHECK(g_cs_counter == 80000);
    DWORD code = 0;
    CHECK(GetExitCodeThread(th[0], &code) && code == 42);
    for (int i = 0; i < 4; i++) CHECK(CloseHandle(th[i]));
    DeleteCriticalSection(&g_cs);
}

/* ---- thread lifecycle ---------------------------------------------------- */
static DWORD WINAPI flag_worker(LPVOID arg) { InterlockedExchange((volatile LONG*)arg, 1); return 3; }
static DWORD WINAPI exit_worker(LPVOID arg) { (void)arg; ExitThread(7); return 0; }
static unsigned crt_worker(void* arg)       { (void)arg; return 11; }

static void test_thread_lifecycle(void)
{
    volatile LONG started = 0;
    DWORD code = 0;

    HANDLE h = CreateThread(NULL, 64 * 1024, flag_worker, (LPVOID)&started, CREATE_SUSPENDED, NULL);
    CHECK(h != NULL);
    Sleep(30);
    CHECK(started == 0);                                  /* parked on the gate */
    CHECK(GetExitCodeThread(h, &code) && code == STILL_ACTIVE);
    CHECK(WaitForSingleObject(h, 20) == WAIT_TIMEOUT);
    CHECK(SuspendThread(h) == 1);                         /* not started: exact */
    CHECK(ResumeThread(h) == 2);
    CHECK(ResumeThread(h) == 1);
    CHECK(WaitForSingleObject(h, 5000) == WAIT_OBJECT_0);
    CHECK(started == 1);
    CHECK(GetExitCodeThread(h, &code) && code == 3);
    CHECK(WaitForSingleObject(h, 0) == WAIT_OBJECT_0);    /* a finished thread stays signaled */
    CHECK(SuspendThread(h) == (DWORD)-1);                 /* running/finished: not expressible */
    CHECK(CloseHandle(h));

    h = CreateThread(NULL, 0, exit_worker, NULL, 0, NULL);
    CHECK(WaitForSingleObject(h, 5000) == WAIT_OBJECT_0);
    CHECK(GetExitCodeThread(h, &code) && code == 7);
    CHECK(CloseHandle(h));

    uintptr_t ch = _beginthreadex(NULL, 0, crt_worker, NULL, 0, NULL);
    CHECK(ch != 0);
    CHECK(WaitForSingleObject((HANDLE)ch, 5000) == WAIT_OBJECT_0);
    CHECK(GetExitCodeThread((HANDLE)ch, &code) && code == 11);
    CHECK(CloseHandle((HANDLE)ch));

    /* spawn and forget: the handle goes before the thread does */
    volatile LONG f2 = 0;
    h = CreateThread(NULL, 0, flag_worker, (LPVOID)&f2, 0, NULL);
    CHECK(CloseHandle(h));
    for (int i = 0; i < 1000 && !f2; i++) Sleep(2);
    CHECK(f2 == 1);

    CHECK(SuspendThread(GetCurrentThread()) == (DWORD)-1);
    CHECK(SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL));
    CHECK(SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL));
    CHECK(SetThreadDescription(GetCurrentThread(), L"compat-test") == 0);
    (void)SetThreadAffinityMask(GetCurrentThread(), 1);
    CHECK(GetCurrentThreadId() != 0);
    CHECK(GetCurrentProcessId() != 0);
    CHECK(CloseHandle(GetCurrentThread()));
    CHECK(WaitForSingleObject(NULL, 0) == WAIT_FAILED);
    CHECK(!CloseHandle(NULL) && GetLastError() == ERROR_INVALID_HANDLE);
}

/* ---- events -------------------------------------------------------------- */
static HANDLE        g_ev;
static volatile LONG g_ev_woken;
static DWORD WINAPI ev_waiter(LPVOID a)
{
    (void)a;
    if (WaitForSingleObject(g_ev, 5000) == WAIT_OBJECT_0) InterlockedIncrement(&g_ev_woken);
    return 0;
}

static void test_events(void)
{
    g_ev = CreateEventA(NULL, FALSE, FALSE, NULL);         /* auto-reset */
    CHECK(g_ev != NULL);
    CHECK(WaitForSingleObject(g_ev, 0) == WAIT_TIMEOUT);
    ULONGLONG t0 = now_ms();
    CHECK(WaitForSingleObject(g_ev, 30) == WAIT_TIMEOUT);
    CHECK(now_ms() - t0 >= 25);
    CHECK(GetLastError() == ERROR_TIMEOUT);
    CHECK(SetEvent(g_ev));
    CHECK(WaitForSingleObject(g_ev, 0) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(g_ev, 0) == WAIT_TIMEOUT);   /* consumed */

    /* auto-reset: one waiter per SetEvent */
    HANDLE th[3];
    g_ev_woken = 0;
    for (int i = 0; i < 3; i++) th[i] = CreateThread(NULL, 0, ev_waiter, NULL, 0, NULL);
    Sleep(60);
    SetEvent(g_ev);
    Sleep(60);
    CHECK(g_ev_woken == 1);
    SetEvent(g_ev); Sleep(30); SetEvent(g_ev);
    CHECK(WaitForMultipleObjects(3, th, TRUE, 5000) == WAIT_OBJECT_0);
    CHECK(g_ev_woken == 3);
    for (int i = 0; i < 3; i++) CloseHandle(th[i]);
    CloseHandle(g_ev);

    /* manual-reset: everyone */
    HANDLE m = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_ev = m; g_ev_woken = 0;
    for (int i = 0; i < 3; i++) th[i] = CreateThread(NULL, 0, ev_waiter, NULL, 0, NULL);
    Sleep(60);
    SetEvent(m);
    CHECK(WaitForMultipleObjects(3, th, TRUE, 5000) == WAIT_OBJECT_0);
    CHECK(g_ev_woken == 3);
    CHECK(WaitForSingleObject(m, 0) == WAIT_OBJECT_0);     /* still signaled */
    CHECK(ResetEvent(m));
    CHECK(WaitForSingleObject(m, 0) == WAIT_TIMEOUT);
    for (int i = 0; i < 3; i++) CloseHandle(th[i]);
    CloseHandle(m);

    HANDLE ex = CreateEventExA(NULL, NULL, CREATE_EVENT_MANUAL_RESET | CREATE_EVENT_INITIAL_SET, 0);
    CHECK(WaitForSingleObject(ex, 0) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(ex, 0) == WAIT_OBJECT_0);
    CloseHandle(ex);
}

/* ---- semaphores + multi-object waits ------------------------------------- */
static DWORD WINAPI set_later(LPVOID a) { Sleep(40); SetEvent((HANDLE)a); return 0; }

static void test_semaphores(void)
{
    HANDLE s = CreateSemaphoreA(NULL, 2, 5, NULL);
    CHECK(s != NULL);
    CHECK(WaitForSingleObject(s, 0) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(s, 0) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(s, 0) == WAIT_TIMEOUT);
    LONG prev = -1;
    CHECK(ReleaseSemaphore(s, 3, &prev) && prev == 0);
    CHECK(!ReleaseSemaphore(s, 3, NULL));                  /* 3 + 3 > maximum 5 */

    HANDLE e = CreateEventA(NULL, FALSE, FALSE, NULL);
    HANDLE hs[2] = { e, s };
    CHECK(WaitForMultipleObjects(2, hs, FALSE, 0) == WAIT_OBJECT_0 + 1);   /* semaphore: 3 -> 2 */
    CHECK(WaitForMultipleObjects(2, hs, TRUE, 20) == WAIT_TIMEOUT);        /* event unset: nothing consumed */
    SetEvent(e);
    CHECK(WaitForMultipleObjects(2, hs, TRUE, 1000) == WAIT_OBJECT_0);     /* both consumed: 2 -> 1 */
    CHECK(WaitForSingleObject(e, 0) == WAIT_TIMEOUT);
    CHECK(WaitForSingleObject(s, 0) == WAIT_OBJECT_0);                     /* 1 -> 0 */
    CHECK(WaitForSingleObject(s, 0) == WAIT_TIMEOUT);

    HANDLE t = CreateThread(NULL, 0, set_later, e, 0, NULL);
    ULONGLONG t0 = now_ms();
    CHECK(WaitForMultipleObjects(2, hs, FALSE, 5000) == WAIT_OBJECT_0);    /* woken by the other thread */
    CHECK(now_ms() - t0 >= 30);
    CHECK(WaitForSingleObject(t, 5000) == WAIT_OBJECT_0);
    CloseHandle(t); CloseHandle(e); CloseHandle(s);
}

/* ---- WaitOnAddress ------------------------------------------------------- */
static volatile LONG g_addr_val;
static DWORD WINAPI addr_waiter(LPVOID a)
{
    LONG expect = 0;
    ULONGLONG t0 = now_ms();
    while (g_addr_val == 0) WaitOnAddress(&g_addr_val, &expect, sizeof expect, 5000);
    *(ULONGLONG*)a = now_ms() - t0;
    return 0;
}

static void test_wait_on_address(void)
{
    ULONGLONG waited = 0;
    HANDLE th = CreateThread(NULL, 0, addr_waiter, &waited, 0, NULL);
    Sleep(60);
    InterlockedExchange(&g_addr_val, 1);
    WakeByAddressAll((PVOID)&g_addr_val);
    CHECK(WaitForSingleObject(th, 5000) == WAIT_OBJECT_0);
    CHECK(waited >= 50 && waited < 4000);
    CloseHandle(th);

    LONG cmp = 12345;
    ULONGLONG t0 = now_ms();
    CHECK(WaitOnAddress(&g_addr_val, &cmp, sizeof cmp, 1000));   /* already different: immediate */
    CHECK(now_ms() - t0 < 500);

    cmp = 1;
    t0 = now_ms();
    CHECK(!WaitOnAddress(&g_addr_val, &cmp, sizeof cmp, 30));
    CHECK(now_ms() - t0 >= 25);
    CHECK(GetLastError() == ERROR_TIMEOUT);

    volatile unsigned char b = 4; unsigned char bc = 5;
    CHECK(WaitOnAddress(&b, &bc, 1, 10));
    CHECK(!WaitOnAddress(&b, &bc, 3, 10) && GetLastError() == ERROR_INVALID_PARAMETER);
    WakeByAddressSingle((PVOID)&b);
}

/* ---- SRW locks and condition variables ----------------------------------- */
static SRWLOCK            g_srw = SRWLOCK_INIT;
static CONDITION_VARIABLE g_cv  = CONDITION_VARIABLE_INIT;
static int                g_ready;
static volatile LONG      g_readers_in;

static DWORD WINAPI cv_producer(LPVOID a)
{
    (void)a;
    Sleep(40);
    AcquireSRWLockExclusive(&g_srw);
    g_ready = 1;
    ReleaseSRWLockExclusive(&g_srw);
    WakeConditionVariable(&g_cv);
    return 0;
}
/* Readers hold the shared lock until told to let go, rather than for a fixed
 * time: on a loaded runner the main thread's own sleep can outlast any hold
 * time a reader picks, and the writer's failed try then passes for the wrong
 * reason. The main thread waits for both readers to be inside first. */
static HANDLE g_readers_go;
static DWORD WINAPI reader(LPVOID a)
{
    (void)a;
    AcquireSRWLockShared(&g_srw);
    InterlockedIncrement(&g_readers_in);
    WaitForSingleObject(g_readers_go, 5000);
    ReleaseSRWLockShared(&g_srw);
    return 0;
}

static void test_srw_and_cv(void)
{
    HANDLE th;

    g_ready = 0;
    AcquireSRWLockExclusive(&g_srw);
    th = CreateThread(NULL, 0, cv_producer, NULL, 0, NULL);
    while (!g_ready) CHECK(SleepConditionVariableSRW(&g_cv, &g_srw, 5000, 0));
    ReleaseSRWLockExclusive(&g_srw);
    CHECK(WaitForSingleObject(th, 5000) == WAIT_OBJECT_0); CloseHandle(th);

    g_ready = 0;
    AcquireSRWLockShared(&g_srw);
    th = CreateThread(NULL, 0, cv_producer, NULL, 0, NULL);
    while (!g_ready) CHECK(SleepConditionVariableSRW(&g_cv, &g_srw, 5000, CONDITION_VARIABLE_LOCKMODE_SHARED));
    ReleaseSRWLockShared(&g_srw);
    CHECK(WaitForSingleObject(th, 5000) == WAIT_OBJECT_0); CloseHandle(th);

    AcquireSRWLockExclusive(&g_srw);
    ULONGLONG t0 = now_ms();
    CHECK(!SleepConditionVariableSRW(&g_cv, &g_srw, 30, 0));
    CHECK(now_ms() - t0 >= 25);
    CHECK(GetLastError() == ERROR_TIMEOUT);
    ReleaseSRWLockExclusive(&g_srw);

    CRITICAL_SECTION cs; CONDITION_VARIABLE cv2;
    InitializeCriticalSection(&cs); InitializeConditionVariable(&cv2);
    EnterCriticalSection(&cs);
    CHECK(!SleepConditionVariableCS(&cv2, &cs, 20));
    LeaveCriticalSection(&cs);
    DeleteCriticalSection(&cs);

    /* two readers overlap; a writer cannot get in while they hold it */
    HANDLE rs[2];
    g_readers_go = CreateEventA(NULL, TRUE, FALSE, NULL);          /* manual-reset */
    CHECK(g_readers_go != NULL);
    for (int i = 0; i < 2; i++) rs[i] = CreateThread(NULL, 0, reader, NULL, 0, NULL);
    for (int spins = 0; g_readers_in != 2 && spins < 5000; spins++) Sleep(1);
    CHECK(g_readers_in == 2);
    CHECK(!TryAcquireSRWLockExclusive(&g_srw));
    CHECK(TryAcquireSRWLockShared(&g_srw)); ReleaseSRWLockShared(&g_srw);
    SetEvent(g_readers_go);
    CHECK(WaitForMultipleObjects(2, rs, TRUE, 5000) == WAIT_OBJECT_0);
    CloseHandle(g_readers_go);
    CHECK(TryAcquireSRWLockExclusive(&g_srw)); ReleaseSRWLockExclusive(&g_srw);
    for (int i = 0; i < 2; i++) CloseHandle(rs[i]);
}

/* ---- one-time initialisation --------------------------------------------- */
static INIT_ONCE     g_once = INIT_ONCE_STATIC_INIT;
static volatile LONG g_once_runs;
static volatile LONG g_once_seen;
static HANDLE        g_once_go;

static BOOL CALLBACK once_cb(PINIT_ONCE once, PVOID parameter, PVOID* context)
{
    (void)once;
    InterlockedIncrement(&g_once_runs);
    Sleep(40);                        /* long enough for the others to pile up */
    *context = parameter;
    return TRUE;
}

static DWORD WINAPI once_worker(LPVOID a)
{
    (void)a;
    WaitForSingleObject(g_once_go, 5000);
    PVOID ctx = NULL;
    if (InitOnceExecuteOnce(&g_once, once_cb, (PVOID)(intptr_t)0x1234, &ctx) &&
        ctx == (PVOID)(intptr_t)0x1234)
        InterlockedIncrement(&g_once_seen);
    return 0;
}

/* Fails twice, then succeeds. A failed callback leaves the INIT_ONCE where it
 * was, which is the half of the contract pthread_once cannot express. */
static BOOL CALLBACK once_fail_cb(PINIT_ONCE once, PVOID parameter, PVOID* context)
{
    (void)once;
    *context = (PVOID)(intptr_t)9;                     /* ignored on failure */
    return InterlockedIncrement((volatile LONG*)parameter) >= 3;
}

static void test_init_once(void)
{
    INIT_ONCE    local = INIT_ONCE_STATIC_INIT;
    volatile LONG tries = 0;
    PVOID        ctx = (PVOID)(intptr_t)-1;

    CHECK(!InitOnceExecuteOnce(&local, once_fail_cb, (PVOID)&tries, &ctx));
    CHECK(!InitOnceExecuteOnce(&local, once_fail_cb, (PVOID)&tries, &ctx));
    CHECK(ctx == (PVOID)(intptr_t)-1);                 /* untouched by a failure */
    CHECK(InitOnceExecuteOnce(&local, once_fail_cb, (PVOID)&tries, &ctx));
    CHECK(tries == 3 && ctx == (PVOID)(intptr_t)9);
    CHECK(InitOnceExecuteOnce(&local, once_fail_cb, (PVOID)&tries, &ctx));
    CHECK(tries == 3);                                 /* done: never runs again */

    /* the runner's shape: four threads race one lazy initialisation */
    g_once_go = CreateEventA(NULL, TRUE, FALSE, NULL);           /* manual-reset */
    CHECK(g_once_go != NULL);
    HANDLE th[4];
    for (int i = 0; i < 4; i++) th[i] = CreateThread(NULL, 0, once_worker, NULL, 0, NULL);
    Sleep(30);
    SetEvent(g_once_go);
    CHECK(WaitForMultipleObjects(4, th, TRUE, 5000) == WAIT_OBJECT_0);
    CHECK(g_once_runs == 1);                           /* exactly one callback */
    CHECK(g_once_seen == 4);                           /* everyone got the context */
    for (int i = 0; i < 4; i++) CloseHandle(th[i]);
    CloseHandle(g_once_go);

    ctx = NULL;
    CHECK(InitOnceExecuteOnce(&g_once, once_cb, NULL, &ctx));
    CHECK(g_once_runs == 1 && ctx == (PVOID)(intptr_t)0x1234);   /* the stored one */

    INIT_ONCE reinit;
    InitOnceInitialize(&reinit);
    CHECK(reinit.Ptr == NULL);
    CHECK(!InitOnceExecuteOnce(&reinit, NULL, NULL, NULL) &&
          GetLastError() == ERROR_INVALID_PARAMETER);
}

/* ---- waitable timers ----------------------------------------------------- */
static void test_timers(void)
{
    HANDLE t = CreateWaitableTimerA(NULL, FALSE, NULL);
    CHECK(t != NULL);
    CHECK(WaitForSingleObject(t, 0) == WAIT_TIMEOUT);          /* not set */
    LARGE_INTEGER due; due.QuadPart = -30 * 10000LL;           /* 30 ms, relative */
    CHECK(SetWaitableTimer(t, &due, 0, NULL, NULL, FALSE));
    ULONGLONG t0 = now_ms();
    CHECK(WaitForSingleObject(t, INFINITE) == WAIT_OBJECT_0);
    CHECK(now_ms() - t0 >= 25);
    CHECK(WaitForSingleObject(t, 0) == WAIT_TIMEOUT);          /* one-shot, consumed */

    CHECK(SetWaitableTimer(t, &due, 20, NULL, NULL, FALSE));   /* periodic */
    t0 = now_ms();
    CHECK(WaitForSingleObject(t, 5000) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(t, 5000) == WAIT_OBJECT_0);
    CHECK(now_ms() - t0 >= 45);
    CHECK(CancelWaitableTimer(t));
    CHECK(WaitForSingleObject(t, 30) == WAIT_TIMEOUT);

    HANDLE e = CreateEventA(NULL, FALSE, FALSE, NULL);
    HANDLE hs[2] = { e, t };
    CHECK(SetWaitableTimer(t, &due, 0, NULL, NULL, FALSE));
    t0 = now_ms();
    CHECK(WaitForMultipleObjects(2, hs, FALSE, 5000) == WAIT_OBJECT_0 + 1);   /* the timer, when due */
    CHECK(now_ms() - t0 >= 25);
    CloseHandle(e); CloseHandle(t);
}

/* ---- timing -------------------------------------------------------------- */
static void test_timing(void)
{
    ULONGLONG a = GetTickCount64();
    Sleep(20);
    ULONGLONG b = GetTickCount64();
    CHECK(b - a >= 15 && b - a < 2000);
    CHECK((DWORD)b == GetTickCount() || GetTickCount() >= (DWORD)b);
    LARGE_INTEGER f, c1, c2;
    CHECK(QueryPerformanceFrequency(&f) && f.QuadPart > 0);
    CHECK(QueryPerformanceCounter(&c1));
    Sleep(5);
    CHECK(QueryPerformanceCounter(&c2) && c2.QuadPart > c1.QuadPart);
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULONGLONG t = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    CHECK(t > 132000000000000000ull);                          /* after 2019 */
    CHECK(timeBeginPeriod(1) == TIMERR_NOERROR && timeEndPeriod(1) == TIMERR_NOERROR);
    CHECK(SwitchToThread() == TRUE);
    Sleep(0);
    LARGE_INTEGER li; li.QuadPart = 0x123456789LL;
    CHECK(li.LowPart == 0x23456789u && li.HighPart == 1);
}

/* ---- virtual memory ------------------------------------------------------ */
static void test_virtual_memory(void)
{
    SYSTEM_INFO si; GetSystemInfo(&si);
    CHECK(si.dwPageSize >= 4096 && (si.dwPageSize & (si.dwPageSize - 1)) == 0);
    CHECK(si.dwNumberOfProcessors >= 1);
    size_t pg = si.dwPageSize, sz = 4u << 20;

    unsigned char* r = (unsigned char*)VirtualAlloc(NULL, sz, MEM_RESERVE, PAGE_NOACCESS);
    CHECK(r != NULL);
    unsigned char* c = (unsigned char*)VirtualAlloc(r + pg, pg * 2, MEM_COMMIT, PAGE_READWRITE);
    CHECK(c == r + pg);
    memset(c, 0xAB, pg * 2);
    CHECK(c[pg] == 0xAB);
    DWORD old = 0;
    CHECK(VirtualProtect(c, pg, PAGE_READONLY, &old));
    CHECK(c[0] == 0xAB);
    CHECK(VirtualProtect(c, pg, PAGE_READWRITE, &old));
    c[1] = 1;
    CHECK(VirtualFree(c, pg, MEM_DECOMMIT));
    CHECK(VirtualFree(r, 0, MEM_RELEASE));
    CHECK(!VirtualFree(r, 0, MEM_RELEASE));                    /* already gone */

    unsigned char* rc = (unsigned char*)VirtualAlloc(NULL, 64 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(rc != NULL);
    memset(rc, 1, 64 * 1024);
    CHECK(VirtualFree(rc, 0, MEM_RELEASE));

    /* a fixed-address reservation over live memory fails, never displaces */
    unsigned char* rr = (unsigned char*)VirtualAlloc(NULL, sz, MEM_RESERVE, PAGE_NOACCESS);
    CHECK(rr != NULL);
    CHECK(VirtualAlloc(rr, pg, MEM_RESERVE, PAGE_NOACCESS) == NULL);
    CHECK(VirtualFree(rr, 0, MEM_RELEASE));
}

/* ---- stopping a running thread ------------------------------------------- */
static volatile LONG g_spin_counter;
static volatile LONG g_spin_stop;
static volatile LONG g_spin_tid;

/* Holds no lock of any kind while it runs, which is what makes it safe to
 * freeze: a thread stopped inside malloc would take the process with it, on
 * Windows exactly as here. */
static DWORD WINAPI spin_worker(LPVOID a)
{
    (void)a;
    InterlockedExchange(&g_spin_tid, (LONG)GetCurrentThreadId());
    while (!g_spin_stop) {
        InterlockedIncrement(&g_spin_counter);
        SwitchToThread();
    }
    return 0;
}

static void test_thread_control(void)
{
    g_spin_stop = 0; g_spin_counter = 0; g_spin_tid = 0;
    HANDLE h = CreateThread(NULL, 0, spin_worker, NULL, 0, NULL);
    CHECK(h != NULL);
    for (int i = 0; i < 1000 && (g_spin_counter == 0 || g_spin_tid == 0); i++) Sleep(2);
    CHECK(g_spin_counter > 0 && g_spin_tid != 0);

    /* frozen: the counter stops moving */
    CHECK(SuspendThread(h) == 0);
    LONG at_suspend = g_spin_counter;
    Sleep(50);
    CHECK(g_spin_counter == at_suspend);

    CONTEXT ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.ContextFlags = CONTEXT_FULL;
    CHECK(GetThreadContext(h, &ctx));
    CHECK(ctx.Pc != 0 && ctx.Sp != 0);
    CHECK(SetThreadContext(h, &ctx));            /* write the same state back */

    /* nesting: two suspends need two resumes */
    CHECK(SuspendThread(h) == 1);
    CHECK(ResumeThread(h) == 2);
    Sleep(30);
    CHECK(g_spin_counter == at_suspend);         /* still held by the first */
    CHECK(ResumeThread(h) == 1);
    for (int i = 0; i < 1000 && g_spin_counter == at_suspend; i++) Sleep(2);
    CHECK(g_spin_counter > at_suspend);
    CHECK(ResumeThread(h) == 0);                 /* already running */
    CHECK(!SetThreadContext(h, &ctx));           /* not suspended: no frame to set */

    /* the same thread through OpenThread, by the id it published itself */
    HANDLE oh = OpenThread(THREAD_ALL_ACCESS, FALSE, (DWORD)g_spin_tid);
    CHECK(oh != NULL);
    CHECK(SuspendThread(oh) == 0);
    CHECK(GetThreadContext(oh, &ctx) && ctx.Sp != 0);
    CHECK(ResumeThread(oh) == 1);
    CHECK(CloseHandle(oh));
    CHECK(OpenThread(THREAD_ALL_ACCESS, FALSE, 0) == NULL);

    /* the snapshot sees this thread and the worker, and nothing else's */
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    CHECK(snap != INVALID_HANDLE_VALUE);
    THREADENTRY32 te;
    te.dwSize = sizeof te;
    int seen_self = 0, seen_worker = 0, foreign = 0, count = 0;
    DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te)) do {
        count++;
        if (te.th32OwnerProcessID != pid) foreign++;
        if (te.th32ThreadID == self)              seen_self = 1;
        if (te.th32ThreadID == (DWORD)g_spin_tid) seen_worker = 1;
    } while (Thread32Next(snap, &te));
    CHECK(count >= 2 && foreign == 0);
    CHECK(seen_self && seen_worker);
    CHECK(te.dwSize == sizeof te);
    CHECK(CloseHandle(snap));

    /* a real handle to the calling thread, which the shim never created */
    HANDLE me = NULL;
    CHECK(DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                          &me, 0, FALSE, DUPLICATE_SAME_ACCESS));
    CHECK(me != NULL && me != GetCurrentThread());
    CHECK(SuspendThread(me) == (DWORD)-1);       /* suspending yourself is refused */

    FILETIME born, died, kern, user;
    CHECK(GetThreadTimes(me, &born, &died, &kern, &user));
    ULONGLONG t_born = ((ULONGLONG)born.dwHighDateTime << 32) | born.dwLowDateTime;
    CHECK(t_born > 132000000000000000ull);       /* after 2019, like the wall clock */
    CHECK(died.dwLowDateTime == 0 && died.dwHighDateTime == 0);   /* still running */
    volatile double burn = 0;
    for (int i = 0; i < 3000000; i++) burn += (double)i;
    CHECK(GetThreadTimes(me, &born, &died, &kern, &user));
    ULONGLONG cpu = (((ULONGLONG)kern.dwHighDateTime << 32) | kern.dwLowDateTime)
                  + (((ULONGLONG)user.dwHighDateTime << 32) | user.dwLowDateTime);
    CHECK(cpu > 0 && burn > 0);
    CHECK(CloseHandle(me));

    InterlockedExchange(&g_spin_stop, 1);
    CHECK(WaitForSingleObject(h, 5000) == WAIT_OBJECT_0);
    CHECK(SuspendThread(h) == (DWORD)-1);        /* finished: nothing to stop */
    CHECK(CloseHandle(h));
}

/* ---- querying the address space ------------------------------------------ */
static void test_virtual_query(void)
{
    SYSTEM_INFO si; GetSystemInfo(&si);
    size_t pg = si.dwPageSize;
    MEMORY_BASIC_INFORMATION mbi;
    DWORD old = 0;

    CHECK(VirtualQuery(&si, &mbi, sizeof mbi - 1) == 0);       /* buffer too small */

    unsigned char* r = (unsigned char*)VirtualAlloc(NULL, 4u << 20, MEM_RESERVE, PAGE_NOACCESS);
    CHECK(r != NULL);
    CHECK(VirtualQuery(r, &mbi, sizeof mbi) == sizeof mbi);
    CHECK(mbi.State == MEM_RESERVE && mbi.Protect == PAGE_NOACCESS);
    CHECK(mbi.AllocationBase == r && mbi.AllocationProtect == PAGE_NOACCESS);
    CHECK((uintptr_t)mbi.BaseAddress <= (uintptr_t)r);
    CHECK((uintptr_t)mbi.BaseAddress + mbi.RegionSize >= (uintptr_t)r + pg);

    unsigned char* c = (unsigned char*)VirtualAlloc(r + pg, pg * 2, MEM_COMMIT, PAGE_READWRITE);
    CHECK(c == r + pg);
    memset(c, 0xCD, pg * 2);
    CHECK(VirtualQuery(c, &mbi, sizeof mbi) == sizeof mbi);
    CHECK(mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE);
    CHECK(mbi.BaseAddress == c && mbi.RegionSize >= pg * 2);
    CHECK(mbi.AllocationBase == r);              /* the reservation it came from */
    CHECK(mbi.Type == MEM_PRIVATE);

    int on_stack = 0;
    CHECK(VirtualQuery(&on_stack, &mbi, sizeof mbi) == sizeof mbi);
    CHECK(mbi.State == MEM_COMMIT);
    CHECK((uintptr_t)mbi.BaseAddress <= (uintptr_t)&on_stack);

    CHECK(IsBadReadPtr(NULL, 4) && IsBadWritePtr(NULL, 4));
    CHECK(!IsBadReadPtr(NULL, 0));                             /* zero length is good */
    CHECK(!IsBadReadPtr(&on_stack, sizeof on_stack));
    CHECK(!IsBadWritePtr(&on_stack, sizeof on_stack));
    CHECK(IsBadReadPtr(r, pg) && IsBadWritePtr(r, pg));        /* reserved: no access */
    CHECK(!IsBadReadPtr(c, pg * 2) && !IsBadWritePtr(c, pg * 2));
    CHECK(!IsBadReadPtr(c + pg - 4, 4));
    CHECK(IsBadReadPtr(c + pg * 2 - 4, 8));                    /* runs off the commit */
    CHECK(VirtualProtect(c, pg, PAGE_READONLY, &old));
    CHECK(!IsBadReadPtr(c, pg) && IsBadWritePtr(c, pg));
    CHECK(VirtualProtect(c, pg, PAGE_READWRITE, &old));
    CHECK(!IsBadCodePtr((LPCVOID)(uintptr_t)&test_virtual_query));

    CHECK(VirtualFree(r, 0, MEM_RELEASE));
    CHECK(VirtualQuery(r, &mbi, sizeof mbi) == sizeof mbi);    /* the gap it left */
    CHECK(mbi.State == MEM_FREE && mbi.BaseAddress == r);
    CHECK(mbi.RegionSize >= pg && mbi.AllocationBase == NULL);
    CHECK(IsBadReadPtr(r, pg));
}

/* ---- vectored exception handlers ----------------------------------------- */
/* Everything the handler reports is volatile. The fault is a volatile store,
 * not a call, so an optimiser is entitled to carry a plain global's value
 * across it -- and at -O2 it does, folding the zero these are cleared to
 * straight into the comparison below. */
static unsigned char*  g_veh_page;
static size_t          g_veh_len;
static volatile LONG   g_veh_hits;
static volatile ULONG_PTR g_veh_at;
static volatile ULONG_PTR g_veh_rw;
static volatile LONG   g_search_hits;
static volatile LONG   g_filter_hits;
static volatile LONG   g_after_fault;
static sigjmp_buf      g_escape;

/* Makes the store that faulted succeed: open the page and re-run it. */
static LONG CALLBACK veh_fixup(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        ep->ExceptionRecord->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;
    ULONG_PTR at = ep->ExceptionRecord->ExceptionInformation[1];
    if (at < (ULONG_PTR)g_veh_page || at >= (ULONG_PTR)g_veh_page + g_veh_len)
        return EXCEPTION_CONTINUE_SEARCH;
    g_veh_at = at;
    g_veh_rw = ep->ExceptionRecord->ExceptionInformation[0];
    InterlockedIncrement(&g_veh_hits);
    DWORD old;
    VirtualProtect(g_veh_page, g_veh_len, PAGE_READWRITE, &old);
    return EXCEPTION_CONTINUE_EXECUTION;
}

static LONG CALLBACK veh_decline(EXCEPTION_POINTERS* ep)
{
    (void)ep;
    InterlockedIncrement(&g_search_hits);
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI filter_escape(EXCEPTION_POINTERS* ep)
{
    (void)ep;
    InterlockedIncrement(&g_filter_hits);
    siglongjmp(g_escape, 1);
}

static void test_exceptions(void)
{
    SYSTEM_INFO si; GetSystemInfo(&si);
    g_veh_len = si.dwPageSize;

    /* a handler that repairs the fault and continues */
    g_veh_page = (unsigned char*)VirtualAlloc(NULL, g_veh_len,
                                              MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    CHECK(g_veh_page != NULL);
    PVOID hfix = AddVectoredExceptionHandler(1, veh_fixup);
    CHECK(hfix != NULL);
    g_veh_hits = 0; g_veh_at = 0; g_veh_rw = 0;
    volatile unsigned char* target = g_veh_page + 8;
    *target = 0x5A;
    CHECK(g_veh_hits == 1);                          /* once, not a loop */
    CHECK(g_veh_at == (ULONG_PTR)(g_veh_page + 8));
    CHECK(g_veh_rw == 1);                            /* a write */
    CHECK(*target == 0x5A);                          /* and it landed */
    *target = 0x77;
    CHECK(g_veh_hits == 1 && *target == 0x77);       /* page open now: no fault */
    CHECK(RemoveVectoredExceptionHandler(hfix) != 0);
    CHECK(RemoveVectoredExceptionHandler(hfix) == 0);          /* already gone */
    CHECK(RemoveVectoredExceptionHandler((PVOID)&si) == 0);    /* never was ours */
    CHECK(VirtualFree(g_veh_page, 0, MEM_RELEASE));

    /* a handler that declines falls through to the unhandled filter */
    g_veh_page = (unsigned char*)VirtualAlloc(NULL, g_veh_len,
                                              MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    CHECK(g_veh_page != NULL);
    PVOID hdecline = AddVectoredExceptionHandler(1, veh_decline);
    CHECK(hdecline != NULL);
    CHECK(SetUnhandledExceptionFilter(filter_escape) == NULL);
    g_search_hits = 0; g_filter_hits = 0; g_after_fault = 0;
    if (sigsetjmp(g_escape, 1) == 0) {
        *(volatile unsigned char*)g_veh_page = 1;
        InterlockedIncrement(&g_after_fault);        /* must not be reached */
    }
    CHECK(g_search_hits == 1);
    CHECK(g_filter_hits == 1);
    CHECK(g_after_fault == 0);
    CHECK(SetUnhandledExceptionFilter(NULL) == filter_escape);
    CHECK(RemoveVectoredExceptionHandler(hdecline) != 0);
    CHECK(VirtualFree(g_veh_page, 0, MEM_RELEASE));
}

/* ---- symbols ------------------------------------------------------------- */
static void test_symbols(void)
{
    CHECK(SymInitialize(GetCurrentProcess(), NULL, TRUE));

    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO* sym = (SYMBOL_INFO*)buf;
    memset(buf, 0, sizeof buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen   = 255;
    DWORD64 disp = 0xDEAD;
    BOOL ok = SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)&test_symbols, &disp, sym);
    /* Whether a name comes back at all is the platform's business -- dladdr on
     * Linux sees only exported symbols -- but where one does it is this one. */
    CHECK(!ok || strstr(sym->Name, "test_symbols") != NULL);
    CHECK(!ok || (sym->Address != 0 && disp < 0x10000));
    CHECK(!ok || sym->NameLen == (ULONG)strlen(sym->Name));
    CHECK(ok || disp == 0);                          /* a miss still zeroes it */
    CHECK(sym->MaxNameLen == 255);                   /* the caller's cap is not touched */
    CHECK(!SymFromAddr(GetCurrentProcess(), (DWORD64)0x10, &disp, sym));   /* unmapped */
    CHECK(!SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)&test_symbols, &disp, NULL));
    CHECK(SymCleanup(GetCurrentProcess()));
}

/* ---- module handles and names -------------------------------------------- */
static void test_module_names(void)
{
    /* A load base, not a Win32 handle: subtracting it from a frame address is
     * the only thing it is good for, and both spellings must give the same one. */
    HMODULE self = GetModuleHandleA(NULL);
    CHECK(self != NULL);
    CHECK(GetModuleHandleW(NULL) == self);

    HMODULE from_a = NULL, from_w = NULL;
    CHECK(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             (LPCSTR)(uintptr_t)&test_module_names, &from_a));
    CHECK(from_a == self);
    CHECK(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             (LPCWSTR)(uintptr_t)&test_module_names, &from_w));
    CHECK(from_w == self);
    CHECK(!GetModuleHandleExW(0, (LPCWSTR)(uintptr_t)0x10, &from_w) && from_w == NULL);
    CHECK(!GetModuleHandleExA(0, (LPCSTR)(uintptr_t)&self, NULL));   /* nowhere to answer */

    char    narrow[512];
    wchar_t wide[512];
    DWORD na = GetModuleFileNameA(NULL, narrow, (DWORD)sizeof narrow);
    DWORD nw = GetModuleFileNameW(NULL, wide, 512);
    CHECK(na > 0 && strlen(narrow) == na);
    CHECK(nw == na && wcslen(wide) == nw);
    int widened = 1;
    for (DWORD i = 0; i < na; i++)
        if (wide[i] != (wchar_t)(unsigned char)narrow[i]) widened = 0;
    CHECK(widened);                                  /* one wchar_t per byte */
    CHECK(GetModuleFileNameW(self, wide, 512) == nw);   /* by handle: same image */

    wchar_t clipped[4];
    CHECK(GetModuleFileNameW(NULL, clipped, 4) == 3 && wcslen(clipped) == 3);
    CHECK(GetModuleFileNameW(NULL, wide, 0) == 0);
    CHECK(GetModuleFileNameA(NULL, narrow, 0) == 0);
}

/* ---- directories and process information --------------------------------- */
static void test_directories_and_process(void)
{
    /* Named for this process: the suites run side by side on a build machine. */
    char dir[256], deep[512];
    snprintf(dir, sizeof dir, "/tmp/ps3recomp_compat_%u", (unsigned)GetCurrentProcessId());
    rmdir(dir);                                       /* a previous run's, if any */

    CHECK(CreateDirectoryA(dir, NULL));
    struct _stat st;
    CHECK(_stat(dir, &st) == 0 && (st.st_mode & _S_IFDIR));
    CHECK(!CreateDirectoryA(dir, NULL) && GetLastError() == ERROR_ALREADY_EXISTS);

    snprintf(deep, sizeof deep, "%s/absent/leaf", dir);
    CHECK(!CreateDirectoryA(deep, NULL) && GetLastError() == ERROR_PATH_NOT_FOUND);
    CHECK(!CreateDirectoryA("/dev/null/leaf", NULL) &&
          GetLastError() == ERROR_PATH_NOT_FOUND);     /* a component is not a directory */
    CHECK(!CreateDirectoryA(NULL, NULL) && GetLastError() == ERROR_INVALID_PARAMETER);
    CHECK(rmdir(dir) == 0);

    /* Accepted and ignored: no POSIX host has an EcoQoS to opt out of. */
    PROCESS_POWER_THROTTLING_STATE pt;
    memset(&pt, 0, sizeof pt);
    pt.Version     = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    pt.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    pt.StateMask   = 0;
    CHECK(SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling,
                                &pt, (DWORD)sizeof pt));
}

/* ---- MSVC CRT and intrinsic spellings ------------------------------------ */
static __declspec(thread) int t_tls_probe;

static void test_msvc_compat(void)
{
    CHECK(_byteswap_ushort(0x1122) == 0x2211);
    CHECK(_byteswap_ulong(0x11223344u) == 0x44332211u);
    CHECK(_byteswap_uint64(1ull) == 1ull << 56);
    uint32_t idx = 99;
    CHECK(_BitScanForward(&idx, 0x80) && idx == 7);
    CHECK(_BitScanReverse(&idx, 0x8F) && idx == 7);
    CHECK(!_BitScanForward(&idx, 0));
    CHECK(_BitScanReverse64(&idx, 1ull << 40) && idx == 40);
    CHECK(_BitScanForward64(&idx, 1ull << 33) && idx == 33);
    CHECK(_rotl(0x80000001u, 1) == 3u && _rotr(3u, 1) == 0x80000001u);
    CHECK(_rotl64(1ull << 63, 1) == 1ull);
    CHECK(__popcnt(0xFFu) == 8 && __popcnt64(0xFFFFull) == 16);
    CHECK(__lzcnt(1u) == 31 && __lzcnt(0u) == 32);
    unsigned long long hi = 0;
    CHECK(_umul128(1ull << 63, 4, &hi) == 0 && hi == 2);
    CHECK(__umulh(1ull << 63, 4) == 2);

    char buf[8];
    CHECK(_stricmp("AbC", "aBc") == 0 && _strnicmp("AbCd", "aBcX", 3) == 0);
    CHECK(_snprintf(buf, sizeof buf, "%d", 42) == 2 && strcmp(buf, "42") == 0);
    CHECK(sprintf_s(buf, sizeof buf, "%s", "xy") == 2 && strcmp(buf, "xy") == 0);
    CHECK(strcpy_s(buf, sizeof buf, "hello") == 0 && strcmp(buf, "hello") == 0);
    CHECK(strcpy_s(buf, 3, "hello") == ERANGE && buf[0] == '\0');
    CHECK(strncpy_s(buf, sizeof buf, "abcdef", 3) == 0 && strcmp(buf, "abc") == 0);
    CHECK(strcat_s(buf, sizeof buf, "de") == 0 && strcmp(buf, "abcde") == 0);
    char* dup = _strdup("dup"); CHECK(dup && strcmp(dup, "dup") == 0); free(dup);

    FILE* f = NULL;
    CHECK(fopen_s(&f, "/nonexistent-dir/x", "rb") != 0 && f == NULL);
    CHECK(fopen_s(&f, "/dev/null", "rb") == 0 && f != NULL);
    if (f) { CHECK(_fseeki64(f, 0, SEEK_SET) == 0 && _ftelli64(f) == 0); fclose(f); }
    struct _stat st;
    CHECK(_stat("/", &st) == 0 && (st.st_mode & _S_IFDIR));
    CHECK(_access("/", 0) == 0);
    CHECK(_putenv_s("PS3_COMPAT_TEST", "1") == 0);
    char env[16];
    CHECK(GetEnvironmentVariableA("PS3_COMPAT_TEST", env, sizeof env) == 1 && strcmp(env, "1") == 0);
    CHECK(GetEnvironmentVariableA("PS3_COMPAT_TEST_MISSING", env, sizeof env) == 0);
    time_t now = time(NULL); struct tm tmv;
    CHECK(localtime_s(&tmv, &now) == 0 && tmv.tm_year > 100);
    void* al = _aligned_malloc(100, 64);
    CHECK(al && ((uintptr_t)al & 63) == 0); _aligned_free(al);
    t_tls_probe = 5;
    CHECK(t_tls_probe == 5);
    CHECK(_countof(buf) == 8);
    CHECK((void*)_ReturnAddress() != NULL);
}

int main(void)
{
    test_interlocked();
    test_threads_and_cs();
    test_thread_lifecycle();
    test_events();
    test_semaphores();
    test_wait_on_address();
    test_srw_and_cv();
    test_init_once();
    test_timers();
    test_timing();
    test_virtual_memory();
    test_thread_control();
    test_virtual_query();
    test_exceptions();
    test_symbols();
    test_module_names();
    test_directories_and_process();
    test_msvc_compat();
    printf("win32_compat tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
