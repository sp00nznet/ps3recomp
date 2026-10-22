/* sys_mutex_destroy refuses a mutex that is held or has a thread blocked
 * acquiring it, with EBUSY, as lv2 does. It used to destroy the host mutex
 * under the blocked thread; the next sys_mutex_create in that slot then
 * initialised a new mutex over it, and the blocked thread waited on the dead
 * one for good.
 *
 * The blocked acquires are counted in sys_mutex_info.waiters: sys_mutex_lock
 * (both timeouts give the count back) and sys_cond_wait's re-lock. */
#include "runtime/platform/win32_compat.h"
#include "runtime/syscalls/sys_cond.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t* vm_base;
static uint32_t mid, cid;
static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)
static int32_t call(int64_t (*f)(ppu_context*), ppu_context* c, uint64_t a, uint64_t b, uint64_t d) {
    c->gpr[3]=a; c->gpr[4]=b; c->gpr[5]=d;
    return (int32_t)f(c);
}
static uint32_t be_read(unsigned off) {
    uint8_t* p=vm_base+off;
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static int waiters(void) { return __atomic_load_n(&g_sys_mutexes[mid-1].waiters, __ATOMIC_SEQ_CST); }
static int wait_waiters(int want) {
    for (int i=0;i<2000 && waiters()!=want;i++) Sleep(1);
    return waiters()==want;
}
/* Thread 2: lock (blocking, or with the timeout in raw), record, unlock. */
static volatile int32_t s_rc=-1;
static DWORD WINAPI locker(void* raw) {
    ppu_context c={0}; c.thread_id=2;
    s_rc=call(sys_mutex_lock,&c,mid,(uint64_t)(uintptr_t)raw,0);
    if (s_rc==CELL_OK) call(sys_mutex_unlock,&c,mid,0,0);
    return 0;
}
static DWORD WINAPI signaler(void* raw) {
    (void)raw;
    ppu_context c={0}; c.thread_id=3;
    while (call(sys_mutex_lock,&c,mid,0,0)!=CELL_OK) Sleep(1);
    call(sys_cond_signal,&c,cid,0,0);
    /* Hold it: the waiter is now blocked re-acquiring. */
    CHECK(wait_waiters(1));
    CHECK(call(sys_mutex_destroy,&c,mid,0,0)==(int32_t)CELL_EBUSY);
    call(sys_mutex_unlock,&c,mid,0,0);
    return 0;
}
int main(void) {
    vm_base=calloc(1,65536);
    ppu_context c={0}; c.thread_id=1;
    vm_base[0x103]=1; vm_base[0x107]=0x20;     /* FIFO, not recursive */
    CHECK(call(sys_mutex_create,&c,0x200,0x100,0)==CELL_OK);
    mid=be_read(0x200);

    /* 1. Held: EBUSY. */
    CHECK(call(sys_mutex_lock,&c,mid,0,0)==CELL_OK);
    CHECK(call(sys_mutex_destroy,&c,mid,0,0)==(int32_t)CELL_EBUSY);

    /* 2. Held, with a thread blocked on it: EBUSY, and the blocked thread
     *    gets the mutex once it is released. */
    HANDLE h=CreateThread(NULL,0,locker,NULL,0,NULL);
    CHECK(wait_waiters(1));
    CHECK(call(sys_mutex_destroy,&c,mid,0,0)==(int32_t)CELL_EBUSY);
    CHECK(call(sys_mutex_unlock,&c,mid,0,0)==CELL_OK);
    CHECK(WaitForSingleObject(h,2000)==WAIT_OBJECT_0); CloseHandle(h);
    CHECK(s_rc==CELL_OK && waiters()==0);

    /* 3. A timed lock that times out gives its count back. */
    CHECK(call(sys_mutex_lock,&c,mid,0,0)==CELL_OK);
    s_rc=-1;
    h=CreateThread(NULL,0,locker,(void*)(uintptr_t)20000,0,NULL);
    CHECK(WaitForSingleObject(h,2000)==WAIT_OBJECT_0); CloseHandle(h);
    CHECK(s_rc==(int32_t)CELL_ETIMEDOUT && waiters()==0);
    CHECK(call(sys_mutex_unlock,&c,mid,0,0)==CELL_OK);

    /* 4. A thread blocked on the host lock while the guest count is 0 (the
     *    window in which a released mutex passes to a waiter): EBUSY. */
    sys_mutex_info* m=&g_sys_mutexes[mid-1];
#ifdef _WIN32
    EnterCriticalSection(&m->cs);
#else
    pthread_mutex_lock(&m->mtx);
#endif
    h=CreateThread(NULL,0,locker,NULL,0,NULL);
    CHECK(wait_waiters(1) && m->lock_count==0);
    CHECK(call(sys_mutex_destroy,&c,mid,0,0)==(int32_t)CELL_EBUSY);
#ifdef _WIN32
    LeaveCriticalSection(&m->cs);
#else
    pthread_mutex_unlock(&m->mtx);
#endif
    CHECK(WaitForSingleObject(h,2000)==WAIT_OBJECT_0); CloseHandle(h);

    /* 5. sys_cond_wait's re-lock counts as a waiter. */
    CHECK(call(sys_cond_create,&c,0x204,mid,0)==CELL_OK);
    cid=be_read(0x204);
    CHECK(call(sys_mutex_lock,&c,mid,0,0)==CELL_OK);
    h=CreateThread(NULL,0,signaler,NULL,0,NULL);
    CHECK(call(sys_cond_wait,&c,cid,0,0)==CELL_OK);
    CHECK(WaitForSingleObject(h,2000)==WAIT_OBJECT_0); CloseHandle(h);
    CHECK(call(sys_mutex_unlock,&c,mid,0,0)==CELL_OK);
    CHECK(call(sys_cond_destroy,&c,cid,0,0)==CELL_OK);

    /* 6. Free and uncontended: destroyed. */
    CHECK(waiters()==0);
    CHECK(call(sys_mutex_destroy,&c,mid,0,0)==CELL_OK);
    CHECK(call(sys_mutex_destroy,&c,mid,0,0)==(int32_t)CELL_ESRCH);

    printf("mutex destroy: %s\n",failures?"FAIL":"PASS");
    return failures?1:0;
}
