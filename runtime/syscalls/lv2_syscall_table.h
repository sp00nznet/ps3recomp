/*
 * ps3recomp - LV2 system call dispatch table
 *
 * PS3 syscalls use the `sc` instruction.  The syscall number is in r11,
 * arguments are in r3-r10 (up to 8 args), and the return value goes in r3.
 *
 * This file defines the syscall number constants, the dispatch table, and
 * the calling convention adapter.
 */

#ifndef LV2_SYSCALL_TABLE_H
#define LV2_SYSCALL_TABLE_H

#include "../ppu/ppu_context.h"
#include "../../include/ps3emu/error_codes.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Syscall numbers  (from LV2 kernel)
 * -----------------------------------------------------------------------*/

/* Process / thread management */
#define SYS_PROCESS_GETPID              1
#define SYS_PROCESS_WAIT_FOR_CHILD      2
#define SYS_PROCESS_EXIT                3
#define SYS_PROCESS_GET_STATUS          4
#define SYS_PROCESS_DETACH_CHILD        5
#define SYS_PROCESS_GET_NUMBER_OF_OBJECT 12
#define SYS_PROCESS_GET_ID              13
#define SYS_PROCESS_IS_SPU_LOCK_LINE_RESERVATION_ADDRESS 14

#define SYS_PPU_THREAD_CREATE           41
#define SYS_PPU_THREAD_EXIT             42
#define SYS_PPU_THREAD_YIELD            43
#define SYS_PPU_THREAD_JOIN             44
#define SYS_PPU_THREAD_DETACH           45
#define SYS_PPU_THREAD_GET_JOIN_STATE   46
#define SYS_PPU_THREAD_SET_PRIORITY     47
#define SYS_PPU_THREAD_GET_PRIORITY     48
#define SYS_PPU_THREAD_GET_STACK_INFORMATION 49
#define SYS_PPU_THREAD_RENAME           56

/* Synchronization */
#define SYS_MUTEX_CREATE                100
#define SYS_MUTEX_DESTROY               101
#define SYS_MUTEX_LOCK                  102
#define SYS_MUTEX_TRYLOCK               103
#define SYS_MUTEX_UNLOCK                104

#define SYS_COND_CREATE                 105
#define SYS_COND_DESTROY                106
#define SYS_COND_WAIT                   107
#define SYS_COND_SIGNAL                 108
#define SYS_COND_SIGNAL_ALL             109
#define SYS_COND_SIGNAL_TO              110

#define SYS_SEMAPHORE_CREATE            90
#define SYS_SEMAPHORE_DESTROY           91
#define SYS_SEMAPHORE_WAIT              92
#define SYS_SEMAPHORE_TRYWAIT           93
#define SYS_SEMAPHORE_POST              94
#define SYS_SEMAPHORE_GET_VALUE         114

#define SYS_RWLOCK_CREATE               120
#define SYS_RWLOCK_DESTROY              121
#define SYS_RWLOCK_RLOCK               122
#define SYS_RWLOCK_TRYRLOCK            123
#define SYS_RWLOCK_RUNLOCK             124
#define SYS_RWLOCK_WLOCK               125
#define SYS_RWLOCK_TRYWLOCK            126
#define SYS_RWLOCK_WUNLOCK             127

#define SYS_EVENT_QUEUE_CREATE          128
#define SYS_EVENT_QUEUE_DESTROY         129
#define SYS_EVENT_QUEUE_RECEIVE         130
#define SYS_EVENT_QUEUE_TRYRECEIVE      131
#define SYS_EVENT_QUEUE_DRAIN           133
#define SYS_EVENT_PORT_CREATE           134
#define SYS_EVENT_PORT_DESTROY          135
#define SYS_EVENT_PORT_CONNECT_LOCAL    136
#define SYS_EVENT_PORT_DISCONNECT       137
#define SYS_EVENT_PORT_SEND             138

#define SYS_EVENT_FLAG_CREATE           139
#define SYS_EVENT_FLAG_DESTROY          140
#define SYS_EVENT_FLAG_WAIT             141
#define SYS_EVENT_FLAG_TRYWAIT          142
#define SYS_EVENT_FLAG_SET              143
#define SYS_EVENT_FLAG_CLEAR            144
#define SYS_EVENT_FLAG_CANCEL           145
#define SYS_EVENT_FLAG_GET              146

#define SYS_LWMUTEX_CREATE              150
#define SYS_LWMUTEX_DESTROY             151
#define SYS_LWMUTEX_LOCK               152
#define SYS_LWMUTEX_TRYLOCK            153
#define SYS_LWMUTEX_UNLOCK             154

