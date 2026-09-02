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
#define SYS_RWLOCK_TRYWLOCK            148
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

/* Canonical LV2 numbers (RPCS3 lv2.cpp ground truth). The previous 139-146
 * block was fictional and collided with the timer family: 141 was also
 * SYS_TIMER_USLEEP, so a PSL1GHT guest's usleep(30us) dispatched to
 * sys_event_flag_wait(flag=30) -- a 65ms stall per call with bogus results
 * (vkcube's init polled via usleep, timed out, and exited(-1)). */
#define SYS_EVENT_FLAG_CREATE           82
#define SYS_EVENT_FLAG_DESTROY          83
#define SYS_EVENT_FLAG_WAIT             85
#define SYS_EVENT_FLAG_TRYWAIT          86
#define SYS_EVENT_FLAG_SET              87
#define SYS_EVENT_FLAG_CLEAR            118
#define SYS_EVENT_FLAG_CANCEL           132
#define SYS_EVENT_FLAG_GET              139

/* lv2 lightweight primitives are the _sys_lw* slow-path syscalls (95-99,
 * 111-117); the old 150-160 values squatted on sys_raw_spu_* (150-154)
 * and sys_spu_image_open/import (156/157). */
#define SYS_LWMUTEX_CREATE              95
#define SYS_LWMUTEX_DESTROY             96
#define SYS_LWMUTEX_LOCK                97
#define SYS_LWMUTEX_UNLOCK              98
#define SYS_LWMUTEX_TRYLOCK             99

#define SYS_LWCOND_CREATE               111
#define SYS_LWCOND_DESTROY              112
#define SYS_LWCOND_WAIT                 113   /* _sys_lwcond_queue_wait */
#define SYS_LWCOND_SIGNAL               115
#define SYS_LWCOND_SIGNAL_ALL           116

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
#define SYS_MEMORY_GET_PAGE_ATTRIBUTE   351   /* 0x15F (was wrongly 358; verified vs rpcs3/rpcs3/Emu/Cell/lv2/sys_memory.h) */
#define SYS_MEMORY_GET_USER_MEMORY_SIZE 352
#define SYS_MEMORY_CONTAINER_CREATE     324
#define SYS_MEMORY_CONTAINER_DESTROY    325
#define SYS_MEMORY_CONTAINER_GET_SIZE   343

#define SYS_MMAPPER_ALLOCATE_ADDRESS     330
#define SYS_MMAPPER_FREE_ADDRESS         331
#define SYS_MMAPPER_ALLOCATE_SHARED_MEMORY 332
#define SYS_MMAPPER_FREE_SHARED_MEMORY   333
#define SYS_MMAPPER_MAP_SHARED_MEMORY    334
#define SYS_MMAPPER_UNMAP_SHARED_MEMORY  335
#define SYS_MMAPPER_SEARCH_AND_MAP      337

/* SPU management — numbers verified against the real lv2 table (RPCS3
 * lv2.cpp 155-252). The old block was wrong almost everywhere (e.g.
 * THREAD_INITIALIZE was registered at 181 while the real 172 fell on
 * GROUP_START — Sony's SPURS init failed silently on the id mismatch). */
#define SYS_SPU_IMAGE_OPEN              156
#define SYS_SPU_IMAGE_IMPORT            157   /* _sys_spu_image_import */
#define SYS_SPU_IMAGE_CLOSE             158   /* _sys_spu_image_close */
#define SYS_SPU_IMAGE_GET_SEGMENTS      159   /* _sys_spu_image_get_segments */

#define SYS_SPU_THREAD_GET_EXIT_STATUS  165
#define SYS_SPU_THREAD_SET_ARGUMENT     166
#define SYS_SPU_INITIALIZE              169

#define SYS_SPU_THREAD_GROUP_CREATE     170
#define SYS_SPU_THREAD_GROUP_DESTROY    171
#define SYS_SPU_THREAD_INITIALIZE       172
#define SYS_SPU_THREAD_GROUP_START      173
#define SYS_SPU_THREAD_GROUP_SUSPEND    174
#define SYS_SPU_THREAD_GROUP_RESUME     175
#define SYS_SPU_THREAD_GROUP_YIELD      176
#define SYS_SPU_THREAD_GROUP_TERMINATE  177
#define SYS_SPU_THREAD_GROUP_JOIN       178
#define SYS_SPU_THREAD_GROUP_SET_PRIORITY 179
#define SYS_SPU_THREAD_GROUP_GET_PRIORITY 180

#define SYS_SPU_THREAD_WRITE_LS         181
#define SYS_SPU_THREAD_READ_LS          182
#define SYS_SPU_THREAD_WRITE_SNR        184
#define SYS_SPU_THREAD_SET_SPU_CFG      187
#define SYS_SPU_THREAD_GET_SPU_CFG      188
#define SYS_SPU_THREAD_GROUP_CONNECT_EVENT    185
#define SYS_SPU_THREAD_GROUP_DISCONNECT_EVENT 186
#define SYS_SPU_THREAD_WRITE_SPU_MB     190
#define SYS_SPU_THREAD_CONNECT_EVENT    191
#define SYS_SPU_THREAD_DISCONNECT_EVENT 192
#define SYS_SPU_THREAD_BIND_QUEUE       193
#define SYS_SPU_THREAD_UNBIND_QUEUE     194
#define SYS_SPU_THREAD_GROUP_CONNECT_EVENT_ALL_THREADS 251

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
#define SYS_PRX_GET_MODULE_LIST         494
#define SYS_PRX_GET_MODULE_INFO         495
#define SYS_PRX_GET_MODULE_ID_BY_NAME   496
#define SYS_PRX_GET_MODULE_ID_BY_ADDRESS 461

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
#define SYS_TTY_READ                    402
#define SYS_TTY_WRITE                   403
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

/* No lv2_syscall() wrapper here on purpose. The real one lives in the PPU
 * boot harness (ppu_loader.cpp) and does considerably more than dispatch:
 * it arms the page guard, tracks which lv2 call each guest thread is
 * inside, handles the boot-critical calls inline and stamps the profiler.
 * A convenience wrapper used to sit here, static inline, calling
 * lv2_syscall_dispatch directly -- nothing called it, but any translation
 * unit including both this header and the lifter-generated one bound the
 * name to the stripped-down version instead of the real one, and the two
 * declarations (extern there, static here) are contradictory besides.
 * GCC rejects the combination outright; Clang accepted it. Call
 * lv2_syscall_dispatch(&g_lv2_syscalls, ctx) if that is genuinely what
 * you want. */

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
