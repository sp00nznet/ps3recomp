/*
 * ps3recomp - PPU thread management syscalls (implementation)
 */

#include "sys_ppu_thread.h"
#include <string.h>
#include <stdio.h>

/* ---------------------------------------------------------------------------
 * Globals
 * -----------------------------------------------------------------------*/
ppu_thread_info       g_ppu_threads[PPU_THREAD_MAX];
vm_stack_alloc        g_vm_stack_alloc;
ppu_thread_entry_fn   g_ppu_thread_entry_trampoline = NULL;

/* Simple mutex for thread table access */
#ifdef _WIN32
static CRITICAL_SECTION s_table_lock;
static int              s_table_lock_init = 0;
#else
static pthread_mutex_t  s_table_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

static void table_lock(void)
{
#ifdef _WIN32
    if (!s_table_lock_init) {
        InitializeCriticalSection(&s_table_lock);
        s_table_lock_init = 1;
    }
    EnterCriticalSection(&s_table_lock);
#else
    pthread_mutex_lock(&s_table_lock);
#endif
}

static void table_unlock(void)
{
#ifdef _WIN32
    LeaveCriticalSection(&s_table_lock);
#else
    pthread_mutex_unlock(&s_table_lock);
#endif
}

/* Find a free slot. Returns index or -1. Must be called under lock. */
static int find_free_slot(void)
{
    for (int i = 0; i < PPU_THREAD_MAX; i++) {
        if (g_ppu_threads[i].state == PPU_THREAD_STATE_FREE)
            return i;
    }
    return -1;
}

/* Find thread by ID. The ID is the index + 1 (0 is invalid). */
static ppu_thread_info* find_thread(uint64_t thread_id)
{
    if (thread_id == 0 || thread_id > PPU_THREAD_MAX) return NULL;
    ppu_thread_info* t = &g_ppu_threads[thread_id - 1];
    if (t->state == PPU_THREAD_STATE_FREE) return NULL;
    return t;
}

/* ---------------------------------------------------------------------------
 * Host thread entry point
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
static DWORD WINAPI ppu_host_thread_proc(LPVOID param)
#else
static void* ppu_host_thread_proc(void* param)
#endif
{
    ppu_thread_info* info = (ppu_thread_info*)param;

    /* Invoke the recompiled entry point */
    if (g_ppu_thread_entry_trampoline) {
        g_ppu_thread_entry_trampoline(&info->ctx);
    }

    /* Mark as finished */
    table_lock();
    info->exit_status = (int64_t)info->ctx.gpr[3];

    if (info->state == PPU_THREAD_STATE_DETACHED) {
        /* Detached threads self-clean */
        info->state = PPU_THREAD_STATE_FREE;
    } else {
        info->state = PPU_THREAD_STATE_FINISHED;
    }
    table_unlock();

    /* Signal anyone waiting for join */
#ifdef _WIN32
    SetEvent(info->finish_event);
    return 0;
#else
    pthread_mutex_lock(&info->finish_mutex);
    info->finished = 1;
    pthread_cond_signal(&info->finish_cond);
    pthread_mutex_unlock(&info->finish_mutex);
    return NULL;
#endif
}

/* ---------------------------------------------------------------------------
 * sys_ppu_thread_create
 *
 * r3 = pointer to receive thread ID (u64*)
 * r4 = entry point address
 * r5 = argument (passed in new thread's r3)
 * r6 = priority (s32)
 * r7 = stack size
 * r8 = flags
 * r9 = thread name pointer
 * -----------------------------------------------------------------------*/