#define SYS_LWCOND_CREATE               155
#define SYS_LWCOND_DESTROY              156
#define SYS_LWCOND_WAIT                 157
#define SYS_LWCOND_SIGNAL               158
#define SYS_LWCOND_SIGNAL_ALL           159
#define SYS_LWCOND_SIGNAL_TO            160

/* Timer */
#define SYS_TIMER_CREATE                70
#define SYS_TIMER_DESTROY               71
#define SYS_TIMER_GET_INFORMATION       72
#define SYS_TIMER_START                 73
#define SYS_TIMER_STOP                  74
#define SYS_TIMER_CONNECT_EVENT_QUEUE   75
#define SYS_TIMER_DISCONNECT_EVENT_QUEUE 76
#define SYS_TIMER_USLEEP                141
#define SYS_TIMER_SLEEP                 142

/* Time */
#define SYS_TIME_GET_CURRENT_TIME       145
#define SYS_TIME_GET_TIMEBASE_FREQUENCY 147

/* Memory management */
#define SYS_MEMORY_ALLOCATE             348
#define SYS_MEMORY_FREE                 349
#define SYS_MEMORY_ALLOCATE_FROM_CONTAINER 350
#define SYS_MEMORY_GET_PAGE_ATTRIBUTE   358
#define SYS_MEMORY_GET_USER_MEMORY_SIZE 352
#define SYS_MEMORY_CONTAINER_CREATE     353
#define SYS_MEMORY_CONTAINER_DESTROY    354
#define SYS_MEMORY_CONTAINER_GET_SIZE   355

#define SYS_MMAPPER_ALLOCATE_ADDRESS     330
#define SYS_MMAPPER_FREE_ADDRESS         331
#define SYS_MMAPPER_ALLOCATE_SHARED_MEMORY 332
#define SYS_MMAPPER_FREE_SHARED_MEMORY   333
#define SYS_MMAPPER_MAP_SHARED_MEMORY    334
#define SYS_MMAPPER_UNMAP_SHARED_MEMORY  335
#define SYS_MMAPPER_SEARCH_AND_MAP      337

/* SPU management */
#define SYS_SPU_THREAD_GROUP_CREATE     170
#define SYS_SPU_THREAD_GROUP_DESTROY    171
#define SYS_SPU_THREAD_GROUP_START      172
#define SYS_SPU_THREAD_GROUP_SUSPEND    173
#define SYS_SPU_THREAD_GROUP_RESUME     174
#define SYS_SPU_THREAD_GROUP_YIELD      175
#define SYS_SPU_THREAD_GROUP_TERMINATE  176
#define SYS_SPU_THREAD_GROUP_JOIN       177

#define SYS_SPU_INITIALIZE              180
#define SYS_SPU_THREAD_INITIALIZE       181
#define SYS_SPU_THREAD_SET_ARGUMENT     182
#define SYS_SPU_THREAD_GET_EXIT_STATUS  183
#define SYS_SPU_THREAD_GROUP_CONNECT_EVENT 191
#define SYS_SPU_THREAD_GROUP_DISCONNECT_EVENT 192
#define SYS_SPU_THREAD_CONNECT_EVENT    193
#define SYS_SPU_THREAD_DISCONNECT_EVENT 194

#define SYS_SPU_THREAD_WRITE_LS         229
#define SYS_SPU_THREAD_READ_LS          230
#define SYS_SPU_THREAD_WRITE_SNR        231
#define SYS_SPU_THREAD_BIND_QUEUE       235
#define SYS_SPU_THREAD_UNBIND_QUEUE     236
#define SYS_SPU_THREAD_GROUP_CONNECT_EVENT_ALL_THREADS 251

#define SYS_SPU_IMAGE_OPEN              156
#define SYS_SPU_IMAGE_CLOSE             157

/* PRX (module) management */
#define SYS_PRX_LOAD_MODULE             480
#define SYS_PRX_START_MODULE            481
#define SYS_PRX_STOP_MODULE             482
#define SYS_PRX_UNLOAD_MODULE           483
#define SYS_PRX_REGISTER_MODULE         484
#define SYS_PRX_QUERY_MODULE            485
#define SYS_PRX_REGISTER_LIBRARY        486
#define SYS_PRX_UNREGISTER_LIBRARY      487
#define SYS_PRX_LINK_LIBRARY            488
#define SYS_PRX_UNLINK_LIBRARY          489
#define SYS_PRX_QUERY_LIBRARY           490
#define SYS_PRX_GET_MODULE_LIST         491
#define SYS_PRX_GET_MODULE_INFO         492
#define SYS_PRX_GET_MODULE_ID_BY_NAME   493
#define SYS_PRX_GET_MODULE_ID_BY_ADDRESS 494

