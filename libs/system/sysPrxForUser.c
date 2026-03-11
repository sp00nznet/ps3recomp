/*
 * ps3recomp - sysPrxForUser HLE stub implementation
 */

#include "sysPrxForUser.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static u64 s_next_thread_id = 1;
static u32 s_next_lwmutex_id = 1;
static u32 s_next_lwcond_id = 1;

/* ---------------------------------------------------------------------------
 * Thread management
 * -----------------------------------------------------------------------*/

/* NID: 0x24A1EA07 */
s32 sys_ppu_thread_create(sys_ppu_thread_t* thread_id,
                          sys_ppu_thread_entry_t entry,
                          u64 arg,
                          s32 priority,
                          u32 stacksize,
                          u64 flags,
                          const char* threadname)
{
    printf("[sysPrxForUser] sys_ppu_thread_create(entry=%p, arg=0x%llX, prio=%d, "
           "stack=0x%X, flags=0x%llX, name='%s')\n",
           (void*)(uintptr_t)entry, (unsigned long long)arg, priority,
           stacksize, (unsigned long long)flags,
           threadname ? threadname : "<null>");

    if (!thread_id)
        return CELL_EFAULT;

    /*
     * TODO: Actually create a host thread that runs the recompiled entry point.
     * For now, assign a unique ID and return success so the caller's
     * control flow proceeds.
     */
    *thread_id = s_next_thread_id++;
    return CELL_OK;
}

/* NID: 0xAFF080A4 */
void sys_ppu_thread_exit(u64 exitcode)
{
    printf("[sysPrxForUser] sys_ppu_thread_exit(code=0x%llX)\n",
           (unsigned long long)exitcode);

    /* TODO: Terminate the calling host thread. */
}

/* NID: 0x1D3EE43E */
s32 sys_ppu_thread_join(sys_ppu_thread_t thread_id, u64* exitstatus)
{
    printf("[sysPrxForUser] sys_ppu_thread_join(id=%llu)\n",
           (unsigned long long)thread_id);

    /* TODO: Wait for the specified host thread to finish. */
    if (exitstatus)
        *exitstatus = 0;

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Process management
 * -----------------------------------------------------------------------*/

/* NID: 0xE6F2C1E7 */
void sys_process_exit(s32 exitcode)
{
    printf("[sysPrxForUser] sys_process_exit(code=%d)\n", exitcode);
    /* TODO: Tear down the emulated process cleanly. */
}

/* NID: 0xE9572E5B */
s32 sys_process_getpid(void)
{
    /* Return a fake PID; games typically just use it for logging. */
    printf("[sysPrxForUser] sys_process_getpid() -> 1001\n");
    return 1001;
}

/* ---------------------------------------------------------------------------
 * Printf / sprintf
 * -----------------------------------------------------------------------*/

/* NID: 0x9FB6228E */
s32 _sys_printf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("[PS3 printf] ");
    int ret = vprintf(fmt, ap);
    va_end(ap);
    return ret;
}

/* NID: 0xFA7F693D */
s32 _sys_sprintf(char* buf, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int ret = vsprintf(buf, fmt, ap);
    va_end(ap);
    return ret;
}

/* ---------------------------------------------------------------------------
 * Lightweight mutex
 * -----------------------------------------------------------------------*/

/* NID: 0x1573DC3F */
s32 sys_lwmutex_create(sys_lwmutex_t_hle* lwmutex, const sys_lwmutex_attribute_t* attr)
{
    printf("[sysPrxForUser] sys_lwmutex_create(name='%.8s')\n",
           attr ? attr->name : "???");

    if (!lwmutex)
        return CELL_EFAULT;

    memset(lwmutex, 0, sizeof(*lwmutex));
    lwmutex->sleep_queue = s_next_lwmutex_id++;

    return CELL_OK;
}

/* NID: 0x1BC200F4 */
s32 sys_lwmutex_lock(sys_lwmutex_t_hle* lwmutex, u64 timeout)
{
    if (!lwmutex)
        return CELL_EFAULT;

    /* TODO: Implement actual locking with host mutex. */
    lwmutex->lock_var = 1;
    return CELL_OK;
}

/* NID: 0x1A1BC3B0 */
s32 sys_lwmutex_unlock(sys_lwmutex_t_hle* lwmutex)
{
    if (!lwmutex)
        return CELL_EFAULT;

    lwmutex->lock_var = 0;
    return CELL_OK;
}

/* NID: 0xC3B0A8A4 */
s32 sys_lwmutex_destroy(sys_lwmutex_t_hle* lwmutex)
{
    printf("[sysPrxForUser] sys_lwmutex_destroy()\n");

    if (!lwmutex)
        return CELL_EFAULT;

    memset(lwmutex, 0, sizeof(*lwmutex));
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Lightweight condition variable
 * -----------------------------------------------------------------------*/

/* NID: 0xDA0EB71A */
s32 sys_lwcond_create(sys_lwcond_t_hle* lwcond, sys_lwmutex_t_hle* lwmutex,
                      const sys_lwcond_attribute_t* attr)
{
    printf("[sysPrxForUser] sys_lwcond_create(name='%.8s')\n",
           attr ? attr->name : "???");

    if (!lwcond || !lwmutex)
        return CELL_EFAULT;

    lwcond->lwcond_queue = s_next_lwcond_id++;
    return CELL_OK;
}

/* NID: 0xEF87A695 */
s32 sys_lwcond_signal(sys_lwcond_t_hle* lwcond)
{
    if (!lwcond)
        return CELL_EFAULT;

    /* TODO: Wake one waiting thread on the associated host condvar. */
    return CELL_OK;
}

/* NID: 0x2A6D9D51 */
s32 sys_lwcond_wait(sys_lwcond_t_hle* lwcond, u64 timeout)
{
    if (!lwcond)
        return CELL_EFAULT;

    /* TODO: Block on host condvar; release associated lwmutex; re-acquire on wake. */
    return CELL_OK;
}

/* NID: 0x1B434AFC */
s32 sys_lwcond_destroy(sys_lwcond_t_hle* lwcond)
{
    printf("[sysPrxForUser] sys_lwcond_destroy()\n");

    if (!lwcond)
        return CELL_EFAULT;

    memset(lwcond, 0, sizeof(*lwcond));
    return CELL_OK;
}
