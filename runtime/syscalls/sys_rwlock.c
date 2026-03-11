/*
 * ps3recomp - Reader-writer lock syscalls (implementation)
 */

#include "sys_rwlock.h"
#include "../memory/vm.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * Globals
 * -----------------------------------------------------------------------*/
sys_rwlock_info g_sys_rwlocks[SYS_RWLOCK_MAX];

static void write_be32(uint32_t addr, uint32_t val)
{
    uint32_t* p = (uint32_t*)vm_to_host(addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
    val = ((val >> 24) & 0xFF) | ((val >> 8) & 0xFF00) |
          ((val <<  8) & 0xFF0000) | ((val << 24) & 0xFF000000u);
#endif
    *p = val;
}

/* ---------------------------------------------------------------------------
 * sys_rwlock_create
 *
 * r3 = pointer to receive rwlock ID (u32*)
 * r4 = pointer to attribute struct
 * -----------------------------------------------------------------------*/
int64_t sys_rwlock_create(ppu_context* ctx)
{
    uint32_t id_out_addr = LV2_ARG_PTR(ctx, 0);
    uint32_t attr_addr   = LV2_ARG_PTR(ctx, 1);

    int slot = -1;
    for (int i = 0; i < SYS_RWLOCK_MAX; i++) {
        if (!g_sys_rwlocks[i].active) { slot = i; break; }
    }
    if (slot < 0)
        return (int64_t)(int32_t)CELL_EAGAIN;

    sys_rwlock_info* r = &g_sys_rwlocks[slot];
    memset(r, 0, sizeof(*r));
    r->active = 1;

    if (attr_addr != 0) {
        uint8_t* attr_raw = (uint8_t*)vm_to_host(attr_addr);
        uint32_t proto_be;
        memcpy(&proto_be, attr_raw, 4);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
        proto_be = ((proto_be >> 24) & 0xFF) | ((proto_be >> 8) & 0xFF00) |
                   ((proto_be << 8) & 0xFF0000) | ((proto_be << 24) & 0xFF000000u);
#endif
        r->protocol = proto_be;
        memcpy(r->name, attr_raw + 8, 8);
    }

#ifdef _WIN32
    InitializeSRWLock(&r->srw);
    r->readers = 0;
    r->writer  = 0;
#else
    pthread_rwlock_init(&r->rwl, NULL);
#endif

    uint32_t rwlock_id = (uint32_t)(slot + 1);
    if (id_out_addr != 0) {
        write_be32(id_out_addr, rwlock_id);
    }

    return CELL_OK;
}

int64_t sys_rwlock_destroy(ppu_context* ctx)
{
    uint32_t rwlock_id = LV2_ARG_U32(ctx, 0);

    if (rwlock_id == 0 || rwlock_id > SYS_RWLOCK_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_rwlock_info* r = &g_sys_rwlocks[rwlock_id - 1];
    if (!r->active)
        return (int64_t)(int32_t)CELL_ESRCH;

#ifndef _WIN32
    pthread_rwlock_destroy(&r->rwl);
#endif
    /* Windows SRWLock doesn't need destruction */

    r->active = 0;
    return CELL_OK;
}

int64_t sys_rwlock_rlock(ppu_context* ctx)
{
    uint32_t rwlock_id  = LV2_ARG_U32(ctx, 0);
    /* uint64_t timeout = LV2_ARG_U64(ctx, 1); -- timeout not commonly used */

    if (rwlock_id == 0 || rwlock_id > SYS_RWLOCK_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_rwlock_info* r = &g_sys_rwlocks[rwlock_id - 1];
    if (!r->active)
        return (int64_t)(int32_t)CELL_ESRCH;

#ifdef _WIN32
    AcquireSRWLockShared(&r->srw);
    InterlockedIncrement(&r->readers);
#else
    pthread_rwlock_rdlock(&r->rwl);
#endif

    return CELL_OK;
}

int64_t sys_rwlock_tryrlock(ppu_context* ctx)
{
    uint32_t rwlock_id = LV2_ARG_U32(ctx, 0);

    if (rwlock_id == 0 || rwlock_id > SYS_RWLOCK_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_rwlock_info* r = &g_sys_rwlocks[rwlock_id - 1];
    if (!r->active)
        return (int64_t)(int32_t)CELL_ESRCH;

#ifdef _WIN32
    if (!TryAcquireSRWLockShared(&r->srw))
        return (int64_t)(int32_t)CELL_EBUSY;
    InterlockedIncrement(&r->readers);
#else
    int rc = pthread_rwlock_tryrdlock(&r->rwl);
    if (rc != 0)
        return (int64_t)(int32_t)CELL_EBUSY;
#endif

    return CELL_OK;
}

int64_t sys_rwlock_runlock(ppu_context* ctx)
{
    uint32_t rwlock_id = LV2_ARG_U32(ctx, 0);

    if (rwlock_id == 0 || rwlock_id > SYS_RWLOCK_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_rwlock_info* r = &g_sys_rwlocks[rwlock_id - 1];
    if (!r->active)
        return (int64_t)(int32_t)CELL_ESRCH;

#ifdef _WIN32
    InterlockedDecrement(&r->readers);
    ReleaseSRWLockShared(&r->srw);
#else
    pthread_rwlock_unlock(&r->rwl);
#endif

    return CELL_OK;
}

int64_t sys_rwlock_wlock(ppu_context* ctx)
{
    uint32_t rwlock_id  = LV2_ARG_U32(ctx, 0);
    /* uint64_t timeout = LV2_ARG_U64(ctx, 1); */

    if (rwlock_id == 0 || rwlock_id > SYS_RWLOCK_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_rwlock_info* r = &g_sys_rwlocks[rwlock_id - 1];
    if (!r->active)
        return (int64_t)(int32_t)CELL_ESRCH;

#ifdef _WIN32
    AcquireSRWLockExclusive(&r->srw);
    InterlockedExchange(&r->writer, 1);
#else
    pthread_rwlock_wrlock(&r->rwl);
#endif

    return CELL_OK;
}

int64_t sys_rwlock_trywlock(ppu_context* ctx)
{
    uint32_t rwlock_id = LV2_ARG_U32(ctx, 0);

    if (rwlock_id == 0 || rwlock_id > SYS_RWLOCK_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_rwlock_info* r = &g_sys_rwlocks[rwlock_id - 1];
    if (!r->active)
        return (int64_t)(int32_t)CELL_ESRCH;

#ifdef _WIN32
    if (!TryAcquireSRWLockExclusive(&r->srw))
        return (int64_t)(int32_t)CELL_EBUSY;
    InterlockedExchange(&r->writer, 1);
#else
    int rc = pthread_rwlock_trywrlock(&r->rwl);
    if (rc != 0)
        return (int64_t)(int32_t)CELL_EBUSY;
#endif

    return CELL_OK;
}

int64_t sys_rwlock_wunlock(ppu_context* ctx)
{
    uint32_t rwlock_id = LV2_ARG_U32(ctx, 0);

    if (rwlock_id == 0 || rwlock_id > SYS_RWLOCK_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_rwlock_info* r = &g_sys_rwlocks[rwlock_id - 1];
    if (!r->active)
        return (int64_t)(int32_t)CELL_ESRCH;

#ifdef _WIN32
    InterlockedExchange(&r->writer, 0);
    ReleaseSRWLockExclusive(&r->srw);
#else
    pthread_rwlock_unlock(&r->rwl);
#endif

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Registration
 * -----------------------------------------------------------------------*/
void sys_rwlock_init(lv2_syscall_table* tbl)
{
    memset(g_sys_rwlocks, 0, sizeof(g_sys_rwlocks));

    lv2_syscall_register(tbl, SYS_RWLOCK_CREATE,   sys_rwlock_create);
    lv2_syscall_register(tbl, SYS_RWLOCK_DESTROY,  sys_rwlock_destroy);
    lv2_syscall_register(tbl, SYS_RWLOCK_RLOCK,    sys_rwlock_rlock);
    lv2_syscall_register(tbl, SYS_RWLOCK_TRYRLOCK, sys_rwlock_tryrlock);
    lv2_syscall_register(tbl, SYS_RWLOCK_RUNLOCK,  sys_rwlock_runlock);
    lv2_syscall_register(tbl, SYS_RWLOCK_WLOCK,    sys_rwlock_wlock);
    lv2_syscall_register(tbl, SYS_RWLOCK_TRYWLOCK, sys_rwlock_trywlock);
    lv2_syscall_register(tbl, SYS_RWLOCK_WUNLOCK,  sys_rwlock_wunlock);
}