/* File system */
#define SYS_FS_OPEN                     801
#define SYS_FS_READ                     802
#define SYS_FS_WRITE                    803
#define SYS_FS_CLOSE                    804
#define SYS_FS_OPENDIR                  805
#define SYS_FS_READDIR                  806
#define SYS_FS_CLOSEDIR                 807
#define SYS_FS_STAT                     808
#define SYS_FS_FSTAT                    809
#define SYS_FS_MKDIR                    811
#define SYS_FS_RENAME                   812
#define SYS_FS_RMDIR                    813
#define SYS_FS_UNLINK                   814
#define SYS_FS_LSEEK                    818
#define SYS_FS_FTRUNCATE                820
#define SYS_FS_FGET_BLOCK_SIZE          840
#define SYS_FS_GET_BLOCK_SIZE           841

/* Misc */
#define SYS_TTY_READ                    400
#define SYS_TTY_WRITE                   402
#define SYS_DBG_GET_THREAD_LIST         610

/* Upper limit for the dispatch table */
#define LV2_SYSCALL_MAX                 1024

/* ---------------------------------------------------------------------------
 * Syscall function signature
 *
 * Every syscall handler receives a pointer to the calling thread's context.
 * Arguments are read from ctx->gpr[3..10], return value written to gpr[3].
 * -----------------------------------------------------------------------*/
typedef int64_t (*lv2_syscall_fn)(ppu_context* ctx);

/* ---------------------------------------------------------------------------
 * Dispatch table
 * -----------------------------------------------------------------------*/
typedef struct lv2_syscall_table {
    lv2_syscall_fn handlers[LV2_SYSCALL_MAX];
} lv2_syscall_table;

extern lv2_syscall_table g_lv2_syscalls;

/* Default handler for unimplemented syscalls — logs the call for debugging */
static inline int64_t lv2_syscall_unimplemented(ppu_context* ctx)
{
    uint32_t num = (uint32_t)ctx->gpr[11];
    fprintf(stderr, "[LV2] unimplemented syscall %u (0x%X)\n", num, num);
    return (int64_t)(int32_t)CELL_ENOSYS;
}

static inline void lv2_syscall_table_init(lv2_syscall_table* tbl)
{
    for (int i = 0; i < LV2_SYSCALL_MAX; i++)
        tbl->handlers[i] = lv2_syscall_unimplemented;
}

static inline void lv2_syscall_register(lv2_syscall_table* tbl, uint32_t num, lv2_syscall_fn fn)
{
    if (num < LV2_SYSCALL_MAX)
        tbl->handlers[num] = fn;
}

/* ---------------------------------------------------------------------------
 * Dispatch a syscall
 *
 * Called when the recompiled code encounters an `sc` instruction.
 * The syscall number is in gpr[11].
 * -----------------------------------------------------------------------*/
static inline void lv2_syscall_dispatch(lv2_syscall_table* tbl, ppu_context* ctx)
{
    uint32_t num = (uint32_t)ctx->gpr[11];

    if (num >= LV2_SYSCALL_MAX) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_ENOSYS;
        return;
    }

    lv2_syscall_fn handler = tbl->handlers[num];
    ctx->gpr[3] = (uint64_t)handler(ctx);
}

/* Convenience wrapper using the global table */
static inline void lv2_syscall(ppu_context* ctx)
{
    lv2_syscall_dispatch(&g_lv2_syscalls, ctx);
}

/* ---------------------------------------------------------------------------
 * Argument extraction macros for syscall handlers
 *
 * PPC64 calling convention: up to 8 integer/pointer args in r3-r10.
 * -----------------------------------------------------------------------*/
#define LV2_ARG_U64(ctx, n)  ((ctx)->gpr[3 + (n)])
#define LV2_ARG_U32(ctx, n)  ((uint32_t)(ctx)->gpr[3 + (n)])
#define LV2_ARG_S32(ctx, n)  ((int32_t)(ctx)->gpr[3 + (n)])
#define LV2_ARG_S64(ctx, n)  ((int64_t)(ctx)->gpr[3 + (n)])
#define LV2_ARG_PTR(ctx, n)  ((uint32_t)(ctx)->gpr[3 + (n)])  /* guest address */

/* Convenience: register a syscall handler with a cleaner syntax */
#define LV2_REGISTER_SYSCALL(num, func) \
    lv2_syscall_register(&g_lv2_syscalls, (num), (lv2_syscall_fn)(func))

/* ---------------------------------------------------------------------------
 * Register all HLE syscall modules
 *
 * Initializes and registers handlers for all implemented LV2 subsystems:
 * ppu_thread, mutex, cond, semaphore, rwlock, event, timer, memory, fs.
 * -----------------------------------------------------------------------*/
void lv2_register_all_syscalls(lv2_syscall_table* tbl);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LV2_SYSCALL_TABLE_H */
