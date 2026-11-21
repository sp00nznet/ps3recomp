/*
 * ps3_guest_host_thread_start: a detached host thread for running guest code,
 * made the way sys_ppu_thread_create makes a guest PPU thread's.
 *
 * Each recompiled guest call is a real host call, so a guest call chain nests
 * on the HOST stack, and the 1 MB Windows default (512 KB on Darwin) is
 * nowhere near enough. Any host thread that calls into the guest -- the
 * PPU threads, and runtime threads that deliver guest handlers -- gets the
 * same 256 MB reservation (committed lazily by the OS), and on Windows the
 * same stack guarantee, so an overflow still reaches the handler that names
 * the recursing function. A host that refuses that much address space gets
 * the thread on its default stack: a shallow handler runs fine there.
 */
#ifndef PS3EMU_GUEST_HOST_THREAD_H
#define PS3EMU_GUEST_HOST_THREAD_H

#include <stdlib.h>

#define PS3_GUEST_HOST_STACK_BYTES      (256u * 1024u * 1024u)
#define PS3_GUEST_HOST_STACK_GUARANTEE  (256u * 1024u)

#ifdef _WIN32
#include <windows.h>
#include <process.h>   /* _beginthreadex: per-thread CRT state, as for PPU threads */
#else
#include <pthread.h>
#endif

typedef void (*ps3_guest_host_thread_fn)(void* arg);

struct ps3_guest_host_thread_start_args { ps3_guest_host_thread_fn fn; void* arg; };

#ifdef _WIN32
static unsigned __stdcall ps3_guest_host_thread_main(void* p)
{
    struct ps3_guest_host_thread_start_args a = *(struct ps3_guest_host_thread_start_args*)p;
    free(p);
    { ULONG g = PS3_GUEST_HOST_STACK_GUARANTEE; SetThreadStackGuarantee(&g); }
    a.fn(a.arg);
    return 0;
}
#else
static void* ps3_guest_host_thread_main(void* p)
{
    struct ps3_guest_host_thread_start_args a = *(struct ps3_guest_host_thread_start_args*)p;
    free(p);
    a.fn(a.arg);
    return NULL;
}
#endif

/* 0 once the thread is started, -1 if it could not be. */
static inline int ps3_guest_host_thread_start(ps3_guest_host_thread_fn fn, void* arg)
{
    struct ps3_guest_host_thread_start_args* a =
        (struct ps3_guest_host_thread_start_args*)malloc(sizeof *a);
    if (!a) return -1;
    a->fn = fn;
    a->arg = arg;
#ifdef _WIN32
    HANDLE h = (HANDLE)_beginthreadex(NULL, PS3_GUEST_HOST_STACK_BYTES, ps3_guest_host_thread_main,
                                      a, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!h) { free(a); return -1; }
    CloseHandle(h);
#else
    pthread_t th;
    pthread_attr_t attr;
    int rc = -1;
    pthread_attr_init(&attr);
    if (pthread_attr_setstacksize(&attr, PS3_GUEST_HOST_STACK_BYTES) == 0)
        rc = pthread_create(&th, &attr, ps3_guest_host_thread_main, a);
    pthread_attr_destroy(&attr);
    if (rc != 0) rc = pthread_create(&th, NULL, ps3_guest_host_thread_main, a);
    if (rc != 0) { free(a); return -1; }
    pthread_detach(th);
#endif
    return 0;
}

#endif /* PS3EMU_GUEST_HOST_THREAD_H */