int64_t sys_ppu_thread_create(ppu_context* ctx)
{
    uint32_t tid_out_addr = LV2_ARG_PTR(ctx, 0);
    uint64_t entry        = LV2_ARG_U64(ctx, 1);
    uint64_t arg          = LV2_ARG_U64(ctx, 2);
    int32_t  priority     = LV2_ARG_S32(ctx, 3);
    uint32_t stack_size   = LV2_ARG_U32(ctx, 4);
    /* uint64_t flags     = LV2_ARG_U64(ctx, 5); */
    uint32_t name_addr    = LV2_ARG_PTR(ctx, 6);

    if (stack_size == 0) stack_size = VM_PPU_STACK_SIZE;
    if (stack_size < 0x4000) stack_size = 0x4000; /* 16 KB minimum */

    table_lock();

    int slot = find_free_slot();
    if (slot < 0) {
        table_unlock();
        return (int64_t)(int32_t)CELL_EAGAIN;
    }

    ppu_thread_info* t = &g_ppu_threads[slot];
    memset(t, 0, sizeof(*t));

    /* Allocate guest stack */
    uint32_t stack_addr = vm_stack_allocate(&g_vm_stack_alloc, stack_size);
    if (stack_addr == 0) {
        table_unlock();
        return (int64_t)(int32_t)CELL_ENOMEM;
    }

    /* Set up the PPU context for the new thread */
    ppu_context_init(&t->ctx);
    t->ctx.cia = entry;
    t->ctx.gpr[3] = arg;
    ppu_set_stack(&t->ctx, (uint64_t)stack_addr, (uint64_t)stack_size);
    /* Copy TOC from creating thread */
    t->ctx.gpr[2] = ctx->gpr[2];

    uint64_t thread_id = (uint64_t)(slot + 1);
    t->ctx.thread_id = thread_id;

    t->state      = PPU_THREAD_STATE_RUNNING;
    t->priority   = priority;
    t->joinable   = 1;
    t->stack_addr = stack_addr;
    t->stack_size = stack_size;
    t->entry_addr = entry;

    /* Copy thread name if provided */
    if (name_addr != 0) {
        const char* name = (const char*)vm_to_host(name_addr);
        strncpy(t->name, name, sizeof(t->name) - 1);
        t->name[sizeof(t->name) - 1] = '\0';
    }

    /* Create synchronization for join */
#ifdef _WIN32
    t->finish_event = CreateEventA(NULL, TRUE, FALSE, NULL);
#else
    pthread_mutex_init(&t->finish_mutex, NULL);
    pthread_cond_init(&t->finish_cond, NULL);
    t->finished = 0;
#endif

    /* Write thread ID to output pointer */
    if (tid_out_addr != 0) {
        uint64_t* out = (uint64_t*)vm_to_host(tid_out_addr);
        /* Store as big-endian u64 */
        uint64_t be_id = thread_id;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
        be_id = ((be_id >> 56) & 0xFF) |
                ((be_id >> 40) & 0xFF00) |
                ((be_id >> 24) & 0xFF0000) |
                ((be_id >>  8) & 0xFF000000ULL) |
                ((be_id <<  8) & 0xFF00000000ULL) |
                ((be_id << 24) & 0xFF0000000000ULL) |
                ((be_id << 40) & 0xFF000000000000ULL) |
                ((be_id << 56) & 0xFF00000000000000ULL);
#endif
        *out = be_id;
    }

    /* Create the host thread */
#ifdef _WIN32
    t->host_thread = CreateThread(NULL, 0, ppu_host_thread_proc, t, 0, &t->host_tid);
    if (t->host_thread == NULL) {
        t->state = PPU_THREAD_STATE_FREE;
        CloseHandle(t->finish_event);
        table_unlock();
        return (int64_t)(int32_t)CELL_EAGAIN;
    }
#else
    int rc = pthread_create(&t->host_thread, NULL, ppu_host_thread_proc, t);
    if (rc != 0) {
        t->state = PPU_THREAD_STATE_FREE;
        pthread_mutex_destroy(&t->finish_mutex);
        pthread_cond_destroy(&t->finish_cond);
        table_unlock();
        return (int64_t)(int32_t)CELL_EAGAIN;
    }
#endif

    table_unlock();
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_ppu_thread_exit
 *
 * r3 = exit status
 * -----------------------------------------------------------------------*/
int64_t sys_ppu_thread_exit(ppu_context* ctx)
{
    uint64_t status = LV2_ARG_U64(ctx, 0);
    uint64_t tid = ctx->thread_id;

    table_lock();
    ppu_thread_info* t = find_thread(tid);
    if (t) {
        t->exit_status = (int64_t)status;
        if (t->state == PPU_THREAD_STATE_DETACHED) {
            t->state = PPU_THREAD_STATE_FREE;
        } else {
            t->state = PPU_THREAD_STATE_FINISHED;
        }

#ifdef _WIN32
        SetEvent(t->finish_event);
#else
        pthread_mutex_lock(&t->finish_mutex);
        t->finished = 1;
        pthread_cond_signal(&t->finish_cond);
        pthread_mutex_unlock(&t->finish_mutex);
#endif
    }
    table_unlock();

    /* In a real implementation this would terminate the calling thread.
     * The thread proc wrapper handles this after return. */
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_ppu_thread_join
 *
 * r3 = thread_id
 * r4 = pointer to receive exit status (s64*)
 * -----------------------------------------------------------------------*/
int64_t sys_ppu_thread_join(ppu_context* ctx)
{
    uint64_t tid          = LV2_ARG_U64(ctx, 0);
    uint32_t status_addr  = LV2_ARG_PTR(ctx, 1);

    table_lock();
    ppu_thread_info* t = find_thread(tid);
    if (!t) {
        table_unlock();
        return (int64_t)(int32_t)CELL_ESRCH;
    }
    if (!t->joinable || t->state == PPU_THREAD_STATE_DETACHED) {
        table_unlock();
        return (int64_t)(int32_t)CELL_EINVAL;
    }
    table_unlock();

    /* Wait for completion */
#ifdef _WIN32
    WaitForSingleObject(t->finish_event, INFINITE);
#else
    pthread_mutex_lock(&t->finish_mutex);
    while (!t->finished) {
        pthread_cond_wait(&t->finish_cond, &t->finish_mutex);
    }
    pthread_mutex_unlock(&t->finish_mutex);
#endif

    /* Write exit status */
    if (status_addr != 0) {
        int64_t es = t->exit_status;
        int64_t* out = (int64_t*)vm_to_host(status_addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
        uint64_t u = (uint64_t)es;
        u = ((u >> 56) & 0xFF) |
            ((u >> 40) & 0xFF00) |
            ((u >> 24) & 0xFF0000) |
            ((u >>  8) & 0xFF000000ULL) |
            ((u <<  8) & 0xFF00000000ULL) |
            ((u << 24) & 0xFF0000000000ULL) |
            ((u << 40) & 0xFF000000000000ULL) |
            ((u << 56) & 0xFF00000000000000ULL);
        *out = (int64_t)u;
#else
        *out = es;
#endif
    }

    /* Clean up */
    table_lock();
#ifdef _WIN32
    CloseHandle(t->host_thread);
    CloseHandle(t->finish_event);
    t->host_thread = NULL;
    t->finish_event = NULL;
#else
    pthread_join(t->host_thread, NULL);
    pthread_mutex_destroy(&t->finish_mutex);
    pthread_cond_destroy(&t->finish_cond);
#endif
    t->state = PPU_THREAD_STATE_FREE;
    table_unlock();

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_ppu_thread_detach
 *
 * r3 = thread_id
 * -----------------------------------------------------------------------*/
int64_t sys_ppu_thread_detach(ppu_context* ctx)
{
    uint64_t tid = LV2_ARG_U64(ctx, 0);

    table_lock();
    ppu_thread_info* t = find_thread(tid);
    if (!t) {
        table_unlock();
        return (int64_t)(int32_t)CELL_ESRCH;
    }

    if (t->state == PPU_THREAD_STATE_FINISHED) {
        /* Already finished, free it */
#ifdef _WIN32
        CloseHandle(t->host_thread);
        CloseHandle(t->finish_event);
#else
        pthread_detach(t->host_thread);
        pthread_mutex_destroy(&t->finish_mutex);
        pthread_cond_destroy(&t->finish_cond);
#endif
        t->state = PPU_THREAD_STATE_FREE;
    } else {
        t->state = PPU_THREAD_STATE_DETACHED;
        t->joinable = 0;
#ifndef _WIN32
        pthread_detach(t->host_thread);
#endif
    }

    table_unlock();
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_ppu_thread_yield
 * -----------------------------------------------------------------------*/
int64_t sys_ppu_thread_yield(ppu_context* ctx)
{
    (void)ctx;
#ifdef _WIN32
    SwitchToThread();
#else
    sched_yield();
#endif
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_ppu_thread_get_priority
 *
 * r3 = thread_id
 * r4 = pointer to receive priority (s32*)
 * -----------------------------------------------------------------------*/
int64_t sys_ppu_thread_get_priority(ppu_context* ctx)
{
    uint64_t tid       = LV2_ARG_U64(ctx, 0);
    uint32_t prio_addr = LV2_ARG_PTR(ctx, 1);

    table_lock();
    ppu_thread_info* t = find_thread(tid);
    if (!t) {
        table_unlock();
        return (int64_t)(int32_t)CELL_ESRCH;
    }

    if (prio_addr != 0) {
        int32_t prio = t->priority;
        int32_t* out = (int32_t*)vm_to_host(prio_addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
        uint32_t u = (uint32_t)prio;
        u = ((u >> 24) & 0xFF) | ((u >> 8) & 0xFF00) |
            ((u <<  8) & 0xFF0000) | ((u << 24) & 0xFF000000u);
        *out = (int32_t)u;
#else
        *out = prio;
#endif
    }

    table_unlock();
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_ppu_thread_set_priority
 *
 * r3 = thread_id
 * r4 = priority
 * -----------------------------------------------------------------------*/
int64_t sys_ppu_thread_set_priority(ppu_context* ctx)
{
    uint64_t tid      = LV2_ARG_U64(ctx, 0);
    int32_t  priority = LV2_ARG_S32(ctx, 1);

    table_lock();
    ppu_thread_info* t = find_thread(tid);
    if (!t) {
        table_unlock();
        return (int64_t)(int32_t)CELL_ESRCH;
    }

    t->priority = priority;
    table_unlock();
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_ppu_thread_rename
 *
 * r3 = thread_id
 * r4 = name pointer
 * -----------------------------------------------------------------------*/
int64_t sys_ppu_thread_rename(ppu_context* ctx)
{
    uint64_t tid       = LV2_ARG_U64(ctx, 0);
    uint32_t name_addr = LV2_ARG_PTR(ctx, 1);

    table_lock();
    ppu_thread_info* t = find_thread(tid);
    if (!t) {
        table_unlock();
        return (int64_t)(int32_t)CELL_ESRCH;
    }

    if (name_addr != 0) {
        const char* name = (const char*)vm_to_host(name_addr);
        strncpy(t->name, name, sizeof(t->name) - 1);
        t->name[sizeof(t->name) - 1] = '\0';
    }

    table_unlock();
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_ppu_thread_get_join_state
 *
 * r3 = pointer to receive join state (s32*)
 * -----------------------------------------------------------------------*/
int64_t sys_ppu_thread_get_join_state(ppu_context* ctx)
{
    uint32_t out_addr = LV2_ARG_PTR(ctx, 0);
    uint64_t tid = ctx->thread_id;

    table_lock();
    ppu_thread_info* t = find_thread(tid);
    int32_t joinable = t ? t->joinable : 0;
    table_unlock();

    if (out_addr != 0) {
        int32_t* out = (int32_t*)vm_to_host(out_addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
        uint32_t u = (uint32_t)joinable;
        u = ((u >> 24) & 0xFF) | ((u >> 8) & 0xFF00) |
            ((u <<  8) & 0xFF0000) | ((u << 24) & 0xFF000000u);
        *out = (int32_t)u;
#else
        *out = joinable;
#endif
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_ppu_thread_get_stack_information
 *
 * r3 = pointer to receive stack info struct
 *   struct { u32 addr; u32 size; }
 * -----------------------------------------------------------------------*/
int64_t sys_ppu_thread_get_stack_information(ppu_context* ctx)
{
    uint32_t out_addr = LV2_ARG_PTR(ctx, 0);
    uint64_t tid = ctx->thread_id;

    table_lock();
    ppu_thread_info* t = find_thread(tid);
    if (!t) {
        table_unlock();
        /* For the main thread, return sensible defaults */
        if (out_addr != 0) {
            uint32_t* out = (uint32_t*)vm_to_host(out_addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
            /* byteswap both fields */
            uint32_t sa = VM_STACK_BASE;
            uint32_t ss = VM_PPU_STACK_SIZE;
            sa = ((sa >> 24) & 0xFF) | ((sa >> 8) & 0xFF00) |
                 ((sa <<  8) & 0xFF0000) | ((sa << 24) & 0xFF000000u);
            ss = ((ss >> 24) & 0xFF) | ((ss >> 8) & 0xFF00) |
                 ((ss <<  8) & 0xFF0000) | ((ss << 24) & 0xFF000000u);
            out[0] = sa;
            out[1] = ss;
#else
            out[0] = VM_STACK_BASE;
            out[1] = VM_PPU_STACK_SIZE;
#endif
        }
        return CELL_OK;
    }

    if (out_addr != 0) {
        uint32_t* out = (uint32_t*)vm_to_host(out_addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
        uint32_t sa = t->stack_addr;
        uint32_t ss = t->stack_size;
        sa = ((sa >> 24) & 0xFF) | ((sa >> 8) & 0xFF00) |
             ((sa <<  8) & 0xFF0000) | ((sa << 24) & 0xFF000000u);
        ss = ((ss >> 24) & 0xFF) | ((ss >> 8) & 0xFF00) |
             ((ss <<  8) & 0xFF0000) | ((ss << 24) & 0xFF000000u);
        out[0] = sa;
        out[1] = ss;
#else
        out[0] = t->stack_addr;
        out[1] = t->stack_size;
#endif
    }

    table_unlock();
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Registration
 * -----------------------------------------------------------------------*/
void sys_ppu_thread_init(lv2_syscall_table* tbl)
{
    /* Initialize stack allocator */
    vm_stack_alloc_init(&g_vm_stack_alloc);

    /* Clear thread table */
    memset(g_ppu_threads, 0, sizeof(g_ppu_threads));

#ifdef _WIN32
    if (!s_table_lock_init) {
        InitializeCriticalSection(&s_table_lock);
        s_table_lock_init = 1;
    }
#endif

    lv2_syscall_register(tbl, SYS_PPU_THREAD_CREATE,              sys_ppu_thread_create);
    lv2_syscall_register(tbl, SYS_PPU_THREAD_EXIT,                sys_ppu_thread_exit);
    lv2_syscall_register(tbl, SYS_PPU_THREAD_YIELD,               sys_ppu_thread_yield);
    lv2_syscall_register(tbl, SYS_PPU_THREAD_JOIN,                sys_ppu_thread_join);
    lv2_syscall_register(tbl, SYS_PPU_THREAD_DETACH,              sys_ppu_thread_detach);
    lv2_syscall_register(tbl, SYS_PPU_THREAD_GET_JOIN_STATE,      sys_ppu_thread_get_join_state);
    lv2_syscall_register(tbl, SYS_PPU_THREAD_SET_PRIORITY,        sys_ppu_thread_set_priority);
    lv2_syscall_register(tbl, SYS_PPU_THREAD_GET_PRIORITY,        sys_ppu_thread_get_priority);
    lv2_syscall_register(tbl, SYS_PPU_THREAD_GET_STACK_INFORMATION, sys_ppu_thread_get_stack_information);
    lv2_syscall_register(tbl, SYS_PPU_THREAD_RENAME,              sys_ppu_thread_rename);
}
