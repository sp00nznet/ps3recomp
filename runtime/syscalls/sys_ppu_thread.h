/*
 * ps3recomp - PPU thread management syscalls
 *
 * Real thread management using host OS threads.
 */

#ifndef SYS_PPU_THREAD_H
#define SYS_PPU_THREAD_H

#include "lv2_syscall_table.h"
#include "../ppu/ppu_context.h"
#include "../../include/ps3emu/ps3types.h"
#include "../../include/ps3emu/error_codes.h"
#include "../memory/vm.h"

#include <stdint.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <pthread.h>
  #include <sched.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum concurrent PPU threads */
#define PPU_THREAD_MAX  64

/* Thread states */
#define PPU_THREAD_STATE_FREE       0
#define PPU_THREAD_STATE_RUNNING    1
#define PPU_THREAD_STATE_FINISHED   2
#define PPU_THREAD_STATE_DETACHED   3

/* Thread entry point type - recompiled function pointer */
typedef void (*ppu_thread_entry_fn)(ppu_context* ctx);

/* Internal thread descriptor */
typedef struct ppu_thread_info {
    int          state;
    ppu_context  ctx;
    char         name[64];
    int32_t      priority;
    int64_t      exit_status;
    int          joinable;
    uint32_t     stack_addr;   /* guest stack base */
    uint32_t     stack_size;
    uint64_t     entry_addr;   /* guest entry point */
    uint64_t     tls_addr;

#ifdef _WIN32
    HANDLE       host_thread;
    DWORD        host_tid;
#else
    pthread_t    host_thread;
#endif

    /* For join synchronization */
#ifdef _WIN32
    HANDLE       finish_event;
#else
    pthread_mutex_t finish_mutex;
    pthread_cond_t  finish_cond;
    int             finished;
#endif

} ppu_thread_info;

/* Global thread table */
extern ppu_thread_info g_ppu_threads[PPU_THREAD_MAX];

/* Stack allocator (shared) */
extern vm_stack_alloc g_vm_stack_alloc;

/* Callback to invoke recompiled code - must be set by the runtime */
extern ppu_thread_entry_fn g_ppu_thread_entry_trampoline;

/* ---------------------------------------------------------------------------
 * Syscall handlers
 * -----------------------------------------------------------------------*/
int64_t sys_ppu_thread_create(ppu_context* ctx);
int64_t sys_ppu_thread_exit(ppu_context* ctx);
int64_t sys_ppu_thread_join(ppu_context* ctx);
int64_t sys_ppu_thread_detach(ppu_context* ctx);
int64_t sys_ppu_thread_yield(ppu_context* ctx);
int64_t sys_ppu_thread_get_priority(ppu_context* ctx);
int64_t sys_ppu_thread_set_priority(ppu_context* ctx);
int64_t sys_ppu_thread_rename(ppu_context* ctx);
int64_t sys_ppu_thread_get_join_state(ppu_context* ctx);
int64_t sys_ppu_thread_get_stack_information(ppu_context* ctx);

/* Registration */
void sys_ppu_thread_init(lv2_syscall_table* tbl);

#ifdef __cplusplus
}
#endif

#endif /* SYS_PPU_THREAD_H */
