/*
 * ps3recomp - sysPrxForUser HLE stub
 *
 * Core PRX runtime: threads, process management, printf, lightweight
 * mutexes and condition variables.
 */

#ifndef PS3RECOMP_SYS_PRX_FOR_USER_H
#define PS3RECOMP_SYS_PRX_FOR_USER_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Types
 * -----------------------------------------------------------------------*/

/* Thread entry point */
typedef void (*sys_ppu_thread_entry_t)(u64 arg);

/* Thread attributes */
typedef struct sys_ppu_thread_attr {
    u32 entry;        /* guest entry address */
    u32 tls_addr;
} sys_ppu_thread_attr_t;

/* Lightweight mutex attribute */
typedef struct sys_lwmutex_attribute {
    u32 protocol;        /* SYS_SYNC_FIFO = 1, SYS_SYNC_PRIORITY = 2 */
    u32 recursive;       /* SYS_SYNC_RECURSIVE = 1, SYS_SYNC_NOT_RECURSIVE = 2 */
    char name[8];
} sys_lwmutex_attribute_t;

/* Lightweight mutex */
typedef struct sys_lwmutex {
    u64 lock_var;
    u32 attribute;
    u32 recursive_count;
    u32 sleep_queue;
    u32 pad;
} sys_lwmutex_t_hle;

/* Lightweight condition variable attribute */
typedef struct sys_lwcond_attribute {
    char name[8];
} sys_lwcond_attribute_t;

/* Lightweight condition variable */
typedef struct sys_lwcond {
    u32 lwmutex_addr;    /* guest pointer to associated lwmutex */
    u32 lwcond_queue;
} sys_lwcond_t_hle;

/* Sync protocol constants */
#define SYS_SYNC_FIFO              0x00000001
#define SYS_SYNC_PRIORITY          0x00000002
#define SYS_SYNC_PRIORITY_INHERIT  0x00000003
#define SYS_SYNC_RECURSIVE         0x00000010
#define SYS_SYNC_NOT_RECURSIVE     0x00000020

/* Thread priority range */
#define SYS_PPU_THREAD_PRIORITY_MAX  0
#define SYS_PPU_THREAD_PRIORITY_MIN  3071

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
                          const char* threadname);

/* NID: 0xAFF080A4 */
void sys_ppu_thread_exit(u64 exitcode);

/* NID: 0x1D3EE43E */
s32 sys_ppu_thread_join(sys_ppu_thread_t thread_id, u64* exitstatus);

/* ---------------------------------------------------------------------------
 * Process management
 * -----------------------------------------------------------------------*/

/* NID: 0xE6F2C1E7 */
void sys_process_exit(s32 exitcode);

/* NID: 0xE9572E5B */
s32 sys_process_getpid(void);

/* ---------------------------------------------------------------------------
 * Printf / sprintf
 * -----------------------------------------------------------------------*/

/* NID: 0x9FB6228E */
s32 _sys_printf(const char* fmt, ...);

/* NID: 0xFA7F693D */
s32 _sys_sprintf(char* buf, const char* fmt, ...);

/* ---------------------------------------------------------------------------
 * Lightweight mutex
 * -----------------------------------------------------------------------*/

/* NID: 0x1573DC3F */
s32 sys_lwmutex_create(sys_lwmutex_t_hle* lwmutex, const sys_lwmutex_attribute_t* attr);

/* NID: 0x1BC200F4 */
s32 sys_lwmutex_lock(sys_lwmutex_t_hle* lwmutex, u64 timeout);

/* NID: 0x1A1BC3B0 */
s32 sys_lwmutex_unlock(sys_lwmutex_t_hle* lwmutex);

/* NID: 0xC3B0A8A4 */
s32 sys_lwmutex_destroy(sys_lwmutex_t_hle* lwmutex);

/* ---------------------------------------------------------------------------
 * Lightweight condition variable
 * -----------------------------------------------------------------------*/

/* NID: 0xDA0EB71A */
s32 sys_lwcond_create(sys_lwcond_t_hle* lwcond, sys_lwmutex_t_hle* lwmutex,
                      const sys_lwcond_attribute_t* attr);

/* NID: 0xEF87A695 */
s32 sys_lwcond_signal(sys_lwcond_t_hle* lwcond);

/* NID: 0x2A6D9D51 */
s32 sys_lwcond_wait(sys_lwcond_t_hle* lwcond, u64 timeout);

/* NID: 0x1B434AFC */
s32 sys_lwcond_destroy(sys_lwcond_t_hle* lwcond);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_SYS_PRX_FOR_USER_H */
